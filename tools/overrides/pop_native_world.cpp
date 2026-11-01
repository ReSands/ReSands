// Native world in the game window, runtime side (renderer step 1; docs/native_renderer.md "Native world
// in the game window"). The game side (pop_native_live.cpp, POP_NATIVE_WORLD=1) skips the opaque static
// draws, puts a marker in each frame's DMA list where its 3D starts and, at the frame's DMA kick, hands
// over the frame's native draws (PNF7: a batch's geometry only the first time, then the renderer keeps
// it). Here a worker thread draws them with the Vulkan renderer (tools/native, linked as
// PS2X_RUNNER_EXTRA_LIBS) into a 640x448 picture and its GS Z (Z24 as the
// game's VU1 programs write it), and a GS host-inject hook (runtime patch 38) replaces the marker with
// two image transfers into the frame and Z buffers the GS is drawing with. Everything the GS draws
// after the marker (characters, particles, decals, 2D, post effects) lands on top, depth-tested
// against the native world. Pixels without a native draw get the frame's clear values (colour 0, Z 0).
// Without patch 38 (the Mac `pop` runtime) this file is empty and the mode stays off.

#include "ps2_runtime.h"

#include <cstdint>
#include <string>
#include <vector>

#if __has_include("runtime/gs/gs_host_inject.h")
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_host_inject.h"
#ifdef GS_HOST_PRESENT
#include "runtime/gs/gs_cpu_backend.h"
#endif
// the native sources, by path (this file is copied to <runtime>/ps2xRuntime/src/runner)
#include "../../../../../../tools/native/src/frame_loader.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

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
            uint32_t draws = 0;
            std::vector<uint8_t> rgba;  // CT32 byte order (R, G, B, A)
            std::vector<uint32_t> gsZ;  // per pixel GS Z (24 bits)
            std::string gs;             // the frame's targets at its marker (load-flash capture only)
        };

        std::mutex g_mutex;
        std::condition_variable g_wake, g_ready;
        struct Job
        {
            uint32_t seq = 0, generation = 0;
            std::vector<uint8_t> pnf;
        };
        std::deque<Job> g_queue; // frames to draw
        std::deque<Picture> g_done;                                    // drawn, oldest first
        // batches the renderer keeps (PNF7 keys sent with their geometry), per generation: when the kept meshes
        // outgrow the budget, a new generation starts (g_sent cleared, so the frames written from then on send all
        // their geometry) and the worker drops the kept meshes just before the first frame of it. Dropping them in
        // the middle of a frame (the renderer's own drop) left that frame's kept draws without geometry: a black
        // world frame, the "random flashes" of 24 Oct (glitch captures 200042 / 200541).
        std::mutex g_sentMutex;
        std::unordered_set<uint64_t> g_sent;
        uint32_t g_sentGeneration = 1;
        void newGeneration(const char *why)
        {
            std::lock_guard<std::mutex> lock(g_sentMutex);
            g_sent.clear();
            ++g_sentGeneration;
            std::fprintf(stderr, "[pop:world] kept meshes: generation %u (%s)\n", g_sentGeneration, why);
        }
        bool g_started = false, g_initDone = false, g_failed = false;

        struct Stats
        {
            uint64_t drawn = 0, injected = 0, missed = 0, wrongTarget = 0;
            size_t textures = 0, textureBytes = 0; // the worker's renderer after its last upload (audit 13: no budget)
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
            renderer.autoDropKept = false; // dropped here, between generations
            if (const char *mb = std::getenv("POP_NATIVE_WORLD_KEPT_MB")) // debug: a smaller budget (tests the drop)
                renderer.keptBudgetBytes = size_t(std::atoi(mb)) << 20;
            uint32_t dropAt = 0;           // the generation whose first frame drops the kept meshes (0: none due)
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_wake.wait(lock, [] { return !g_queue.empty(); });
                    job = std::move(g_queue.front());
                    g_queue.pop_front();
                }
                const auto t0 = std::chrono::steady_clock::now();
                Picture pic;
                pic.seq = job.seq;
                if (dropAt && int32_t(job.generation - dropAt) >= 0)
                {
                    std::fprintf(stderr, "[pop:world] frame %u: kept meshes dropped (%zu MB)\n", job.seq, renderer.keptBytes() >> 20);
                    renderer.dropKept();
                    dropAt = 0;
                }
                renderer.reset(true); // the textures stay (the frames send each one once)
                std::vector<pop_native::Draw> draws;
                pop_native::FrameExtras extras;
                float clip[16];
                std::istringstream in(std::string(reinterpret_cast<const char *>(job.pnf.data()), job.pnf.size()), std::ios::binary);
                error.clear();
                pic.ok = loader.load(renderer, in, "world", clip, draws, extras, false);
                renderer.setPostEffects(pop_native::postEffectsOf(extras)); // the effects the game skipped on the GS (POP_NATIVE_POST)
                const auto t1 = std::chrono::steady_clock::now();
                pic.ok = pic.ok && renderer.upload(error);
                if (loader.keptMissing)
                {
                    // a key the renderer does not have (should not happen any more): the writer sends everything again
                    std::fprintf(stderr, "[pop:world] frame %u: %zu kept batches missing\n", job.seq, loader.keptMissing);
                    loader.keptMissing = 0;
                    newGeneration("batches missing");
                }
                else if (!dropAt && renderer.keptBytes() > renderer.keptBudgetBytes)
                {
                    newGeneration("over budget");
                    std::lock_guard<std::mutex> lock(g_sentMutex);
                    dropAt = g_sentGeneration;
                }
                const auto t2 = std::chrono::steady_clock::now();
                pic.draws = uint32_t(draws.size());
                pic.ok = pic.ok && renderer.renderOffscreen(kWidth, kHeight, clip, draws, pic.rgba, error, nullptr, &pic.gsZ);
                const auto t3 = std::chrono::steady_clock::now();
                if (!pic.ok)
                {
                    static int logged = 0;
                    if (logged++ < 8)
                        std::fprintf(stderr, "[pop:world] frame %u not drawn: %s\n", pic.seq, error.empty() ? "unreadable" : error.c_str());
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stats.drawMs += ms;
                g_stats.loadMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
                g_stats.uploadMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
                g_stats.renderMs += std::chrono::duration<double, std::milli>(t3 - t2).count();
                ++g_stats.drawn;
                g_stats.textures = renderer.textureCount();
                g_stats.textureBytes = renderer.textureBytes();
                g_done.push_back(std::move(pic));
                while (g_done.size() > 4)
                    g_done.pop_front(); // frames whose marker never came (or came first)
                g_ready.notify_all();
            }
        }

        // Load-flash capture (POP_NATIVE_WORLD_DUMP=<dir>): after a pause of >= 300 ms between two markers
        // or the first frame with native draws after one without (a load), the 3 frames before it and the 12 after it are written to <dir> as PPM (colour of the
        // injected picture), with a line each in <dir>/index.txt: frame, pause, draws, coverage (alpha != 0),
        // mean colour, or "MISSED" (no picture: the frame keeps whatever was in its buffers). For the
        // full-frame flash after the loading glyph (docs/native_renderer.md "Load flash is ours").
        struct Dump
        {
            std::string dir;
            int loads = 0, left = 0;
            uint32_t lastDraws = 0;
            std::chrono::steady_clock::time_point last{};
            std::deque<Picture> before; // the last 3 frames, written when a pause is seen
        } g_dump;

        void dumpPicture(const Picture &pic, bool found, double gapMs, const char *when)
        {
            char name[64];
            std::snprintf(name, sizeof name, "load%02d_%s_%06u.ppm", g_dump.loads, when, pic.seq);
            uint64_t covered = 0, sum[3] = {0, 0, 0};
            if (found && pic.ok && pic.rgba.size() == size_t(kWidth) * kHeight * 4)
            {
                if (FILE *f = std::fopen((g_dump.dir + "/" + name).c_str(), "wb"))
                {
                    std::fprintf(f, "P6\n%u %u\n255\n", kWidth, kHeight);
                    std::vector<uint8_t> rgb(size_t(kWidth) * kHeight * 3);
                    for (size_t i = 0; i < size_t(kWidth) * kHeight; ++i)
                    {
                        for (int c = 0; c < 3; ++c)
                            sum[c] += rgb[i * 3 + c] = pic.rgba[i * 4 + c];
                        covered += pic.rgba[i * 4 + 3] != 0;
                    }
                    std::fwrite(rgb.data(), 1, rgb.size(), f);
                    std::fclose(f);
                }
            }
            if (FILE *f = std::fopen((g_dump.dir + "/index.txt").c_str(), "a"))
            {
                const double n = double(kWidth) * kHeight;
                if (found && pic.ok)
                    std::fprintf(f, "load %02d %-6s frame %u pause %.0f ms: %u draws, coverage %.1f%%, mean %.1f %.1f %.1f  %s  %s\n", g_dump.loads, when,
                                 pic.seq, gapMs, pic.draws, 100.0 * covered / n, sum[0] / n, sum[1] / n, sum[2] / n, name, pic.gs.c_str());
                else
                    std::fprintf(f, "load %02d %-6s frame %u pause %.0f ms: MISSED (%s)\n", g_dump.loads, when, pic.seq, gapMs,
                                 found ? "not drawn" : "no picture");
                std::fclose(f);
            }
        }

        // called for every marker; `pic` is kept for the next frames' "before" list
        void dumpFrame(Picture &pic, bool found)
        {
            static const char *dir = std::getenv("POP_NATIVE_WORLD_DUMP");
            if (!dir || !*dir)
                return;
            if (g_dump.dir.empty())
                g_dump.dir = dir;
            const auto now = std::chrono::steady_clock::now();
            const double gapMs = g_dump.last.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - g_dump.last).count() : 0.0;
            g_dump.last = now;
            const uint32_t draws = found && pic.ok ? pic.draws : 0;
            const bool worldBack = draws > 0 && g_dump.lastDraws == 0;
            g_dump.lastDraws = draws;
            if ((gapMs >= 300.0 || worldBack) && g_dump.left == 0 && g_dump.loads < 30)
            {
                ++g_dump.loads;
                g_dump.left = 12;
                for (const Picture &b : g_dump.before)
                    dumpPicture(b, true, 0.0, "before");
                std::fprintf(stderr, "[pop:world] load %d (pause %.0f ms, world back %d): dumping frames to %s\n", g_dump.loads, gapMs, int(worldBack), dir);
            }
            if (g_dump.left > 0)
            {
                --g_dump.left;
                dumpPicture(pic, found, gapMs, "after");
            }
            pic.gsZ.clear();
            g_dump.before.push_back(found ? std::move(pic) : Picture{});
            if (!found)
                g_dump.before.back().seq = pic.seq;
            while (g_dump.before.size() > 3)
                g_dump.before.pop_front();
        }

        // Glitch capture (24 Oct; docs/native_bugs.md "Tooling"): the last kGlitchFrames frames, each with its
        // injected picture at half size (RGB) and what the game side noted at its DMA kick (noteFrame), written
        // by dumpRecent at every kept quick-save as <save>.glitch (read with tools/glitchview.py). The picture is
        // the native world only: the GS draws characters, effects and 2D on top after the marker.
        // 600 frames = 10 s at 60 fps (~130 MB): the kept copy of a quick-save is made 1-2 s after the press
        // (keepQuickSaves checks once a second), so this leaves ~8 s before it
        constexpr uint32_t kGlitchFrames = 600, kGlitchW = kWidth / 2, kGlitchH = kHeight / 2;
        const GSRegisters *g_display = nullptr; // DISPFB1: the buffer on screen
        struct FrameNote
        {
            uint32_t seq = 0;
            std::string text; // one line from pop_native_live.cpp (camera, fog zones, purity)
        };
        struct GlitchFrame
        {
            uint32_t seq = 0;
            int state = 0; // 0 injected, 1 no picture, 2 not drawn, 3 unexpected buffers
            uint32_t draws = 0, fbp = 0, target = 0, shown = 0;
            double atMs = 0;           // marker time since the first frame
            FrameNote note;            // the game side's record of this frame (seq 0: none came)
            std::vector<uint8_t> rgb;  // kGlitchW x kGlitchH, empty without a picture
        };
        std::mutex g_glitchMutex;
        std::deque<GlitchFrame> g_glitch;
        std::deque<FrameNote> g_notes; // noted at the kick, not yet matched with their marker
        const auto g_glitchStart = std::chrono::steady_clock::now();

        void glitchRecord(const Picture &pic, int state, uint32_t fbp, uint32_t target)
        {
            GlitchFrame g;
            g.seq = pic.seq;
            g.state = state;
            g.draws = pic.draws;
            g.fbp = fbp;
            g.target = target;
            g.shown = g_display ? uint32_t(g_display->dispfb1) & 0x1FFu : ~0u;
            g.atMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_glitchStart).count();
            if (state == 0 && pic.rgba.size() == size_t(kWidth) * kHeight * 4)
            {
                g.rgb.resize(size_t(kGlitchW) * kGlitchH * 3);
                for (uint32_t y = 0; y < kGlitchH; ++y)
                    for (uint32_t x = 0; x < kGlitchW; ++x)
                        for (int c = 0; c < 3; ++c)
                        {
                            const size_t i = (size_t(2 * y) * kWidth + 2 * x) * 4 + c;
                            g.rgb[(size_t(y) * kGlitchW + x) * 3 + c] =
                                uint8_t((pic.rgba[i] + pic.rgba[i + 4] + pic.rgba[i + kWidth * 4] + pic.rgba[i + kWidth * 4 + 4] + 2) / 4);
                        }
            }
            std::lock_guard<std::mutex> lock(g_glitchMutex);
            while (!g_notes.empty() && int32_t(g_notes.front().seq - g.seq) < 0)
                g_notes.pop_front(); // frames whose marker never came
            if (!g_notes.empty() && g_notes.front().seq == g.seq)
            {
                g.note = g_notes.front();
                g_notes.pop_front();
            }
            g_glitch.push_back(std::move(g));
            while (g_glitch.size() > kGlitchFrames)
                g_glitch.pop_front();
        }

        // The buffer the frame draws into. The marker takes FRAME from the GS state, but the first frame after a
        // load has no clear sprite before its 3D, so FRAME is still the loading screen's last target, which is
        // the buffer on screen: the picture went straight onto the display for one frame (the load flash; GS
        // traces of 23 Oct, docs/native_renderer.md "Load flash: what the GS traces show"). The game never draws
        // into the displayed buffer, so then the target is the other buffer of the pair seen in play.
        uint32_t targetFbp(uint32_t fbp, uint32_t seq)
        {
            static uint32_t pair[2] = {~0u, ~0u};
            if (pair[0] != fbp && pair[1] != fbp)
            {
                pair[1] = pair[0];
                pair[0] = fbp;
            }
            const uint32_t shown = g_display ? uint32_t(g_display->dispfb1) & 0x1FFu : ~0u;
            const uint32_t other = pair[0] == fbp ? pair[1] : pair[0];
            if (fbp != shown || other == ~0u)
                return fbp;
            static uint32_t logged = 0;
            if (logged++ < 16)
                std::fprintf(stderr, "[pop:world] frame %u: FRAME fbp %x is on screen, picture into fbp %x\n", seq, fbp, other);
            return other;
        }

        // GS census (POP_NATIVE_GS_CENSUS=<n>; 0.2 step 1, docs/roadmap.md): every draw the GS still gets, whichever path
        // sent it, counted per marker-to-marker interval and keyed by PRIM and FRAME fbp. An interval holds frame N's tail
        // after its marker and frame N+1's start before its own (the clear lands there: a constant baseline). Every n-th
        // interval (1: 3000) is printed in draw order as runs (prim, fbp, FBMSK, kicks, empty sprites, box in pixels
        // before XYOFFSET), so what lies after the marker can be told from the next frame's start.
        struct CensusRun
        {
            uint32_t prim, fbp, fbmsk, kicks = 0, empty = 0, x0 = ~0u, y0 = ~0u, x1 = 0, y1 = 0;
        };
        struct Census
        {
            uint32_t prim = 0, seq = 0, kicksSince = 0, vtx = 0, lastX = 0, lastY = 0;
            uint64_t intervals = 0, withDraws = 0, images = 0;
            std::map<uint64_t, std::pair<uint64_t, uint64_t>> bySig; // prim | fbp << 16 -> vertex kicks, of them in zero-area sprites
            std::vector<CensusRun> runs;                              // this interval, while it is one to print
        } g_census;
        const uint32_t g_censusEvery = [] {
            const char *e = std::getenv("POP_NATIVE_GS_CENSUS");
            const uint32_t n = e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0;
            return n == 1 ? 3000u : n;
        }();
        const bool g_censusOn = g_censusEvery != 0;

        // one vertex kick (XYZ2 / XYZF2) at x, y (12.4), filed under the PRIM in effect then
        void censusKick(uint32_t fbp, uint32_t fbmsk, uint32_t x, uint32_t y)
        {
            Census &c = g_census;
            // a sprite whose two corners share an x or a y covers nothing (the native letters' stand-ins)
            const bool empty = (c.prim & 7) == 6 && (c.vtx++ & 1) && (x == c.lastX || y == c.lastY);
            c.lastX = x;
            c.lastY = y;
            auto &n = c.bySig[uint64_t(c.prim) | uint64_t(fbp) << 16];
            static uint32_t logged = 0;
            if (!n.first && logged++ < 64)
                std::fprintf(stderr, "[pop:census] new: prim %03x fbp %x, %u kicks after marker %u\n", c.prim, fbp, c.kicksSince, c.seq);
            ++n.first;
            n.second += empty ? 2 : 0;
            ++c.kicksSince;
            if (c.intervals % g_censusEvery != 0 || !c.seq)
                return;
            if (c.runs.empty() || c.runs.back().prim != c.prim || c.runs.back().fbp != fbp || c.runs.back().fbmsk != fbmsk)
                c.runs.push_back({c.prim, fbp, fbmsk});
            CensusRun &r = c.runs.back();
            ++r.kicks;
            r.empty += empty ? 2 : 0;
            r.x0 = std::min(r.x0, x >> 4), r.y0 = std::min(r.y0, y >> 4), r.x1 = std::max(r.x1, x >> 4), r.y1 = std::max(r.y1, y >> 4);
        }

        // walks one GIF packet's tags; tracks PRIM across packets as the GS does
        void censusPacket(const uint8_t *data, uint32_t sizeBytes, uint32_t fbp, uint32_t fbmsk)
        {
            Census &c = g_census;
            for (uint32_t at = 0; at + 16 <= sizeBytes;)
            {
                uint64_t tag[2];
                std::memcpy(tag, data + at, 16);
                at += 16;
                const uint32_t nloop = tag[0] & 0x7FFF, flg = (tag[0] >> 58) & 3, nreg = (tag[0] >> 60) ? uint32_t(tag[0] >> 60) : 16u;
                if (flg < 2 && (tag[0] >> 46) & 1) // PRE
                    c.prim = (tag[0] >> 47) & 0x7FF, c.vtx = 0;
                if (flg == 0) // PACKED
                    for (uint32_t i = 0; i < nloop * nreg && at + 16 <= sizeBytes; ++i, at += 16)
                    {
                        uint64_t q[2];
                        std::memcpy(q, data + at, 16);
                        uint32_t reg = (tag[1] >> (4 * (i % nreg))) & 0xF;
                        if (reg == 0xE) // A+D: the address names the register (0x0C/0x0D XYZ3 draw nothing)
                            reg = (q[1] & 0xFF) <= 0x05 ? uint32_t(q[1] & 0xFF) : 0xF;
                        else if ((reg == 0x4 || reg == 0x5) && (q[1] >> 47) & 1) // ADC: no kick
                            reg = 0xF;
                        if (reg == 0x0)
                            c.prim = q[0] & 0x7FF, c.vtx = 0;
                        else if ((reg == 0x4 || reg == 0x5) && (tag[1] >> (4 * (i % nreg)) & 0xF) == 0xE) // A+D: X 0-15, Y 16-31
                            censusKick(fbp, fbmsk, q[0] & 0xFFFF, (q[0] >> 16) & 0xFFFF);
                        else if (reg == 0x4 || reg == 0x5) // PACKED: X 0-15, Y 32-47
                            censusKick(fbp, fbmsk, q[0] & 0xFFFF, (q[0] >> 32) & 0xFFFF);
                    }
                else if (flg == 1) // REGLIST: 64-bit words, padded to a qword
                {
                    const uint32_t words = nloop * nreg;
                    for (uint32_t i = 0; i < words && at + 8 * i + 8 <= sizeBytes; ++i)
                    {
                        const uint32_t reg = (tag[1] >> (4 * (i % nreg))) & 0xF;
                        uint64_t v;
                        std::memcpy(&v, data + at + 8 * i, 8);
                        if (reg == 0x0)
                            c.prim = v & 0x7FF, c.vtx = 0;
                        else if (reg == 0x4 || reg == 0x5)
                            censusKick(fbp, fbmsk, v & 0xFFFF, (v >> 16) & 0xFFFF);
                    }
                    at += ((words + 1) / 2) * 16;
                }
                else // IMAGE: a texture or CLUT transfer, nothing drawn
                {
                    c.images += nloop != 0;
                    at += nloop * 16;
                }
            }
        }

        // at each marker: closes the interval; every 600 a summary (top 10 signatures by kicks)
        void censusMarker(uint32_t seq)
        {
            Census &c = g_census;
            if (!c.runs.empty())
            {
                std::string text;
                for (const CensusRun &r : c.runs)
                {
                    char s[112];
                    std::snprintf(s, sizeof s, " | %03x fbp %x msk %08x: %u kicks (%u empty) %u,%u-%u,%u", r.prim, r.fbp, r.fbmsk, r.kicks, r.empty,
                                  r.x0, r.y0, r.x1, r.y1);
                    text += s;
                }
                std::fprintf(stderr, "[pop:census] interval after marker %u:%s\n", c.seq, text.c_str());
                c.runs.clear();
            }
            if (c.seq)
            {
                ++c.intervals;
                c.withDraws += c.kicksSince != 0;
            }
            c.seq = seq;
            c.kicksSince = 0;
            if (c.intervals == 0 || c.intervals % 600)
                return;
            std::vector<std::pair<uint64_t, uint64_t>> by;
            for (const auto &[sig, n] : c.bySig)
                by.emplace_back(n.first, sig);
            std::sort(by.rbegin(), by.rend());
            std::string top;
            for (size_t i = 0; i < by.size() && i < 10; ++i)
            {
                char s[80];
                std::snprintf(s, sizeof s, "%sprim %03x fbp %x %llu (%llu empty)", i ? ", " : "", uint32_t(by[i].second & 0x7FF),
                              uint32_t(by[i].second >> 16), (unsigned long long)by[i].first, (unsigned long long)c.bySig[by[i].second].second);
                top += s;
            }
            std::fprintf(stderr, "[pop:census] intervals %llu, with GS draws %llu, image transfers %llu; kicks by prim/fbp: %s\n",
                         (unsigned long long)c.intervals, (unsigned long long)c.withDraws, (unsigned long long)c.images, top.c_str());
        }

#ifdef GS_HOST_PRESENT
        // Native presenter (POP_NATIVE_PRESENT=1; 0.2 step 2, docs/gs_off.md): a GS memory of our own holding only the
        // native pictures, each in the buffer its frame draws into; at each presentation the runtime's own CPU display
        // merge (PMODE, DISPFB1/2, DISPLAY1/2: the game's line blend) runs on it instead of on the GS's memory (runtime
        // patch 45). The GS's picture stays on screen until the first native picture and whenever none came for 0.5 s:
        // after a save-state load the saved screen is only in the GS's memory (192103: the loading screen, not redrawn
        // until the movie; ours showed the title's last box).
        bool envOn(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e && *e != '0';
        }
        // GS off (POP_GS_OFF=1; 0.2 step 3): the GS gets no draws (runtime patch 46), so the native presenter shows the
        // picture. While no native picture comes (cold boot, save-state loads) the GS's memory is shown: transfers and
        // loaded states still reach it, its draws do not.
        const bool g_gsOff = envOn("POP_GS_OFF");
        const bool g_presentOn = g_gsOff || envOn("POP_NATIVE_PRESENT");
        std::atomic<int64_t> g_presentAt{0}; // steady_clock ticks of the latest native picture, 0 = none yet
        GSCpuBackend &presentGs()
        {
            static std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE);
            static GSCpuBackend gs;
            static const bool init = (gs.Initialize(vram.data(), uint32_t(vram.size())), true);
            (void)init;
            return gs;
        }
        void presentUpload(uint32_t fbp, uint32_t fbw, const std::vector<uint8_t> &rgba)
        {
            GSTransferCommand c;
            c.bitbltbuf.dbp = fbp * 32u;
            c.bitbltbuf.dbw = uint8_t(fbw);
            c.bitbltbuf.dpsm = 0; // PSMCT32
            c.trxreg.rrw = kWidth;
            c.trxreg.rrh = kHeight;
            c.direction = 0; // host -> local
            presentGs().BeginTransfer(c);
            presentGs().UploadImage(rgba.data(), uint32_t(rgba.size()));
            g_presentAt.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
        }
        bool presentHook(const GSPresentationRequest &request, PresentationFrame &frame)
        {
            const int64_t at = g_presentAt.load(std::memory_order_acquire);
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            if (!at || now - std::chrono::steady_clock::duration(at) > std::chrono::milliseconds(500))
                return false;
            frame = presentGs().Present(request);
            return bool(frame);
        }
#endif

        // The GS hook: the marker is a 2-qword PACKED GIF packet (NLOOP 1, EOP, NREG 1, A+D) writing
        // register 0x7F with "PNWD" and the frame number.
        bool hook(GS &gs, const uint8_t *data, uint32_t sizeBytes, const GSHostInject::Targets &t)
        {
            uint64_t q[4] = {};
            if (sizeBytes == 32)
                std::memcpy(q, data, sizeof q);
            const bool marker = sizeBytes == 32 && q[0] == 0x1000000000008001ull && (q[1] & 0xF) == 0xE && (q[3] & 0xFF) == 0x7F &&
                                (q[2] >> 32) == kMagic;
            if (g_censusOn)
                marker ? censusMarker(uint32_t(q[2])) : censusPacket(data, sizeBytes, t.fbp, t.fbmsk);
            if (!marker)
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
            if (!found)
                pic.seq = seq;
            {
                // the buffers at the marker (load-flash capture; the presented picture is not readable here: the
                // play build presents on the GPU, GSSharedPresent, 23 Oct)
                static const bool dumping = std::getenv("POP_NATIVE_WORLD_DUMP") != nullptr;
                if (dumping)
                {
                    char gsText[96];
                    std::snprintf(gsText, sizeof gsText, "FRAME fbp %x fbmsk %08x, ZBUF zbp %x psm %x zmsk %d", t.fbp, t.fbmsk, t.zbp, t.zpsm, int(t.zmask));
                    pic.gs = gsText;
                }
            }
            if (!found || !pic.ok)
            {
                glitchRecord(pic, found ? 2 : 1, t.fbp, t.fbp);
                dumpFrame(pic, found);
                if (g_stats.missed++ < 8)
                    std::fprintf(stderr, "[pop:world] frame %u: no native picture (its static world is missing)\n", seq);
                return true;
            }
            // the buffers the frame's 3D draws into: FRAME PSMCT32 640 wide, ZBUF PSMZ24 or PSMZ32. ZBUF's
            // write mask is not checked: in some frames (after loading, the fixed vault camera; saves
            // 171143/171355 of 22 Oct) the last state before the marker has ZMSK 1 with no draw after it, and
            // the game sets ZMSK 0 right after the marker. Refusing those frames lost their whole static world.
            if (t.fpsm != 0 || t.fbw != kWidth / 64 || (t.zpsm != 0x30 && t.zpsm != 0x31))
            {
                if (g_stats.wrongTarget++ < 8)
                    std::fprintf(stderr, "[pop:world] frame %u: unexpected buffers (FRAME fbp %x fbw %u psm %x, ZBUF zbp %x psm %x mask %d)\n", seq,
                                 t.fbp, t.fbw, t.fpsm, t.zbp, t.zpsm, int(t.zmask));
                glitchRecord(pic, 3, t.fbp, t.fbp);
                return true;
            }
            // BITBLTBUF: DBP (64-word blocks) bits 32-45, DBW bits 48-53, DPSM bits 56-61; TRXREG: RRW, RRH
            // at bit 32; TRXPOS 0 (destination 0, 0), TRXDIR 0 (host -> local)
            const uint64_t trxreg = uint64_t(kWidth) | (uint64_t(kHeight) << 32);
            const uint32_t target = targetFbp(t.fbp, seq);
            glitchRecord(pic, 0, t.fbp, target);
            const uint64_t frame = (uint64_t(target * 32u) << 32) | (uint64_t(t.fbw) << 48);
            gs.uploadImageNative(frame, 0, trxreg, 0, pic.rgba.data(), uint32_t(pic.rgba.size()));
#ifdef GS_HOST_PRESENT
            if (g_presentOn)
                presentUpload(target, t.fbw, pic.rgba);
#endif
            const bool z24 = t.zpsm == 0x31;
            // Depth bias (POP_NATIVE_WORLD_ZBIAS, Z24 units, default 128): the native Z lies slightly nearer
            // than the VU1 programs' Z on 27% of the world's pixels (19 Oct saves: p50 19, p99 106, p99.9 442
            // units), so the GS's coplanar second passes over it (shadows, decals; all GEQUAL) failed there
            // and flickered (lever shadow, save 172041 of 22 Oct). Pushing the world back covers ~99%.
            static const uint32_t zBias = [] {
                const char *e = std::getenv("POP_NATIVE_WORLD_ZBIAS");
                return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 128u;
            }();
            std::vector<uint8_t> z(size_t(kWidth) * kHeight * (z24 ? 3 : 4));
            for (size_t i = 0, o = 0; i < pic.gsZ.size(); ++i)
            {
                const uint32_t raw = pic.gsZ[i];
                const uint32_t v = raw > zBias ? raw - zBias : (raw ? 1u : 0u); // 0 stays the clear
                z[o++] = uint8_t(v);
                z[o++] = uint8_t(v >> 8);
                z[o++] = uint8_t(v >> 16);
                if (!z24)
                    z[o++] = 0;
            }
            const uint64_t zbuf = (uint64_t(t.zbp * 32u) << 32) | (uint64_t(kWidth / 64) << 48) | (uint64_t(t.zpsm) << 56);
            gs.uploadImageNative(zbuf, 0, trxreg, 0, z.data(), uint32_t(z.size()));
            dumpFrame(pic, true);
            if (++g_stats.injected % 600 == 0)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                std::fprintf(stderr, "[pop:world] %llu frames injected, %llu missed, %llu unexpected buffers; per frame: worker %.2f ms (read %.2f, "
                                     "upload %.2f, draw %.2f), GS thread waited %.2f ms; %zu textures, %zu MB\n",
                             (unsigned long long)g_stats.injected, (unsigned long long)g_stats.missed,
                             (unsigned long long)g_stats.wrongTarget, g_stats.drawMs / double(g_stats.drawn), g_stats.loadMs / double(g_stats.drawn),
                             g_stats.uploadMs / double(g_stats.drawn), g_stats.renderMs / double(g_stats.drawn),
                             g_stats.waitMs / double(g_stats.injected + g_stats.missed), g_stats.textures, g_stats.textureBytes >> 20);
            }
            return true;
        }

    }

    // starts the worker and waits for its renderer: the game skips draws only if they can be drawn
    bool active(const GSRegisters *display)
    {
        g_display = display;
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
#ifdef GS_HOST_PRESENT
        if (g_presentOn)
            GSHostPresent::setHook(presentHook);
#ifdef GS_HOST_DRAWS_OFF
        if (g_gsOff)
        {
            GSHostInject::setDrawsOff(true);
            std::fprintf(stderr, "[pop:world] GS off: no draws reach the GS, the native presenter shows the picture\n");
        }
#endif
#endif
        return true;
    }

    void submitFrame(uint32_t seq, uint32_t generation, std::vector<uint8_t> &&pnf)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_failed)
                return;
            g_queue.push_back({seq, generation, std::move(pnf)});
        }
        g_wake.notify_one();
    }

    uint32_t sentGeneration()
    {
        std::lock_guard<std::mutex> lock(g_sentMutex);
        return g_sentGeneration;
    }

    // a frame written in an older generation sends everything in full and notes nothing (its keys would
    // otherwise count as kept in the new generation, but its geometry goes before the drop)
    bool meshSent(uint64_t key, uint32_t generation)
    {
        std::lock_guard<std::mutex> lock(g_sentMutex);
        if (generation != g_sentGeneration)
            return false;
        return !g_sent.insert(key).second;
    }

    void noteFrame(uint32_t seq, std::string &&text)
    {
        std::lock_guard<std::mutex> lock(g_glitchMutex);
        g_notes.push_back({seq, std::move(text)});
        while (g_notes.size() > 16)
            g_notes.pop_front();
    }

    // <path>: "PNGL" u32 version 1, frames, width, height, then per frame the RGB picture (zeros without one);
    // <path>.txt: one line per frame. Written on a thread from a copy, so the game does not wait for the disk.
    void dumpRecent(const std::string &path)
    {
        std::deque<GlitchFrame> frames;
        {
            std::lock_guard<std::mutex> lock(g_glitchMutex);
            frames = g_glitch;
        }
        if (frames.empty())
            return;
        std::thread([frames = std::move(frames), path] {
            const char *kState[] = {"injected", "NO PICTURE", "NOT DRAWN", "UNEXPECTED BUFFERS"};
            FILE *bin = std::fopen(path.c_str(), "wb");
            FILE *txt = std::fopen((path + ".txt").c_str(), "w");
            if (!bin || !txt)
            {
                std::fprintf(stderr, "[pop:glitch] cannot write %s\n", path.c_str());
                if (bin)
                    std::fclose(bin);
                if (txt)
                    std::fclose(txt);
                return;
            }
            const uint32_t header[5] = {0x4C474E50u, 1u, uint32_t(frames.size()), kGlitchW, kGlitchH};
            std::fwrite(header, sizeof header, 1, bin);
            const std::vector<uint8_t> blank(size_t(kGlitchW) * kGlitchH * 3, 0);
            for (const GlitchFrame &g : frames)
            {
                const std::vector<uint8_t> &px = g.rgb.empty() ? blank : g.rgb;
                std::fwrite(px.data(), 1, px.size(), bin);
                std::fprintf(txt, "frame %u at %.1f ms: %s, %u draws, FRAME fbp %x -> %x, on screen %x | %s\n", g.seq, g.atMs, kState[g.state], g.draws,
                             g.fbp, g.target, g.shown, g.note.seq == g.seq ? g.note.text.c_str() : "(no note)");
            }
            std::fclose(bin);
            std::fclose(txt);
            std::fprintf(stderr, "[pop:glitch] last %zu frames written to %s\n", frames.size(), path.c_str());
        }).detach();
    }
}

#else // a runtime without the GS host-inject hook: the native world cannot be shown

namespace pop_native_world
{
    bool active(const GSRegisters *) { return false; }
    void submitFrame(uint32_t, uint32_t, std::vector<uint8_t> &&) {}
    uint32_t sentGeneration() { return 0; }
    bool meshSent(uint64_t, uint32_t) { return false; }
    void noteFrame(uint32_t, std::string &&) {}
    void dumpRecent(const std::string &) {}
}

#endif
