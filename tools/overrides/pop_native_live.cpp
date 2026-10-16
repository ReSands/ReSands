// Native renderer, live side (Stage B steps 2-3), POP_NATIVE_LIVE=1. Converts every static mesh element
// as the game loads it, with the C++ asset conversion (tools/native/src/assets.*, the same code the
// offline viewer uses), and checks each element the game then draws against its conversion: every
// position the draw's DMA packet uploads (UNPACK V3-32) must be one of the converted vertices.
// Prints "[pop:native]" statistics every 5 s. Nothing is drawn natively yet.
// POP_NATIVE_FRAME=<file>:<seconds>: after that many seconds, writes the next frame's static world as
// the native renderer would draw it (camera = world -> clip at Display3DElements, then per drawn
// element and matrix: the matrix pool entry and the converted batches); render it with
// pop_native_view <file> <any> <out.png> --frame.
//
// Hooks: CJMeshElement::ReadElement (0x4C9500, this, char **cursor): the element's bytes start at
// *cursor in the pack buffer, in file form (block indices in the packet tags, patched to pointers by
// the original), so the conversion runs before the original. CJMeshElement::Display (0x4C8A80,
// element, matrix index, pass): the pass packet is element + 0x20 + 4 * pass (pass 4: packet 1), a
// DMA chain in guest memory ending in RET; the function draws the object's next element (+0x4C) by an
// internal tail call, so the hook walks that chain itself. Both checks run before the original
// function (on the SotC runtime, code after calling a generated function can run mid-call).

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

// The native sources, by path: this file is copied to <runtime>/ps2xRuntime/src/runner, six levels
// below the repository root on both the Mac (game/recomp/...) and the PC (game/recomp-sotc/...).
#include "../../../../../../tools/native/src/assets.cpp"
#include "../../../../../../tools/native/src/meshdecode.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

uint32_t popTextureKey(uint32_t index); // pop_slus20743.cpp: texture index -> bigfile key
namespace pop_hdtex
{
    bool readEntry(const std::string &path, uint32_t key, std::vector<uint8_t> &out); // pop_hdtextures.cpp
}

namespace pop_native_live
{
    namespace
    {
        constexpr uint32_t kReadElement = 0x004C9500u;
        constexpr uint32_t kMeshDisplay = 0x004C8A80u;
        constexpr uint32_t kDisplay3D = 0x004BC000u; // CJadeDrawManager::Display3DElements
        constexpr uint32_t kRefDisplay = 0x004C7B40u; // CJMeshElementRef::Display (instanced meshes)
        constexpr uint32_t kGfxDisplay = 0x00458FC0u; // CElementList::Render's default case: GFX (VU program 4, FUN_004584C0)
        constexpr size_t kRam = 32u << 20;

        PS2Runtime::RecompiledFunction g_origGfx = nullptr;
        // GFX draws of the captured frame (not drawn natively yet): entry type byte, element, texture key, flags
        struct GfxDraw
        {
            uint32_t type, element, textureKey, flags;
        };
        std::vector<GfxDraw> g_frameGfx;
        PS2Runtime::RecompiledFunction g_origRead = nullptr, g_origDisplay = nullptr, g_origDisplay3D = nullptr, g_origRefDisplay = nullptr;

        // Per converted element: its vertex positions (exact float bits), for the draw check.
        struct Converted
        {
            std::unordered_set<uint64_t> positions; // hash of (x, y, z) bits
            size_t batches = 0, vertices = 0, triangles = 0;
            std::vector<pop_assets::Batch> meshes;  // kept for the frame export
        };
        // frame export (POP_NATIVE_FRAME)
        std::string g_framePath;
        double g_frameAfter = -1.0;
        bool g_frameCapturing = false, g_frameDone = false;
        std::chrono::steady_clock::time_point g_start;
        float g_frameClip[16];
        std::vector<std::pair<uint32_t, std::array<float, 16>>> g_frameDraws; // element, model matrix
        std::vector<uint32_t> g_frameSub;                                       // current sub-material per draw
        struct DrawMaterial
        {
            uint32_t textureKey = 0;
            uint32_t flags = 0; // render state flags (sub-material word 1 = record +0x1C): pop_native::materialFromGsFlags
            uint32_t ref = 0;   // sub-material +0x10 (record +0x28): AREF in the top byte
            // ExtractLocalAlphaAndConstantColor (0x4B23C0) + CJMeshElement::Display (passes 0 and 2):
            // VIF STROW/STMASK replace the vertex colour's RGB (mask 0x15151515) with a constant
            // colour and/or its alpha (0x40404040) with a local alpha, both on the 0x80 = 1.0 scale
            int32_t constColour = -1; // 0xBBGGRR, -1: vertex colours
            int32_t constAlpha = -1;  // 0..0x7F, -1: vertex alpha
            // GS fog of the draw: zone (0x70000084, uploaded by FUN_004B7200 from the table at
            // 0x70000270 + id * 16 to VU 0x10 = (255, 0, a, b); F = a + b * w) and FOGCOL as the
            // render state sends it (0x70000024: the zone colour, 0 for additive/darkening blends)
            float fogA = 255.0f, fogB = 0.0f;
            uint32_t fogColour = 0;
            uint32_t zTest = 1; // render state 0x70000038: 0 = ZTST ALWAYS (Display3DElements' +0x78 list)
            uint32_t seq = 0;   // the draw's position in the frame (game draw order)
        };
        uint32_t g_frameSeq = 0;
        const bool g_noLighting = std::getenv("POP_NATIVE_NOLIGHT") != nullptr; // A/B: unlit characters
        uint8_t *g_rdramPtr = nullptr;
        std::vector<DrawMaterial> g_frameMaterials;
        std::vector<std::pair<std::vector<pop_assets::Batch>, DrawMaterial>> g_framePosed; // skinned draws, posed in world space
        std::unordered_map<uint64_t, uint32_t> g_frameLightmap; // (element, matrix index) -> lightmap texture key (pass 3)

        // PS2 world packs the game has looked up (BIG_ul_SearchKeyToPos), read from the disc's
        // PRINCE.BF (POP_PS2_BIGFILE, default DATA/PRINCE.BF) and unpacked on a worker thread:
        // textures for the native frame are decoded from them by key.
        struct LoadedPack
        {
            std::vector<uint8_t> bytes;
            pop_assets::Pack index;
        };
        std::mutex g_packsMutex;
        std::unordered_map<uint32_t, std::shared_ptr<LoadedPack>> g_packs; // pack key -> pack (null while loading)
        bool g_live = false;

        bool findTexture(uint32_t key, pop_native::Image &out)
        {
            std::vector<std::shared_ptr<LoadedPack>> packs;
            {
                std::lock_guard<std::mutex> lock(g_packsMutex);
                for (const auto &p : g_packs)
                    if (p.second)
                        packs.push_back(p.second);
            }
            for (const auto &p : packs)
                if (p->index.records.count(key) && pop_assets::decodeTexture(p->index, key, out))
                    return true;
            return false;
        }
        std::unordered_set<uint64_t> g_frameSeen;                               // (element, matrix index)

        uint32_t guestAny(uint8_t *rdram, uint32_t addr) // main RAM or scratchpad
        {
            uint32_t v;
            std::memcpy(&v, getConstMemPtr(rdram, addr), 4);
            return v;
        }
        uint32_t guest8(uint8_t *rdram, uint32_t addr) { return *getConstMemPtr(rdram, addr); }
        uint32_t guest16(uint8_t *rdram, uint32_t addr)
        {
            uint16_t v;
            std::memcpy(&v, getConstMemPtr(rdram, addr), 2);
            return v;
        }

        // The render state's current sub-material (0x70000034, set by CElementList::Render before
        // Display): word 0 = texture index (low 16 bits; flags in the top byte), word 1 = the record's
        // +0x1C = the render state flags (blend kind in bits 16-19, 0x10 alpha test, 0x200 no depth
        // write; decoded by pop_native::materialFromGsFlags as SetBlending 0x4B2500 does), +0x10 = its
        // +0x28 (AREF in the top byte); 16 Oct.
        DrawMaterial currentMaterial(uint8_t *rdram)
        {
            DrawMaterial dm;
            dm.seq = g_frameSeq++;
            const uint32_t sub = guestAny(rdram, 0x70000034u);
            if (!sub)
                return dm;
            // word 0: texture index in the low bits, flags in the top byte (0x88/0xB8/0xC0/0xF8.. seen)
            dm.textureKey = popTextureKey(guestAny(rdram, sub) & 0xFFFFu);
            static int logged = 0;
            if (!dm.textureKey && logged++ < 16)
            {
                std::fprintf(stderr, "[pop:native] no texture key: sub %08x words", sub);
                for (uint32_t k = 0; k < 12; ++k)
                    std::fprintf(stderr, " %08x", guestAny(rdram, sub + 4 * k));
                std::fprintf(stderr, "\n");
            }
            // what FUN_004B2000 receives as the render state (flags, AREF word): CElementList::Render
            // compares exactly these two words with 0x70000000 / 0x70000004 before calling it
            dm.flags = guestAny(rdram, sub + 4);
            dm.ref = guestAny(rdram, sub + 0x10);
            // flags bits 13-15: 2 = constant colour from the current material's (0x70000030) +8 word,
            // 4 = 0x808080; 0x400: local alpha = (u16 at sub + 2) << 15 >> 24 = word 0 >> 25
            const uint32_t cc = (dm.flags & 0xE000) >> 13, material = guestAny(rdram, 0x70000030u);
            if (cc == 2 && material)
                dm.constColour = int32_t(guestAny(rdram, material + 8) & 0xFFFFFF);
            else if (cc == 4)
                dm.constColour = 0x808080;
            if (dm.flags & 0x400)
                dm.constAlpha = int32_t(guestAny(rdram, sub) >> 25);
            const uint32_t zone = guestAny(rdram, 0x70000084u) & 0xFF, fa = guestAny(rdram, 0x70000270u + zone * 16 + 8),
                           fb = guestAny(rdram, 0x70000270u + zone * 16 + 12);
            std::memcpy(&dm.fogA, &fa, 4);
            std::memcpy(&dm.fogB, &fb, 4);
            dm.fogColour = guestAny(rdram, 0x70000024u) & 0xFFFFFF;
            dm.zTest = guest8(rdram, 0x70000038u);
            return dm;
        }

        // VU1 character lighting as CVuManager::DoSetSkinLightEnv (0x4B64C0) uploads it: the light set
        // index is at 0x70000088 (SetSkinDriver, from list entry bits 42-49); set = *(0x700004F0) +
        // index * 0xF0: +4 ambient RGB (0x80 = 1.0), +8 light count, +0x10 + 16i directions, +0xD0 + 4i
        // colours, +0xE0 + 4i types, +1 point-light flag. No lights and no ambient: the global
        // environment (gp+0x140 directions as columns, gp+0x180 colours as rows, gp = 0x6749F0).
        // A constant colour (render state 0x70000039) replaces the ambient and switches the lights off.
        // Vertex colour = ambient + sum max(0, n . d_i) * colour_i (tools/chainskin.py; game-checked).
        struct LightEnv
        {
            bool valid = false;
            int count = 0;
            float dir[4][3] = {}, col[4][3] = {}, amb[3] = {};
        };
        float guestF(uint8_t *rdram, uint32_t a)
        {
            const uint32_t b = guestAny(rdram, a);
            float f;
            std::memcpy(&f, &b, 4);
            return f;
        }
        LightEnv readLightEnv(uint8_t *rdram)
        {
            LightEnv e;
            const uint32_t table = guestAny(rdram, 0x700004F0u);
            if (!table)
                return e;
            const uint32_t set = table + (guestAny(rdram, 0x70000088u) & 0xFF) * 0xF0u;
            uint32_t ambient = guestAny(rdram, set + 4);
            int count = int(guest8(rdram, set + 8));
            const bool pointFlag = guest8(rdram, set + 1) != 0;
            if (guest8(rdram, 0x70000039u))
            {
                ambient = guestAny(rdram, 0x7000003Cu);
                count = 0;
            }
            for (int c = 0; c < 3; ++c)
                e.amb[c] = float((ambient >> (8 * c)) & 0xFF);
            if ((ambient & 0xFFFFFF) == 0 && count == 0)
            {
                const uint32_t m = 0x006749F0u + 0x140u, cm = 0x006749F0u + 0x180u;
                e.count = 4;
                for (int i = 0; i < 4; ++i)
                    for (int c = 0; c < 3; ++c)
                    {
                        e.dir[i][c] = guestF(rdram, m + 4u * (4 * c + i));
                        e.col[i][c] = guestF(rdram, cm + 4u * (4 * i + c));
                    }
            }
            else
            {
                for (int i = 0; i < count && i < 4; ++i)
                {
                    if (pointFlag && guestAny(rdram, set + 0xE0 + 4 * i) != 0)
                        continue; // point lights (types 1/2) not modelled yet
                    const uint32_t c = guestAny(rdram, set + 0xD0 + 4 * i);
                    for (int k = 0; k < 3; ++k)
                    {
                        e.dir[e.count][k] = guestF(rdram, set + 0x10 + 16 * i + 4 * k);
                        e.col[e.count][k] = float((c >> (8 * k)) & 0xFF);
                    }
                    ++e.count;
                }
            }
            e.valid = true;
            return e;
        }

        // A skinned element drawn with matrix index `index`, posed as CJMeshElement::Display uploads
        // its bones: palette at element +0x48 (byte 0 = bone count n, bytes 4.. = bone ids); bone k's
        // matrix = *(0x700001D0) + (firstBone + id[k]) * 0x40, firstBone = u16 table at 0x700001DC
        // indexed by index - u16 at 0x7000025E. n >= 2: vertex slot s uses bone s - 1 (VU 0x388); one
        // bone: every vertex uses it (VU 0). Matrices are world space, row vectors.
        bool poseSkinned(uint8_t *rdram, uint32_t element, uint32_t index, const std::vector<pop_assets::SkinnedBatch> &bind,
                         std::vector<pop_assets::Batch> &out, const LightEnv *light = nullptr)
        {
            const uint32_t pal = guestAny(rdram, element + 0x48);
            if (!pal)
                return false;
            const uint32_t n = guest8(rdram, pal);
            const uint32_t base = guestAny(rdram, 0x700001D0u), baseIndex = guest16(rdram, 0x7000025Eu);
            const uint32_t first = guest16(rdram, 0x700001DCu + ((index & 0xFFFF) - baseIndex) * 2u);
            std::vector<std::array<float, 16>> bones(std::max(n, 1u));
            for (uint32_t k = 0; k < bones.size(); ++k)
            {
                const uint32_t m = base + (first + guest8(rdram, pal + 4 + k)) * 0x40u;
                for (int j = 0; j < 16; ++j)
                {
                    const uint32_t bits = guestAny(rdram, m + 4u * j);
                    std::memcpy(&bones[k][j], &bits, 4);
                }
            }
            for (const auto &b : bind)
            {
                const size_t nv = b.positions.size() / 3;
                pop_assets::Batch o;
                o.vertices.resize(nv);
                for (size_t i = 0; i < nv; ++i)
                {
                    const float *v = &b.positions[3 * i];
                    float acc[3] = {0, 0, 0}, nacc[3] = {0, 0, 0}, total = 0;
                    const bool lit = light && light->valid && b.normals.size() >= 3 * nv;
                    for (int k = 0; k < 3; ++k)
                    {
                        const int32_t slot = b.slots[3 * i + k];
                        const float w = n < 2 ? (k == 0 ? 1.0f : 0.0f) : b.weights[3 * i + k];
                        if (w == 0 || (n >= 2 && (slot < 1 || uint32_t(slot) > n)))
                            continue;
                        const auto &m = bones[n < 2 ? 0 : uint32_t(slot - 1)];
                        for (int c = 0; c < 3; ++c)
                            acc[c] += w * (v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + m[12 + c]);
                        if (lit)
                        {
                            const float *nb = &b.normals[3 * i];
                            for (int c = 0; c < 3; ++c)
                                nacc[c] += w * (nb[0] * m[c] + nb[1] * m[4 + c] + nb[2] * m[8 + c]);
                        }
                        total += w;
                    }
                    pop_native::Vertex &out1 = o.vertices[i];
                    for (int c = 0; c < 3; ++c)
                        out1.pos[c] = total > 0 ? acc[c] / total : v[c];
                    out1.uv[0] = b.uvs[2 * i];
                    out1.uv[1] = b.uvs[2 * i + 1];
                    std::memcpy(out1.colour, &b.colours[4 * i], sizeof out1.colour);
                    const float nl = std::sqrt(nacc[0] * nacc[0] + nacc[1] * nacc[1] + nacc[2] * nacc[2]);
                    if (lit && nl > 0)
                    {
                        float c3[3] = {light->amb[0], light->amb[1], light->amb[2]};
                        for (int l = 0; l < light->count; ++l)
                        {
                            const float k = std::max(0.0f, (nacc[0] * light->dir[l][0] + nacc[1] * light->dir[l][1] + nacc[2] * light->dir[l][2]) / nl);
                            for (int c = 0; c < 3; ++c)
                                c3[c] += k * light->col[l][c];
                        }
                        for (int c = 0; c < 3; ++c)
                            out1.colour[c] = std::min(c3[c], 255.0f) / 128.0f;
                    }
                    out1.uv2[0] = out1.uv2[1] = 0;
                }
                o.indices = b.indices;
                out.push_back(std::move(o));
            }
            return true;
        }
        std::unordered_map<uint32_t, Converted> g_elements; // element object address -> conversion
        std::unordered_map<uint32_t, std::vector<pop_assets::SkinnedBatch>> g_skinned; // skinned elements: bind pose
        // why an element was not converted: 's' skinned, 'i' instanced (variant 3), 'n' no pass packet, 'f' failed
        std::unordered_map<uint32_t, char> g_skipped;
        std::unordered_map<char, uint64_t> g_unknownBy; // unknown draws by reason ('?': never loaded)

        struct Stats
        {
            uint64_t read = 0, converted = 0, notConverted = 0, batches = 0, vertices = 0, triangles = 0;
            uint64_t draws = 0, drawsKnown = 0, drawsUnknown = 0, positions = 0, positionsFound = 0, drawsExact = 0;
        } g_stats;
        std::chrono::steady_clock::time_point g_lastReport;

        uint32_t guest32(uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & (kRam - 1)), 4);
            return v;
        }

        uint64_t key3(const float *p)
        {
            uint32_t b[3];
            std::memcpy(b, p, 12);
            return (uint64_t(b[0]) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(b[1]) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(b[2]) * 0x165667B19E3779F9ull);
        }

        void report()
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - g_lastReport < std::chrono::seconds(5))
                return;
            g_lastReport = now;
            const Stats &s = g_stats;
            std::fprintf(stderr,
                         "[pop:native] loaded %llu elements: %llu converted (%llu batches, %llu vertices, %llu triangles), %llu not "
                         "(skinned/instanced/no pass); draws %llu: %llu converted, %llu unknown; positions %llu/%llu found, %llu draws exact\n",
                         (unsigned long long)s.read, (unsigned long long)s.converted, (unsigned long long)s.batches,
                         (unsigned long long)s.vertices, (unsigned long long)s.triangles, (unsigned long long)s.notConverted,
                         (unsigned long long)s.draws, (unsigned long long)s.drawsKnown, (unsigned long long)s.drawsUnknown,
                         (unsigned long long)s.positionsFound, (unsigned long long)s.positions, (unsigned long long)s.drawsExact);
            std::fprintf(stderr, "[pop:native] unknown draws: skinned %llu, instanced %llu, no pass %llu, failed %llu, never loaded %llu\n",
                         (unsigned long long)g_unknownBy['s'], (unsigned long long)g_unknownBy['i'], (unsigned long long)g_unknownBy['n'],
                         (unsigned long long)g_unknownBy['f'], (unsigned long long)g_unknownBy['?']);
        }

        // Batches of a packet as the hardware receives it: the DMA chain from `packet` in guest
        // memory, its VIF UNPACKs in order; each V3-32 positions upload closes a batch with the
        // latest V3-16 UVs (s, t, strip flag; raw / uvscale) and V4-8 colours (0x80 = 1.0) before it.
        // Used for instanced (CJMeshElementRef) draws, whose colours/UVs are per instance.
        void livePacketBatches(uint8_t *rdram, uint32_t packet, double uvscale, std::vector<pop_assets::Batch> &out)
        {
            std::vector<uint32_t> words;
            uint32_t pc = packet;
            for (int steps = 0; steps < 4096; ++steps)
            {
                const uint32_t t0 = guestAny(rdram, pc), addr = guestAny(rdram, pc + 4) & 0x7FFFFFF0u;
                const uint32_t qwc = t0 & 0xFFFF, id = (t0 >> 28) & 7;
                words.push_back(guestAny(rdram, pc + 8));
                words.push_back(guestAny(rdram, pc + 12));
                uint32_t data = pc + 16, next = pc + 16 + qwc * 16;
                bool stop = false;
                switch (id)
                {
                case 0: data = addr; stop = true; break;
                case 1: break;
                case 2: next = addr; break;
                case 3: case 4: data = addr; next = pc + 16; break;
                default: stop = true; break;
                }
                for (uint32_t k = 0; k < qwc * 4u; ++k)
                    words.push_back(guestAny(rdram, data + 4 * k));
                if (stop)
                    break;
                pc = next;
            }
            static const uint8_t kElementBytes[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};
            std::vector<uint8_t> uv, col;
            for (size_t i = 0; i < words.size();)
            {
                const uint32_t w = words[i++];
                const uint32_t cmd = (w >> 24) & 0x7F, num = ((w >> 16) & 0xFF) ? ((w >> 16) & 0xFF) : 256u;
                if (cmd >= 0x60)
                {
                    const uint32_t bytes = kElementBytes[cmd & 0xF] * num, n = (bytes + 3) / 4;
                    if (i + n > words.size())
                        break;
                    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&words[i]);
                    if ((cmd & 0xF) == 9)
                        uv.assign(raw, raw + bytes);
                    else if ((cmd & 0xF) == 0xE)
                        col.assign(raw, raw + bytes);
                    else if ((cmd & 0xF) == 8)
                    {
                        pop_assets::Batch b;
                        b.vertices.resize(num);
                        std::vector<int32_t> flags(num, 0);
                        for (uint32_t v = 0; v < num; ++v)
                        {
                            pop_native::Vertex &vx = b.vertices[v];
                            std::memcpy(vx.pos, raw + 12 * v, 12);
                            int16_t st[3] = {0, 0, 0};
                            if (6 * v + 6 <= uv.size())
                                std::memcpy(st, &uv[6 * v], 6);
                            vx.uv[0] = float(st[0] / uvscale);
                            vx.uv[1] = float(st[1] / uvscale);
                            flags[v] = st[2];
                            for (int k = 0; k < 4; ++k)
                                vx.colour[k] = 4 * v + 4 <= col.size() ? std::min(col[4 * v + k] / 128.0f, 1.0f) : 1.0f;
                            vx.uv2[0] = vx.uv2[1] = 0;
                        }
                        for (uint32_t v = 2; v < num; ++v)
                        {
                            if (flags[v])
                                continue;
                            if (v % 2 == 0)
                                b.indices.insert(b.indices.end(), {v - 2, v - 1, v});
                            else
                                b.indices.insert(b.indices.end(), {v - 1, v - 2, v});
                        }
                        if (!b.indices.empty())
                            out.push_back(std::move(b));
                    }
                    i += n;
                }
                else if (cmd == 0x4A)
                    i += 2 * num;
                else if (cmd == 0x50 || cmd == 0x51)
                    i += 4 * ((w & 0xFFFF) ? (w & 0xFFFF) : 65536u);
                else if (cmd == 0x20)
                    i += 1;
                else if (cmd == 0x30 || cmd == 0x31)
                    i += 4;
            }
        }

        std::vector<std::pair<std::vector<pop_assets::Batch>, std::pair<std::array<float, 16>, DrawMaterial>>> g_frameRefs; // instanced draws

        // CJMeshElementRef::Display(ref, matrix index, pass): the instance's pass packet is the word
        // at ref + 4 * pass (pass 4: 0; low 4 bits masked); the object's next ref is at +0x1C.
        void refDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing)
            {
                const uint32_t pass = getRegU32(ctx, 6) & 0xFF, index = getRegU32(ctx, 5) & 0xFFFF;
                uint32_t ref = getRegU32(ctx, 4);
                for (unsigned link = 0; ref && link < 4096 && (pass == 0 || pass == 1 || pass == 4); ++link)
                {
                    if (g_frameSeen.insert((uint64_t(ref) << 16) | index | (uint64_t(1) << 63)).second)
                    {
                        const uint32_t packet = guestAny(rdram, ref + 4 * (pass == 4 ? 0 : pass)) & ~0xFu;
                        std::vector<pop_assets::Batch> batches;
                        livePacketBatches(rdram, packet, 512.0, batches); // texture UVs 1/512 (VU1 drivers: ITOF4 * 1/32)
                        std::array<float, 16> model;
                        const uint32_t m = guestAny(rdram, 0x70000260u) + index * 0x40u;
                        for (int k = 0; k < 16; ++k)
                        {
                            const uint32_t bits = guestAny(rdram, m + 4u * k);
                            std::memcpy(&model[k], &bits, 4);
                        }
                        if (!batches.empty())
                            g_frameRefs.emplace_back(std::move(batches), std::make_pair(model, currentMaterial(rdram)));
                    }
                    ref = guestAny(rdram, ref + 0x1C);
                }
            }
            g_origRefDisplay(rdram, ctx, runtime);
        }

        // FUN_00458FC0(entry): the sub-material is the entry's material (+8) list (+0x10, linked by
        // +0x20) at index byte +0xC, as the function itself selects it
        void gfxDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing)
            {
                const uint32_t entry = getRegU32(ctx, 4);
                const uint32_t material = guestAny(rdram, entry + 8);
                uint32_t sub = material ? guestAny(rdram, material + 0x10) : 0;
                for (uint32_t k = 0, n = guest8(rdram, entry + 0xC); sub && k < n; ++k)
                    sub = guestAny(rdram, sub + 0x20);
                g_frameGfx.push_back({guest8(rdram, entry + 6), guestAny(rdram, entry) << 4,
                                      sub ? popTextureKey(guestAny(rdram, sub) & 0xFFFF) : 0u, sub ? guestAny(rdram, sub + 4) : 0u});
            }
            g_origGfx(rdram, ctx, runtime);
        }

        // Other draw paths of CElementList::Render (not drawn natively yet), counted per frame with
        // the current sub-material's texture: the render state is set before each of these calls.
        const double kIdxUvScale = std::getenv("POP_IDX_UVSCALE") ? std::atof(std::getenv("POP_IDX_UVSCALE")) : 512.0; // CJIdxMesh UVs: 1/512 (V3-16 raw 0..512)
        struct OtherPath
        {
            uint32_t address;
            const char *name;
            PS2Runtime::RecompiledFunction original;
        };
        OtherPath g_other[] = {
            {0x004BDB80u, "CJIdxMesh::Display", nullptr},
            {0x004C6200u, "CJadeSpriteElement::Display", nullptr},
            {0x004C77C0u, "CJSmWaterElement::Display", nullptr},
            {0x00469C00u, "FUN_00469C00 (list type 0x18)", nullptr},
        };
        // Halos (AE_AddHaloToDisplay 0x4AB1C0 -> array 0x6DF2C0, 0x20 bytes each, count 0x67518C):
        // position (x, y, z, 1), half sizes (world units), colour 0x808080 | (alpha / 2) << 24, material
        PS2Runtime::RecompiledFunction g_origAddHalo = nullptr;
        uint32_t g_haloAdds = 0, g_haloAddsInWindow = 0;
        void addHalo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_haloAdds;
            if (g_frameCapturing)
                ++g_haloAddsInWindow;
            g_origAddHalo(rdram, ctx, runtime);
        }
        struct OtherDraw
        {
            int path;
            uint32_t textureKey, flags, object;
        };
        std::vector<OtherDraw> g_frameOther;
        template <int N>
        void otherDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing)
            {
                const uint32_t sub = guestAny(rdram, 0x70000034u);
                g_frameOther.push_back({N, sub ? popTextureKey(guestAny(rdram, sub) & 0xFFFF) : 0u, sub ? guestAny(rdram, sub + 4) : 0u,
                                        getRegU32(ctx, 4)});
                if (N == 0)
                {
                    // CJIdxMesh::Display(mesh, matrix index): CALLs the packet at the low 28 bits of the
                    // mesh's first word with the pool matrix, static driver (VU program 0) like CJMeshElement
                    const uint32_t mesh = getRegU32(ctx, 4), index = getRegU32(ctx, 5) & 0xFFFF;
                    const uint32_t packet = guestAny(rdram, mesh) & 0x0FFFFFFFu;
                    std::vector<pop_assets::Batch> batches;
                    livePacketBatches(rdram, packet, kIdxUvScale, batches);
                    std::array<float, 16> model;
                    const uint32_t m = guestAny(rdram, 0x70000260u) + index * 0x40u;
                    for (int k = 0; k < 16; ++k)
                    {
                        const uint32_t bits = guestAny(rdram, m + 4u * k);
                        std::memcpy(&model[k], &bits, 4);
                    }
                    static int logged = 0;
                    if (logged++ < 4)
                        std::fprintf(stderr, "[pop:native] CJIdxMesh %08x packet %08x: %zu batches\n", mesh, packet, batches.size());
                    if (!batches.empty())
                        g_frameRefs.emplace_back(std::move(batches), std::make_pair(model, currentMaterial(rdram)));
                }
            }
            g_other[N].original(rdram, ctx, runtime);
        }

        void readElement(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t element = getRegU32(ctx, 4);
            const uint32_t data = guest32(rdram, getRegU32(ctx, 5)) & (kRam - 1);
            ++g_stats.read;
            pop_assets::Pack pack;
            pack.d = rdram;
            pack.len = kRam;
            std::vector<pop_assets::Batch> batches;
            if (pop_assets::convertElement(pack, data, 4096.0, batches))
            {
                Converted c;
                for (const auto &b : batches)
                {
                    for (const auto &v : b.vertices)
                        c.positions.insert(key3(v.pos));
                    c.vertices += b.vertices.size();
                    c.triangles += b.indices.size() / 3;
                }
                c.batches = batches.size();
                c.meshes = std::move(batches);
                g_stats.converted++;
                g_stats.batches += c.batches;
                g_stats.vertices += c.vertices;
                g_stats.triangles += c.triangles;
                g_elements[element] = std::move(c);
            }
            else
            {
                g_stats.notConverted++;
                g_elements.erase(element);
                pop_mesh::ElementParts parts;
                char why = 'f';
                if (pop_mesh::elementParts(rdram, kRam, data, parts))
                    why = parts.boneCount && !parts.passes.empty() ? 's'
                          : parts.passes.empty() ? (pop_assets::isInstanced(pack, data) ? 'i' : 'n') : 'f';
                g_skipped[element] = why;
                std::vector<pop_assets::SkinnedBatch> skinned;
                if (why == 's' && pop_assets::convertSkinned(pack, data, skinned))
                    g_skinned[element] = std::move(skinned);
            }
            g_origRead(rdram, ctx, runtime);
        }

        // Every V3-32 position the packet at `packet` uploads (DMA chain in guest memory, ending in RET).
        void packetPositions(uint8_t *rdram, uint32_t packet, std::vector<float> &out)
        {
            // The VIF stream: each tag's upper 64 bits (two VIF codes), then its data (inline for cnt,
            // at the tag's address for ref/refs).
            std::vector<uint32_t> words;
            uint32_t pc = packet;
            for (int steps = 0; steps < 4096; ++steps)
            {
                const uint32_t t0 = guest32(rdram, pc), addr = guest32(rdram, pc + 4) & 0x7FFFFFF0u;
                const uint32_t qwc = t0 & 0xFFFF, id = (t0 >> 28) & 7;
                words.push_back(guest32(rdram, pc + 8));
                words.push_back(guest32(rdram, pc + 12));
                uint32_t data = pc + 16, next = pc + 16 + qwc * 16;
                bool stop = false;
                switch (id)
                {
                case 0: data = addr; stop = true; break;       // refe
                case 1: break;                                 // cnt
                case 2: next = addr; break;                    // next
                case 3: case 4: data = addr; next = pc + 16; break; // ref, refs
                default: stop = true; break;                   // call (not used in mesh packets), ret, end
                }
                for (uint32_t k = 0; k < qwc * 4u; ++k)
                    words.push_back(guest32(rdram, data + 4 * k));
                if (stop)
                    break;
                pc = next;
            }
            static const uint8_t kElementBytes[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};
            for (size_t i = 0; i < words.size();)
            {
                const uint32_t w = words[i++];
                const uint32_t cmd = (w >> 24) & 0x7F, num = ((w >> 16) & 0xFF) ? ((w >> 16) & 0xFF) : 256u;
                if (cmd >= 0x60)
                {
                    const uint32_t bytes = kElementBytes[cmd & 0xF] * num, n = (bytes + 3) / 4;
                    if ((cmd & 0xF) == 8 && i + n <= words.size())
                    {
                        const size_t o = out.size();
                        out.resize(o + 3 * num);
                        std::memcpy(&out[o], &words[i], 12 * num);
                    }
                    else if ((cmd & 0xF) == 0xC && (w & 0x3FF) == 3 && i + n <= words.size())
                    {
                        // normal-lit layout: V4-32 positions at VU 0x03 (w = +-1); other V4-32 uploads
                        // (bbox, matrices) go elsewhere
                        for (uint32_t k = 0; k < num; ++k)
                        {
                            float xyz[3];
                            std::memcpy(xyz, &words[i + 4 * k], 12);
                            out.insert(out.end(), xyz, xyz + 3);
                        }
                    }
                    i += n;
                }
                else if (cmd == 0x4A)
                    i += 2 * num;
                else if (cmd == 0x50 || cmd == 0x51)
                    i += 4 * ((w & 0xFFFF) ? (w & 0xFFFF) : 65536u);
                else if (cmd == 0x20)
                    i += 1;
                else if (cmd == 0x30 || cmd == 0x31)
                    i += 4;
            }
        }

        void display(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pass = getRegU32(ctx, 6) & 0xFF, index = getRegU32(ctx, 5) & 0xFFFF;
            uint32_t element = getRegU32(ctx, 4);
            for (unsigned link = 0; element && link < 4096; ++link)
            {
                ++g_stats.draws;
                const auto it = g_elements.find(element);
                const auto sk = g_skinned.find(element);
                if (sk != g_skinned.end() && g_frameCapturing && (pass == 0 || pass == 1 || pass == 4) &&
                    g_frameSeen.insert((uint64_t(element) << 16) | index).second)
                {
                    std::vector<pop_assets::Batch> posed;
                    const LightEnv light = g_noLighting ? LightEnv{} : readLightEnv(rdram);
                    if (poseSkinned(rdram, element, index, sk->second, posed, &light))
                        g_framePosed.emplace_back(std::move(posed), currentMaterial(rdram));
                }
                if (it == g_elements.end())
                {
                    ++g_stats.drawsUnknown;
                    const auto sk = g_skipped.find(element);
                    ++g_unknownBy[sk == g_skipped.end() ? '?' : sk->second];
                }
                else if (pass == 3 && g_frameCapturing)
                {
                    // lightmap pass (CLMElementList::Render): note the current sub-material's texture
                    const uint32_t sub = guestAny(rdram, 0x70000034u);
                    const uint32_t key = sub ? popTextureKey(guestAny(rdram, sub)) : 0u;
                    g_frameLightmap[(uint64_t(element) << 16) | index] = key;
                }
                else if (pass == 0 || pass == 1 || pass == 4)
                {
                    ++g_stats.drawsKnown;
                    const uint32_t packet = guest32(rdram, element + 0x20 + 4 * (pass == 4 ? 1 : pass));
                    std::vector<float> pos;
                    packetPositions(rdram, packet, pos);
                    size_t found = 0;
                    for (size_t k = 0; k + 3 <= pos.size(); k += 3)
                        found += it->second.positions.count(key3(&pos[k]));
                    g_stats.positions += pos.size() / 3;
                    g_stats.positionsFound += found;
                    if (found == pos.size() / 3 && !pos.empty())
                        ++g_stats.drawsExact;
                    if (g_frameCapturing && g_frameSeen.insert((uint64_t(element) << 16) | index).second)
                    {
                        // matrix pool: base pointer at scratchpad 0x70000260, 0x40 bytes per index
                        const uint32_t m = guestAny(rdram, 0x70000260u) + index * 0x40u;
                        std::array<float, 16> model;
                        for (int k = 0; k < 16; ++k)
                        {
                            const uint32_t bits = guestAny(rdram, m + 4u * k);
                            std::memcpy(&model[k], &bits, 4);
                        }
                        g_frameDraws.emplace_back(element, model);
                        // the render state manager's current sub-material (0x70000034, set by
                        // CElementList::Render before the Display call)
                        const uint32_t sub = guestAny(rdram, 0x70000034u);
                        g_frameSub.push_back(sub);
                        g_frameMaterials.push_back(currentMaterial(rdram));
                    }
                }
                element = guest32(rdram, element + 0x4C);
            }
            report();
            g_origDisplay(rdram, ctx, runtime);
        }
    }

    namespace
    {
        // the draw's vertex colours as the VIF delivers them (constant colour / local alpha masks)
        void writeVertices(FILE *f, const std::vector<pop_native::Vertex> &vertices, const DrawMaterial &m)
        {
            if (m.constColour < 0 && m.constAlpha < 0)
            {
                std::fwrite(vertices.data(), sizeof(pop_native::Vertex), vertices.size(), f);
                return;
            }
            std::vector<pop_native::Vertex> v = vertices;
            for (pop_native::Vertex &x : v)
            {
                if (m.constColour >= 0)
                    for (int k = 0; k < 3; ++k)
                        x.colour[k] = float((uint32_t(m.constColour) >> (8 * k)) & 0xFF) / 128.0f;
                if (m.constAlpha >= 0)
                    x.colour[3] = float(m.constAlpha) / 128.0f;
            }
            std::fwrite(v.data(), sizeof(pop_native::Vertex), v.size(), f);
        }

        // File: "PNF1", clip[16], draw count; per draw: model[16], batch count; per batch: vertex
        // count, index count, vertices (pop_native::Vertex), indices (u32).
        std::function<uint32_t(uint32_t)> g_rdramForDump;
        void writeFrame()
        {
            FILE *f = std::fopen(g_framePath.c_str(), "wb");
            if (!f)
                return;
            // "PNF4": clip[16]; images (count; per image: w, h, RGBA); draws in the game's order (count;
            // per draw: model[16], image index (-1: none), render state flags, AREF word, fog a, fog b,
            // FOGCOL, Z test, batch count, batches)
            std::fwrite("PNF4", 1, 4, f);
            std::fwrite(g_frameClip, 4, 16, f);
            std::unordered_map<uint32_t, int32_t> imageOf;
            std::vector<pop_native::Image> images;
            std::vector<DrawMaterial> allMaterials = g_frameMaterials;
            for (const auto &p : g_framePosed)
                allMaterials.push_back(p.second);
            for (const auto &r : g_frameRefs)
                allMaterials.push_back(r.second.second);
            for (const OtherDraw &o : g_frameOther) // other paths' textures too (image list only)
            {
                DrawMaterial m;
                m.textureKey = o.textureKey;
                m.flags = o.flags;
                allMaterials.push_back(m);
            }
            for (const GfxDraw &g : g_frameGfx) // GFX textures too (image list only, for inspection)
            {
                DrawMaterial m;
                m.textureKey = g.textureKey;
                m.flags = g.flags;
                allMaterials.push_back(m);
            }
            for (const DrawMaterial &m : allMaterials)
                if (m.textureKey && !imageOf.count(m.textureKey))
                {
                    pop_native::Image img;
                    imageOf[m.textureKey] = findTexture(m.textureKey, img) ? int32_t(images.size()) : -1;
                    if (imageOf[m.textureKey] >= 0)
                        images.push_back(std::move(img));
                }
            {
                std::unordered_map<uint32_t, int> keys;
                for (const auto &kv : g_frameLightmap)
                    ++keys[kv.second];
                std::fprintf(stderr, "[pop:native] lightmap pass: %zu draws, %zu distinct keys:", g_frameLightmap.size(), keys.size());
                for (const auto &kv : keys)
                {
                    pop_native::Image img;
                    const bool ok = findTexture(kv.first, img);
                    std::fprintf(stderr, " %08x x%d (%s %ux%u)", kv.first, kv.second, ok ? "decoded" : "not found", img.width, img.height);
                }
                std::fprintf(stderr, "\n");
            }
            {
                // unresolved texture keys, and whether each is a bigfile entry of its own
                const char *env = std::getenv("POP_PS2_BIGFILE");
                const std::string bf = env && *env ? env : "DATA/PRINCE.BF";
                int shown = 0;
                for (const auto &kv : imageOf)
                    if (kv.second < 0 && shown++ < 12)
                    {
                        std::vector<uint8_t> entry;
                        const bool own = pop_hdtex::readEntry(bf, kv.first, entry);
                        std::fprintf(stderr, "[pop:native] texture %08x not found in the loaded packs; own bigfile entry: %s (%zu bytes)\n",
                                     kv.first, own ? "yes" : "no", entry.size());
                    }
            }
            const uint32_t ni0 = uint32_t(images.size());
            std::fwrite(&ni0, 4, 1, f);
            for (const auto &img : images)
            {
                std::fwrite(&img.width, 4, 1, f);
                std::fwrite(&img.height, 4, 1, f);
                std::fwrite(img.rgba.data(), 1, img.rgba.size(), f);
            }
            const uint32_t n = uint32_t(g_frameDraws.size() + g_framePosed.size() + g_frameRefs.size());
            std::fwrite(&n, 4, 1, f);
            size_t tris = 0, textured = 0;
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            // every draw in the game's order: (seq, kind 0 static / 1 skinned / 2 instanced, index)
            std::vector<std::array<uint32_t, 3>> order;
            for (size_t i = 0; i < g_frameDraws.size(); ++i)
                order.push_back({g_frameMaterials[i].seq, 0u, uint32_t(i)});
            for (size_t i = 0; i < g_framePosed.size(); ++i)
                order.push_back({g_framePosed[i].second.seq, 1u, uint32_t(i)});
            for (size_t i = 0; i < g_frameRefs.size(); ++i)
                order.push_back({g_frameRefs[i].second.second.seq, 2u, uint32_t(i)});
            std::sort(order.begin(), order.end());
            for (const auto &o : order)
            {
                const float *model = kIdentity;
                const DrawMaterial *matp = nullptr;
                const std::vector<pop_assets::Batch> *batches = nullptr;
                if (o[1] == 0)
                {
                    model = g_frameDraws[o[2]].second.data();
                    matp = &g_frameMaterials[o[2]];
                    batches = &g_elements[g_frameDraws[o[2]].first].meshes;
                }
                else if (o[1] == 1)
                {
                    matp = &g_framePosed[o[2]].second;
                    batches = &g_framePosed[o[2]].first;
                }
                else
                {
                    model = g_frameRefs[o[2]].second.first.data();
                    matp = &g_frameRefs[o[2]].second.second;
                    batches = &g_frameRefs[o[2]].first;
                }
                const DrawMaterial &mat = *matp;
                const int32_t image = mat.textureKey && imageOf.count(mat.textureKey) ? imageOf[mat.textureKey] : -1;
                textured += image >= 0;
                const uint32_t nb = uint32_t(batches->size());
                std::fwrite(model, 4, 16, f);
                std::fwrite(&image, 4, 1, f);
                std::fwrite(&mat.flags, 4, 1, f);
                std::fwrite(&mat.ref, 4, 1, f);
                std::fwrite(&mat.fogA, 4, 1, f);
                std::fwrite(&mat.fogB, 4, 1, f);
                std::fwrite(&mat.fogColour, 4, 1, f);
                std::fwrite(&mat.zTest, 4, 1, f);
                std::fwrite(&nb, 4, 1, f);
                for (const auto &b : *batches)
                {
                    const uint32_t nv = uint32_t(b.vertices.size()), ni = uint32_t(b.indices.size());
                    std::fwrite(&nv, 4, 1, f);
                    std::fwrite(&ni, 4, 1, f);
                    writeVertices(f, b.vertices, mat);
                    std::fwrite(b.indices.data(), 4, ni, f);
                    tris += ni / 3;
                }
            }
            std::fclose(f);
            // side file for identifying the sub-material layout: per draw, the sub-material object's
            // first 0x30 bytes and the key that word +4 maps to as a texture index
            if (FILE *t = std::fopen((g_framePath + ".sub.txt").c_str(), "w"))
            {
                for (size_t i = 0; i < g_frameSub.size(); ++i)
                {
                    const uint32_t sub = g_frameSub[i];
                    std::fprintf(t, "%zu %08x", i, sub);
                    for (uint32_t k = 0; k < 12; ++k)
                        std::fprintf(t, " %08x", sub ? g_rdramForDump(sub + 4 * k) : 0u);
                    std::fprintf(t, " key4 %08x key0 %08x\n", sub ? popTextureKey(g_rdramForDump(sub + 4)) : 0u,
                                 sub ? popTextureKey(g_rdramForDump(sub)) : 0u);
                }
                std::fclose(t);
            }
            {
                // draws per render state blend kind (flags bits 16-19, SetBlending's table)
                std::map<uint32_t, int> kinds;
                for (const DrawMaterial &m : allMaterials)
                    ++kinds[(m.flags >> 16) & 0xF];
                std::fprintf(stderr, "[pop:native] blend kinds:");
                for (const auto &kv : kinds)
                    std::fprintf(stderr, " %u x%d", kv.first, kv.second);
                std::fprintf(stderr, "\n");
            }
            {
                std::map<uint32_t, int> byKey;
                for (const GfxDraw &g : g_frameGfx)
                    ++byKey[g.textureKey];
                std::fprintf(stderr, "[pop:native] GFX draws (FUN_00458FC0, not drawn natively): %zu;", g_frameGfx.size());
                for (const auto &kv : byKey)
                {
                    pop_native::Image img;
                    const bool ok = kv.first && findTexture(kv.first, img);
                    std::fprintf(stderr, " tex %08x (%ux%u, image %d) x%d", kv.first, ok ? img.width : 0u, ok ? img.height : 0u,
                                 imageOf.count(kv.first) ? imageOf[kv.first] : -1, kv.second);
                }
                std::fprintf(stderr, "\n");
                for (size_t i = 0; i < g_frameGfx.size() && i < 24; ++i)
                    std::fprintf(stderr, "[pop:native]   gfx %zu: type %02x element %08x tex %08x flags %08x\n", i, g_frameGfx[i].type,
                                 g_frameGfx[i].element, g_frameGfx[i].textureKey, g_frameGfx[i].flags);
            }
            for (const OtherDraw &o : g_frameOther)
            {
                pop_native::Image img;
                const bool ok = o.textureKey && findTexture(o.textureKey, img);
                std::fprintf(stderr, "[pop:native]   %s: object %08x tex %08x (%ux%u, image %d) flags %08x\n", g_other[o.path].name, o.object,
                             o.textureKey, ok ? img.width : 0u, ok ? img.height : 0u,
                             imageOf.count(o.textureKey) ? imageOf[o.textureKey] : -1, o.flags);
            }
            {
                const uint32_t count = guestAny(g_rdramPtr, 0x0067518Cu);
                std::fprintf(stderr, "[pop:native] halos: %u in the array at the frame's end, %u adds in the window, %u adds in all\n", count,
                             g_haloAddsInWindow, g_haloAdds);
                for (uint32_t h = 0; h < count && h < 32; ++h)
                {
                    const uint32_t e = 0x006DF2C0u + h * 0x20;
                    float v[6];
                    for (int k = 0; k < 6; ++k)
                    {
                        const uint32_t bits = guestAny(g_rdramPtr, e + 4 * k);
                        std::memcpy(&v[k], &bits, 4);
                    }
                    const uint32_t colour = guestAny(g_rdramPtr, e + 0x18), material = guestAny(g_rdramPtr, e + 0x1C);
                    const uint32_t list = material ? guestAny(g_rdramPtr, material + 0x20) : 0;
                    const uint32_t texIndex = list ? guest16(g_rdramPtr, list) : 0xFFFF;
                    std::fprintf(stderr, "[pop:native]   halo %u: pos (%.2f %.2f %.2f) half size %.2f x %.2f colour %08x material %08x tex index %04x key %08x\n",
                                 h, v[0], v[1], v[2], v[4], v[5], colour, material, texIndex,
                                 texIndex != 0xFFFF ? popTextureKey(texIndex) : 0u);
                }
            }
            std::fprintf(stderr, "[pop:native] frame written to %s: %u draws (%zu skinned, %zu instanced, %zu textured, %u images), %zu triangles\n",
                         g_framePath.c_str(), n, g_framePosed.size(), g_frameRefs.size(), textured, ni0, tris);
        }

        void display3D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdramPtr = rdram;
            if (g_frameCapturing)
            {
                g_rdramForDump = [rdram](uint32_t a) { return guestAny(rdram, a); };
                g_frameCapturing = false;
                g_frameDone = true;
                writeFrame();
            }
            if (!g_frameDone && g_frameAfter >= 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count() >= g_frameAfter)
            {
                // world -> clip of the graphics system object (pointer at 0x675148, gp+0x758; +0x240)
                const uint32_t gs = guestAny(rdram, 0x00675148u);
                if (gs)
                {
                    for (int k = 0; k < 16; ++k)
                    {
                        const uint32_t bits = guestAny(rdram, gs + 0x240u + 4u * k);
                        std::memcpy(&g_frameClip[k], &bits, 4);
                    }
                    g_frameCapturing = true;
                }
            }
            g_origDisplay3D(rdram, ctx, runtime);
        }
    }

    // The game looked up bigfile key `key`. For a world pack (0xFF......), load the PS2 pack once.
    void onBigfileLookup(uint32_t key)
    {
        if (!g_live || (key >> 24) != 0xFFu)
            return;
        {
            std::lock_guard<std::mutex> lock(g_packsMutex);
            if (g_packs.count(key))
                return;
            g_packs[key] = nullptr;
        }
        std::thread([key] {
            const char *env = std::getenv("POP_PS2_BIGFILE");
            auto pack = std::make_shared<LoadedPack>();
            if (!pop_hdtex::readEntry(env && *env ? env : "DATA/PRINCE.BF", key, pack->bytes) ||
                !pop_assets::indexPack(pack->bytes.data(), pack->bytes.size(), pack->index))
                return;
            std::lock_guard<std::mutex> lock(g_packsMutex);
            g_packs[key] = std::move(pack);
        }).detach();
    }

    void install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("POP_NATIVE_LIVE");
        if (!v || std::strcmp(v, "1") != 0)
            return;
        g_origRead = runtime.lookupFunction(kReadElement);
        g_origDisplay = runtime.lookupFunction(kMeshDisplay);
        if (!g_origRead || !g_origDisplay)
        {
            std::fprintf(stderr, "[pop:native] hooks not found\n");
            return;
        }
        runtime.replaceFunction(kReadElement, readElement);
        runtime.replaceFunction(kMeshDisplay, display);
        g_live = true;
        g_start = std::chrono::steady_clock::now();
        if (const char *f = std::getenv("POP_NATIVE_FRAME"); f && *f)
        {
            const std::string spec = f;
            const auto colon = spec.rfind(':');
            g_framePath = spec.substr(0, colon);
            g_frameAfter = colon != std::string::npos ? std::atof(spec.c_str() + colon + 1) : 60.0;
            g_origDisplay3D = runtime.lookupFunction(kDisplay3D);
            if (g_origDisplay3D)
                runtime.replaceFunction(kDisplay3D, display3D);
            g_origRefDisplay = runtime.lookupFunction(kRefDisplay);
            if (g_origRefDisplay)
                runtime.replaceFunction(kRefDisplay, refDisplay);
            auto hookOther = [&](int n, PS2Runtime::RecompiledFunction f) {
                g_other[n].original = runtime.lookupFunction(g_other[n].address);
                if (g_other[n].original)
                    runtime.replaceFunction(g_other[n].address, f);
            };
            hookOther(0, otherDisplay<0>);
            hookOther(1, otherDisplay<1>);
            hookOther(2, otherDisplay<2>);
            hookOther(3, otherDisplay<3>);
            g_origAddHalo = runtime.lookupFunction(0x004AB1C0u);
            if (g_origAddHalo)
                runtime.replaceFunction(0x004AB1C0u, addHalo);
            g_origGfx = runtime.lookupFunction(kGfxDisplay);
            if (g_origGfx)
                runtime.replaceFunction(kGfxDisplay, gfxDisplay);
        }
        g_lastReport = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[pop:native] live conversion check on\n");
    }
}
