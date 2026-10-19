// Native world in the game window, runtime side (renderer step 1; docs/native_renderer.md "Native world
// in the game window"). The game side (pop_native_live.cpp, POP_NATIVE_WORLD=1) skips the opaque static
// draws, puts a marker in each frame's DMA list where its 3D starts and, at the frame's DMA kick, hands
// over the frame's native draws (PNF6). Here a worker thread draws them with the Vulkan renderer
// (tools/native, linked as PS2X_RUNNER_EXTRA_LIBS) into a 640x448 picture and its GS Z (Z24 as the
// game's VU1 programs write it), and a GS host-inject hook (runtime patch 38) replaces the marker with
// two image transfers into the frame and Z buffers the GS is drawing with. Everything the GS draws
// after the marker (characters, particles, decals, 2D, post effects) lands on top, depth-tested
// against the native world. Pixels without a native draw get the frame's clear values (colour 0, Z 0).
// Without patch 38 (the Mac `pop` runtime) this file is empty and the mode stays off.

#include "ps2_runtime.h"

#include <cstdint>
#include <vector>

#if __has_include("runtime/gs/gs_host_inject.h")
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_host_inject.h"
// the native sources, by path (this file is copied to <runtime>/ps2xRuntime/src/runner)
#include "../../../../../../tools/native/src/frame_loader.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace pop_native_world
{
    namespace
    {
        constexpr uint32_t kWidth = 640, kHeight = 448;
        constexpr uint32_t kMagic = 0x504E5744u; // "PNWD", as pop_native_live.cpp writes it

        struct Picture
        {
            uint32_t seq = 0;
            bool ok = false;
            std::vector<uint8_t> rgba;  // CT32 byte order (R, G, B, A)
            std::vector<uint32_t> gsZ;  // per pixel GS Z (24 bits)
        };

        std::mutex g_mutex;
        std::condition_variable g_wake, g_ready;
        std::deque<std::pair<uint32_t, std::vector<uint8_t>>> g_queue; // frames to draw
        std::deque<Picture> g_done;                                    // drawn, oldest first
        bool g_started = false, g_initDone = false, g_failed = false;

        struct Stats
        {
            uint64_t drawn = 0, injected = 0, missed = 0, wrongTarget = 0;
            double drawMs = 0, waitMs = 0, loadMs = 0, uploadMs = 0, renderMs = 0; // worker: total, read, upload, draw + readback
        } g_stats;

        void worker()
        {
            pop_native::Renderer renderer;
            pop_native::Options options;
            options.gameOrder = true; // the game's draw order
            for (float &c : options.clearColour)
                c = 0.0f; // the frame's clear (black, alpha 0, Z 0) where nothing native is drawn
            std::string error;
            const bool ok = renderer.init(options, error);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_initDone = true;
                g_failed = !ok;
                g_ready.notify_all();
            }
            if (!ok)
            {
                std::fprintf(stderr, "[pop:world] Vulkan renderer: %s; the native world is off\n", error.c_str());
                return;
            }
            std::fprintf(stderr, "[pop:world] drawing on %s\n", renderer.deviceName().c_str());
            pop_native::FrameLoader loader;
            for (;;)
            {
                std::pair<uint32_t, std::vector<uint8_t>> job;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_wake.wait(lock, [] { return !g_queue.empty(); });
                    job = std::move(g_queue.front());
                    g_queue.pop_front();
                }
                const auto t0 = std::chrono::steady_clock::now();
                Picture pic;
                pic.seq = job.first;
                renderer.reset(true); // the textures stay (the frames send each one once)
                std::vector<pop_native::Draw> draws;
                pop_native::FrameExtras extras;
                float clip[16];
                std::istringstream in(std::string(reinterpret_cast<const char *>(job.second.data()), job.second.size()), std::ios::binary);
                error.clear();
                pic.ok = loader.load(renderer, in, "world", clip, draws, extras, false);
                const auto t1 = std::chrono::steady_clock::now();
                pic.ok = pic.ok && renderer.upload(error);
                const auto t2 = std::chrono::steady_clock::now();
                pic.ok = pic.ok && renderer.renderOffscreen(kWidth, kHeight, clip, draws, pic.rgba, error, nullptr, &pic.gsZ);
                const auto t3 = std::chrono::steady_clock::now();
                g_stats.loadMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
                g_stats.uploadMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
                g_stats.renderMs += std::chrono::duration<double, std::milli>(t3 - t2).count();
                if (!pic.ok)
                {
                    static int logged = 0;
                    if (logged++ < 8)
                        std::fprintf(stderr, "[pop:world] frame %u not drawn: %s\n", pic.seq, error.empty() ? "unreadable" : error.c_str());
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stats.drawMs += ms;
                ++g_stats.drawn;
                g_done.push_back(std::move(pic));
                while (g_done.size() > 4)
                    g_done.pop_front(); // frames whose marker never came (or came first)
                g_ready.notify_all();
            }
        }

        // The GS hook: the marker is a 2-qword PACKED GIF packet (NLOOP 1, EOP, NREG 1, A+D) writing
        // register 0x7F with "PNWD" and the frame number.
        bool hook(GS &gs, const uint8_t *data, uint32_t sizeBytes, const GSHostInject::Targets &t)
        {
            if (sizeBytes != 32)
                return false;
            uint64_t q[4];
            std::memcpy(q, data, sizeof q);
            if (q[0] != 0x1000000000008001ull || (q[1] & 0xF) != 0xE || (q[3] & 0xFF) != 0x7F || (q[2] >> 32) != kMagic)
                return false;
            const uint32_t seq = uint32_t(q[2]);
            Picture pic;
            bool found = false;
            {
                // the worker started on this frame at its DMA kick, so it is at most one draw away
                const auto t0 = std::chrono::steady_clock::now();
                std::unique_lock<std::mutex> lock(g_mutex);
                g_ready.wait_for(lock, std::chrono::milliseconds(250), [&] {
                    if (g_failed)
                        return true;
                    for (const Picture &p : g_done)
                        if (p.seq == seq)
                            return true;
                    return false;
                });
                for (auto it = g_done.begin(); it != g_done.end(); ++it)
                    if (it->seq == seq)
                    {
                        pic = std::move(*it);
                        g_done.erase(g_done.begin(), it + 1);
                        found = true;
                        break;
                    }
                g_stats.waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
            if (!found || !pic.ok)
            {
                if (g_stats.missed++ < 8)
                    std::fprintf(stderr, "[pop:world] frame %u: no native picture (its static world is missing)\n", seq);
                return true;
            }
            // the buffers the frame's 3D draws into: FRAME PSMCT32 640 wide, ZBUF PSMZ24 or PSMZ32
            if (t.fpsm != 0 || t.fbw != kWidth / 64 || (t.zpsm != 0x30 && t.zpsm != 0x31) || t.zmask)
            {
                if (g_stats.wrongTarget++ < 8)
                    std::fprintf(stderr, "[pop:world] frame %u: unexpected buffers (FRAME fbp %x fbw %u psm %x, ZBUF zbp %x psm %x mask %d)\n", seq,
                                 t.fbp, t.fbw, t.fpsm, t.zbp, t.zpsm, int(t.zmask));
                return true;
            }
            // BITBLTBUF: DBP (64-word blocks) bits 32-45, DBW bits 48-53, DPSM bits 56-61; TRXREG: RRW, RRH
            // at bit 32; TRXPOS 0 (destination 0, 0), TRXDIR 0 (host -> local)
            const uint64_t trxreg = uint64_t(kWidth) | (uint64_t(kHeight) << 32);
            const uint64_t frame = (uint64_t(t.fbp * 32u) << 32) | (uint64_t(t.fbw) << 48);
            gs.uploadImageNative(frame, 0, trxreg, 0, pic.rgba.data(), uint32_t(pic.rgba.size()));
            const bool z24 = t.zpsm == 0x31;
            std::vector<uint8_t> z(size_t(kWidth) * kHeight * (z24 ? 3 : 4));
            for (size_t i = 0, o = 0; i < pic.gsZ.size(); ++i)
            {
                const uint32_t v = pic.gsZ[i];
                z[o++] = uint8_t(v);
                z[o++] = uint8_t(v >> 8);
                z[o++] = uint8_t(v >> 16);
                if (!z24)
                    z[o++] = 0;
            }
            const uint64_t zbuf = (uint64_t(t.zbp * 32u) << 32) | (uint64_t(kWidth / 64) << 48) | (uint64_t(t.zpsm) << 56);
            gs.uploadImageNative(zbuf, 0, trxreg, 0, z.data(), uint32_t(z.size()));
            if (++g_stats.injected % 600 == 0)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                std::fprintf(stderr, "[pop:world] %llu frames injected, %llu missed, %llu unexpected buffers; per frame: worker %.2f ms (read %.2f, "
                                     "upload %.2f, draw %.2f), GS thread waited %.2f ms\n",
                             (unsigned long long)g_stats.injected, (unsigned long long)g_stats.missed,
                             (unsigned long long)g_stats.wrongTarget, g_stats.drawMs / double(g_stats.drawn), g_stats.loadMs / double(g_stats.drawn),
                             g_stats.uploadMs / double(g_stats.drawn), g_stats.renderMs / double(g_stats.drawn),
                             g_stats.waitMs / double(g_stats.injected + g_stats.missed));
            }
            return true;
        }

    }

    // starts the worker and waits for its renderer: the game skips draws only if they can be drawn
    bool active()
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_started)
        {
            g_started = true;
            std::thread(worker).detach();
        }
        g_ready.wait_for(lock, std::chrono::seconds(20), [] { return g_initDone; });
        if (!g_initDone || g_failed)
            return false;
        GSHostInject::setHook(hook);
        return true;
    }

    void submitFrame(uint32_t seq, std::vector<uint8_t> &&pnf)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_failed)
                return;
            g_queue.emplace_back(seq, std::move(pnf));
        }
        g_wake.notify_one();
    }
}

#else // a runtime without the GS host-inject hook: the native world cannot be shown

namespace pop_native_world
{
    bool active() { return false; }
    void submitFrame(uint32_t, std::vector<uint8_t> &&) {}
}

#endif
