// Native renderer probe (POP_DRAWPROBE=1, needs POP_GIF_TAP=1): how much of each frame the
// Jade draw manager produces, to decide whether a native renderer hooked there would cover the
// picture (CLAUDE.md, "Native renderer probe").
//
// Per frame: calls of CJadeDrawManager::Display3DElements (0x4BC000), CElementList::Render
// (0x4B9440) and CJMeshElement::Display (0x4C8A80, by pass); and the GS primitives the frame's
// DMA produced, with covered pixel area, split by GIF path (PATH1 = VU1 XGKICK, which is the
// manager's VU1 drivers; PATH2/3 = VIF1 DIRECT / GIF DMA) and render target (FRAME.FBP).
// The game kicks its whole frame's DMA chain once, inside PS2_Flip, so GS work is attributed by
// path, not by time. Printed every 30 frames as "[pop:draw]" lines.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/ps2_gif_arbiter.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace pop_drawprobe
{
    namespace
    {
        PS2Runtime *g_rt = nullptr;
        std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();
        PS2Runtime::RecompiledFunction g_origDisplay3D = nullptr, g_origListRender = nullptr, g_origMeshDisplay = nullptr;

        // Primitive size buckets: full-screen passes (>= 100k px), large (>= 10k), small.
        enum Bucket { kFull, kLarge, kSmall, kBuckets };
        struct PathStats
        {
            uint64_t packets = 0, prims = 0;
            double area = 0.0; // pixels
            std::map<uint32_t, double> areaByFbp;
            std::array<std::array<uint64_t, 2>, kBuckets> primsBy{};  // [bucket][textured]
            std::array<std::array<double, 2>, kBuckets> areaBy{};
        };
        struct Totals
        {
            uint64_t frames = 0, display3d = 0, listRender = 0;
            std::array<uint64_t, 6> meshByPass{};
            std::array<PathStats, 4> path; // index 1..3
        } g_t;

        // GS drawing state seen in the GIF stream (both contexts).
        uint32_t g_prim = 0;
        std::array<uint32_t, 2> g_fbp{};
        int g_queued = 0;
        std::array<std::array<float, 2>, 3> g_v{};

        void addArea(int path, double a)
        {
            PathStats &p = g_t.path[path];
            ++p.prims;
            p.area += a;
            const int b = a >= 100000.0 ? kFull : a >= 10000.0 ? kLarge : kSmall;
            const int tex = (g_prim >> 4) & 1u;
            ++p.primsBy[b][tex];
            p.areaBy[b][tex] += a;
            p.areaByFbp[g_fbp[(g_prim >> 9) & 1u]] += a;
        }

        void vertex(int path, uint32_t x16, uint32_t y16, bool kick)
        {
            const float x = static_cast<float>(x16) / 16.0f, y = static_cast<float>(y16) / 16.0f;
            const uint32_t type = g_prim & 7u;
            if (g_queued < 3)
                g_v[g_queued++] = {x, y};
            else if (type == 5) // fan: keep the first vertex
            {
                g_v[1] = g_v[2];
                g_v[2] = {x, y};
            }
            else
            {
                g_v[0] = g_v[1];
                g_v[1] = g_v[2];
                g_v[2] = {x, y};
            }
            if (!kick)
                return;
            auto tri = [&] {
                const auto &a = g_v[0], &b = g_v[1], &c = g_v[2];
                return std::fabs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) * 0.5;
            };
            switch (type)
            {
            case 0: addArea(path, 1.0); g_queued = 0; break;            // point
            case 1: if (g_queued >= 2) { addArea(path, 0.0); g_queued = 0; } break; // line
            case 2: if (g_queued >= 2) addArea(path, 0.0); break;       // line strip
            case 3: if (g_queued >= 3) { addArea(path, tri()); g_queued = 0; } break;
            case 4: case 5: if (g_queued >= 3) addArea(path, tri()); break;
            case 6:                                                    // sprite
                if (g_queued >= 2)
                {
                    const auto &a = g_v[g_queued - 2], &b = g_v[g_queued - 1];
                    addArea(path, std::fabs((b[0] - a[0]) * (b[1] - a[1])));
                    g_queued = 0;
                }
                break;
            default: break;
            }
        }

        void reg(int path, uint32_t r, uint64_t v)
        {
            switch (r)
            {
            case 0x00: g_prim = static_cast<uint32_t>(v) & 0x7FFu; g_queued = 0; break; // PRIM
            case 0x4C: g_fbp[0] = static_cast<uint32_t>(v) & 0x1FFu; break;             // FRAME_1
            case 0x4D: g_fbp[1] = static_cast<uint32_t>(v) & 0x1FFu; break;             // FRAME_2
            case 0x05: case 0x0D: vertex(path, v & 0xFFFFu, (v >> 16) & 0xFFFFu, r == 0x05); break; // XYZ2 / XYZ3
            case 0x04: case 0x0C: vertex(path, v & 0xFFFFu, (v >> 16) & 0xFFFFu, r == 0x04); break; // XYZF2 / XYZF3
            default: break;
            }
        }

        void onGifPacket(int path, const uint8_t *data, uint32_t size)
        {
            g_t.path[path].packets++;
            uint32_t off = 0;
            while (off + 16u <= size)
            {
                uint64_t tag, regs;
                std::memcpy(&tag, data + off, 8);
                std::memcpy(&regs, data + off + 8, 8);
                off += 16;
                const uint32_t nloop = tag & 0x7FFFu, flg = (tag >> 58) & 3u;
                uint32_t nreg = (tag >> 60) & 0xFu;
                if (nreg == 0)
                    nreg = 16;
                if ((tag >> 46) & 1u)
                    reg(path, 0x00, (tag >> 47) & 0x7FFu);
                if (flg == 0) // PACKED
                {
                    for (uint32_t i = 0; i < nloop && off + 16u <= size; ++i)
                        for (uint32_t j = 0; j < nreg && off + 16u <= size; ++j, off += 16)
                        {
                            uint64_t lo, hi;
                            std::memcpy(&lo, data + off, 8);
                            std::memcpy(&hi, data + off + 8, 8);
                            const uint32_t r = (regs >> (4 * j)) & 0xFu;
                            if (r == 0xE) // A+D
                                reg(path, hi & 0xFFu, lo);
                            else if (r == 0x5) // XYZ2: ADC (bit 111) set = no kick
                                vertex(path, lo & 0xFFFFu, (lo >> 32) & 0xFFFFu, !((hi >> 47) & 1u));
                            else if (r == 0x4) // XYZF2
                                vertex(path, lo & 0xFFFFu, (lo >> 32) & 0xFFFFu, !((hi >> 47) & 1u));
                            else if (r == 0x0)
                                reg(path, 0x00, lo & 0x7FFu);
                        }
                }
                else if (flg == 1) // REGLIST
                {
                    const uint32_t words = nloop * nreg;
                    for (uint32_t k = 0; k < words && off + 8u <= size; ++k, off += 8)
                    {
                        uint64_t v;
                        std::memcpy(&v, data + off, 8);
                        const uint32_t r = (regs >> (4 * (k % nreg))) & 0xFu;
                        if (r == 0x5 || r == 0x4 || r == 0x0)
                            reg(path, r, v);
                    }
                    off = (off + 15u) & ~15u;
                }
                else // IMAGE
                    off += 16u * nloop;
            }
        }

        void report()
        {
            if (g_t.frames == 0)
            {
                // No PS2_Flip for a while (loading screen): totals for the window instead.
                double area = 0.0;
                uint64_t packets = 0;
                for (int p = 1; p <= 3; ++p)
                {
                    area += g_t.path[p].area;
                    packets += g_t.path[p].packets;
                }
                std::fprintf(stderr, "[pop:draw] no flip for 10 s (loading screen?): %llu GIF packets, %.0fk px (PATH1 %.0fk, PATH2 %.0fk, PATH3 %.0fk), Display3DElements %llu, CJMeshElement::Display %llu\n",
                             static_cast<unsigned long long>(packets), area / 1000.0, g_t.path[1].area / 1000.0,
                             g_t.path[2].area / 1000.0, g_t.path[3].area / 1000.0,
                             static_cast<unsigned long long>(g_t.display3d),
                             static_cast<unsigned long long>(g_t.meshByPass[0] + g_t.meshByPass[1] + g_t.meshByPass[2] + g_t.meshByPass[3] + g_t.meshByPass[4]));
                g_t = Totals{};
                return;
            }
            const double n = static_cast<double>(g_t.frames);
            std::fprintf(stderr, "[pop:draw] per frame over %llu frames: Display3DElements %.2f, CElementList::Render %.1f, CJMeshElement::Display %.1f (pass0 %.1f pass1 %.1f pass2 %.1f pass3 %.1f)\n",
                         static_cast<unsigned long long>(g_t.frames), g_t.display3d / n, g_t.listRender / n,
                         (g_t.meshByPass[0] + g_t.meshByPass[1] + g_t.meshByPass[2] + g_t.meshByPass[3] + g_t.meshByPass[4]) / n,
                         g_t.meshByPass[0] / n, g_t.meshByPass[1] / n, g_t.meshByPass[2] / n, g_t.meshByPass[3] / n);
            double total = 0.0;
            for (int p = 1; p <= 3; ++p)
                total += g_t.path[p].area;
            for (int p = 1; p <= 3; ++p)
            {
                const PathStats &s = g_t.path[p];
                std::string fbps;
                for (const auto &[fbp, a] : s.areaByFbp)
                    if (a / n >= 1000.0)
                    {
                        char b[48];
                        std::snprintf(b, sizeof(b), " fbp%u:%.0fk", fbp, a / n / 1000.0);
                        fbps += b;
                    }
                std::fprintf(stderr, "[pop:draw]   PATH%d: %.0f packets, %.0f prims, %.0fk px (%.0f%% of area)%s\n", p,
                             s.packets / n, s.prims / n, s.area / n / 1000.0, total > 0 ? 100.0 * s.area / total : 0.0, fbps.c_str());
                if (s.prims == 0)
                    continue;
                static const char *names[kBuckets] = {"full-screen", "large", "small"};
                std::string by;
                for (int b = 0; b < kBuckets; ++b)
                    for (int t = 0; t < 2; ++t)
                        if (s.primsBy[b][t])
                        {
                            char buf[96];
                            std::snprintf(buf, sizeof(buf), " %s%s %.0f/%.0fk", names[b], t ? "+tex" : "", s.primsBy[b][t] / n,
                                          s.areaBy[b][t] / n / 1000.0);
                            by += buf;
                        }
                std::fprintf(stderr, "[pop:draw]     prims/px by size:%s\n", by.c_str());
            }
            g_t = Totals{};
        }

        void display3D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_t.display3d;
            g_origDisplay3D(rdram, ctx, runtime);
        }
        void listRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_t.listRender;
            g_origListRender(rdram, ctx, runtime);
        }
        void meshDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pass = getRegU32(ctx, 6) & 0xFFu; // Display(this, matrix index, pass)
            ++g_t.meshByPass[pass < 5 ? pass : 5];
            g_origMeshDisplay(rdram, ctx, runtime);
        }
    }

    bool enabled()
    {
        static const bool on = [] { const char *v = std::getenv("POP_DRAWPROBE"); return v && *v == '1'; }();
        return on;
    }

    void install(PS2Runtime &runtime)
    {
        if (!enabled())
            return;
        g_rt = &runtime;
        g_origDisplay3D = runtime.lookupFunction(0x004BC000u);
        g_origListRender = runtime.lookupFunction(0x004B9440u);
        g_origMeshDisplay = runtime.lookupFunction(0x004C8A80u);
        if (!g_origDisplay3D || !g_origListRender || !g_origMeshDisplay)
        {
            std::fprintf(stderr, "[pop:draw] draw manager functions not found; probe off\n");
            return;
        }
        runtime.replaceFunction(0x004BC000u, display3D);
        runtime.replaceFunction(0x004B9440u, listRender);
        runtime.replaceFunction(0x004C8A80u, meshDisplay);
        std::fprintf(stderr, "[pop:draw] probe on\n");
    }

    // From the GIF tap, for every packet the GS processes.
    void onPacket(const uint8_t *data, uint32_t size)
    {
        if (!g_rt)
            return;
#ifdef PS2X_GIF_ARBITER_CURRENT_PATH
        const int path = static_cast<int>(g_rt->gifArbiter().currentPath());
#else
        const int path = 3;
#endif
        onGifPacket(path >= 1 && path <= 3 ? path : 3, data, size);
        if (std::chrono::steady_clock::now() - g_lastReport > std::chrono::seconds(10)) // few or no flips (loading screen)
        {
            report();
            g_lastReport = std::chrono::steady_clock::now();
        }
    }

    // After each PS2_Flip (the frame's DMA has been processed by then).
    void onFrameEnd()
    {
        if (!g_rt)
            return;
        if (++g_t.frames >= 30)
        {
            report();
            g_lastReport = std::chrono::steady_clock::now();
        }
    }
}
