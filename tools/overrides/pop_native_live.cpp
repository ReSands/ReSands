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
namespace pop_live_client // pop_live_client.cpp: the live native window's frame sender
{
    void start(int port);
    void submit(std::vector<uint8_t> &&frame);
    uint64_t connection(); // changes with every new viewer connection
    bool busy();           // the previous frame is still being sent
    void waitIdle();       // blocks until it is
}
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
#include <deque>
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
            uint32_t type, element, textureKey, flags, list;
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
        int g_liveWindow = 0; // POP_NATIVE_WINDOW=<port>: capture every frame and stream it to the live native window
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
            // GS FRAME FBMSK as FUN_004B2000 sets it: flags 0x60 == 0x40: RGB masked (alpha only);
            // else alpha masked unless flag 0x8 or 0x20, RGB masked with 0x40, plus ~render state
            // 0x70000044 (0xFF000000 around list +0x98: alpha only). The frame's alpha is the glow mask
            // (ColorDiffusion copies Cs * As).
            uint32_t fbmsk = 0xFF000000u;
            uint32_t list = 0;
            uint32_t seq = 0;   // the draw's position in the frame (game draw order)
            // UV matrix of the static driver (VU 0x15/0x16), set by CVuManager::DoComputeAndSetUVMatrix
            // when the sub-material's +8 word is nonzero (UV scroll / rotation; FUN_0044A440):
            // u' = u * m0 + v * m2 + m4, v' = u * m1 + v * m3 + m5
            bool hasUvm = false;
            float uvm[6] = {1, 0, 0, 1, 0, 0};
        };
        uint32_t g_frameSeq = 0;
        const bool g_noUvMatrix = std::getenv("POP_NATIVE_NOUVM") != nullptr; // A/B: no UV animation
        // dedupe key bit for the alpha-only pass (list +0x98, render state 0x44 = 0xFF000000): the game
        // draws mask elements again there, depth-tested against the finished scene
        uint64_t alphaPassBit(uint8_t *rdram);
        // the element list being rendered (CElementList::Render 0x4B9440, a0 - the draw manager given to
        // Display3DElements): EraseAlphaBuffer runs just before list +0x98, so only lists +0x98 and
        // +0xA8 leave alpha (the glow mask) in the frame
        uint32_t g_drawManager = 0, g_currentList = 0;
        PS2Runtime::RecompiledFunction g_origListRender = nullptr;
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
        float guestF(uint8_t *rdram, uint32_t a)
        {
            const uint32_t b = guestAny(rdram, a);
            float f;
            std::memcpy(&f, &b, 4);
            return f;
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
            if (const uint32_t w8 = guestAny(rdram, sub + 8); w8 && !g_noUvMatrix)
            {
                // FUN_0044A440 / 0044A840 / 0044A540, reading the running scroll and angle the game
                // has already advanced for this frame
                const uint32_t wC = guestAny(rdram, sub + 0xC);
                auto asf = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
                float m[6];
                m[0] = asf(w8 & 0xFFFE0000u);
                m[3] = asf(wC & 0xFFFE0000u);
                m[1] = m[2] = 0;
                m[4] = m[0] * asf(w8 << 16);
                m[5] = m[3] * asf(wC << 16);
                if (dm.flags & 0x40000000u)
                    m[4] = guestF(rdram, sub + 0x14);
                if (dm.flags & 0x80000000u)
                    m[5] = guestF(rdram, sub + 0x18);
                float sn, cs;
                if (guest8(rdram, sub + 0x11) & 0x80)
                {
                    const float a = guestF(rdram, sub + 0x1C);
                    sn = std::sin(a);
                    cs = std::cos(a);
                }
                else
                {
                    const uint32_t idx = (((w8 & 0x10000u) >> 9) | ((w8 & 1u) << 6) | ((wC & 0x10000u) >> 11) | ((wC & 1u) << 4)) >> 4;
                    sn = guestF(rdram, 0x004F0930u + idx * 4);
                    cs = guestF(rdram, 0x004F0930u + ((idx + 4) & 15) * 4);
                }
                const float m0 = m[0], m1 = m[1];
                dm.uvm[0] = cs * m0 - sn * m[2];
                dm.uvm[1] = cs * m1 - sn * m[3];
                dm.uvm[2] = cs * m[2] + sn * m0;
                dm.uvm[3] = cs * m[3] + sn * m1;
                dm.uvm[4] = m[4];
                dm.uvm[5] = m[5];
                dm.hasUvm = true;
                static int logged = 0;
                if (logged++ < 8)
                    std::fprintf(stderr, "[pop:native] UV matrix sub %08x flags %08x: %.3f %.3f %.3f %.3f + (%.3f, %.3f)\n", sub, dm.flags, dm.uvm[0],
                                 dm.uvm[1], dm.uvm[2], dm.uvm[3], dm.uvm[4], dm.uvm[5]);
            }
            const uint32_t zone = guestAny(rdram, 0x70000084u) & 0xFF, fa = guestAny(rdram, 0x70000270u + zone * 16 + 8),
                           fb = guestAny(rdram, 0x70000270u + zone * 16 + 12);
            std::memcpy(&dm.fogA, &fa, 4);
            std::memcpy(&dm.fogB, &fb, 4);
            dm.fogColour = guestAny(rdram, 0x70000024u) & 0xFFFFFF;
            dm.zTest = guest8(rdram, 0x70000038u);
            if ((dm.flags & 0x60) == 0x40)
                dm.fbmsk = 0x00FFFFFFu;
            else
            {
                dm.fbmsk = 0;
                if (!(dm.flags & 0x20) && !(dm.flags & 0x8))
                    dm.fbmsk |= 0xFF000000u;
                if (dm.flags & 0x40)
                    dm.fbmsk |= 0x00FFFFFFu;
                dm.fbmsk |= ~guestAny(rdram, 0x70000044u);
            }
            // EraseAlphaBuffer runs just before list +0x98, the only list drawn with render state 0x44 =
            // 0xFF000000 (alpha only): earlier alpha writes do not survive, so only that pass makes the mask
            if (guestAny(rdram, 0x70000044u) != 0xFF000000u)
                dm.fbmsk |= 0xFF000000u;
            dm.list = g_currentList;
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
        uint64_t alphaPassBit(uint8_t *rdram) { return guestAny(rdram, 0x70000044u) == 0xFF000000u ? uint64_t(1) << 62 : 0; }
        LightEnv readLightEnv(uint8_t *rdram)
        {
            LightEnv e;
            const uint32_t table = guestAny(rdram, 0x700004F0u);
            if (!table)
                return e;
            const uint32_t set = table + (guestAny(rdram, 0x70000088u) & 0xFF) * 0xF0u;
            uint32_t ambient = guestAny(rdram, set + 4);
            int count = int(guest8(rdram, set + 8));
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
                // As DoSetSkinLightEnv uploads them: for point-light sets (+1 != 0) it takes its other path
                // (retail 0x4B6A40) and reads `count` slots as directional lights without looking at their
                // types: vector at +0x10 + 16i (not normalised), colour bytes at +0xD0 + 4i (slots with a
                // zero colour word skipped). The title scene's set 0 holds a position and floats there; the
                // game lights with them as they are (the backlit curtains come out bright, as in the GS frame).
                for (int i = 0; i < count && i < 4; ++i)
                {
                    if (guestAny(rdram, set + 0xD0 + 4 * i) == 0)
                        continue;
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
        uint64_t g_skinnedNoPalette = 0; // skinned draws without a bone palette (posed by the object matrix)
        uint64_t g_passDraws[8][2] = {};  // CJMeshElement::Display calls per pass, static / skinned
        uint64_t g_refPassDraws[8] = {};   // CJMeshElementRef::Display calls per pass
        bool poseSkinned(uint8_t *rdram, uint32_t element, uint32_t index, const std::vector<pop_assets::SkinnedBatch> &bind,
                         std::vector<pop_assets::Batch> &out, const LightEnv *light = nullptr)
        {
            const uint32_t pal = guestAny(rdram, element + 0x48);
            const uint32_t n = pal ? guest8(rdram, pal) : 0;
            std::vector<std::array<float, 16>> bones(std::max(n, 1u));
            if (!pal)
            {
                // no bone palette: one rigid part, placed by the object's matrix-pool entry (as static
                // elements); these were dropped before
                ++g_skinnedNoPalette;
                const uint32_t m = guestAny(rdram, 0x70000260u) + (index & 0xFFFF) * 0x40u;
                for (int j = 0; j < 16; ++j)
                {
                    const uint32_t bits = guestAny(rdram, m + 4u * j);
                    std::memcpy(&bones[0][j], &bits, 4);
                }
            }
            else
            {
                const uint32_t base = guestAny(rdram, 0x700001D0u), baseIndex = guest16(rdram, 0x7000025Eu);
                const uint32_t first = guest16(rdram, 0x700001DCu + ((index & 0xFFFF) - baseIndex) * 2u);
                for (uint32_t k = 0; k < bones.size(); ++k)
                {
                    const uint32_t m = base + (first + guest8(rdram, pal + 4 + k)) * 0x40u;
                    for (int j = 0; j < 16; ++j)
                    {
                        const uint32_t bits = guestAny(rdram, m + 4u * j);
                        std::memcpy(&bones[k][j], &bits, 4);
                    }
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
            uint64_t drawsFromPacket = 0, drawsLightmap = 0; // unknown at load (save-state scenes) and lightmap draws, from the pass packets
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
            std::fprintf(stderr, "[pop:native] skinned draws without a bone palette (object matrix used): %llu; draws per pass (static/skinned):",
                         (unsigned long long)g_skinnedNoPalette);
            for (int pp = 0; pp < 8; ++pp)
                if (g_passDraws[pp][0] || g_passDraws[pp][1])
                    std::fprintf(stderr, " %d: %llu/%llu", pp, (unsigned long long)g_passDraws[pp][0], (unsigned long long)g_passDraws[pp][1]);
            std::fprintf(stderr, "; instanced per pass:");
            for (int pp = 0; pp < 8; ++pp)
                if (g_refPassDraws[pp])
                    std::fprintf(stderr, " %d: %llu", pp, (unsigned long long)g_refPassDraws[pp]);
            std::fprintf(stderr, "\n");
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
            static int unpackLogs = std::getenv("POP_NATIVE_UNPACKLOG") ? 6 : 0;
            std::string unpackLog;
            // A batch is the group of uploads between two uploads of the same format: packets send a
            // batch's V3-16 UVs, V3-32 positions and V4-8 colours (and a V4-32 x3 bounding box) in
            // either order, e.g. bbox, UVs, positions, colours (the palm fronds of the title scene)
            // or colours, UVs, positions, bbox. Closing each batch at its positions upload gave the
            // first order the previous batch's colours, and white for the first batch.
            std::vector<uint8_t> uv, col, pos;
            uint32_t posNum = 0;
            unsigned have = 0; // bit per format seen in the current group (1 << format)
            auto flush = [&]() {
                if (posNum)
                {
                    const uint32_t num = posNum;
                    pop_assets::Batch b;
                    b.vertices.resize(num);
                    std::vector<int32_t> flags(num, 0);
                    for (uint32_t v = 0; v < num; ++v)
                    {
                        pop_native::Vertex &vx = b.vertices[v];
                        std::memcpy(vx.pos, &pos[12 * v], 12);
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
                // UVs and colours stay, as they stay in VU memory: a batch that uploads none uses the
                // previous batch's (the flat curtain panels when they were cleared)
                pos.clear();
                posNum = 0;
                have = 0;
            };
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
                    const unsigned fmt = cmd & 0xF;
                    if (unpackLogs > 0)
                        unpackLog += " " + std::to_string(fmt) + "@" + std::to_string(w & 0x3FF) + "x" + std::to_string(num);
                    if (fmt == 8 || fmt == 9 || fmt == 0xC || fmt == 0xE)
                    {
                        if (have & (1u << fmt))
                            flush();
                        have |= 1u << fmt;
                        if (fmt == 9)
                            uv.assign(raw, raw + bytes);
                        else if (fmt == 0xE)
                            col.assign(raw, raw + bytes);
                        else if (fmt == 8)
                        {
                            pos.assign(raw, raw + bytes);
                            posNum = num;
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
            flush();
            if (unpackLogs > 0 && !unpackLog.empty())
            {
                --unpackLogs;
                std::fprintf(stderr, "[pop:unpack] packet %08x (format@addr x num):%s\n", packet, unpackLog.c_str());
            }
        }

        std::vector<std::pair<std::vector<pop_assets::Batch>, std::pair<std::array<float, 16>, DrawMaterial>>> g_frameRefs; // instanced draws
        std::unordered_map<uint64_t, std::vector<pop_assets::Batch>> g_fromPacket; // (element << 8 | pass) -> decoded pass packet

        // CJMeshElementRef::Display(ref, matrix index, pass): the instance's pass packet is the word
        // at ref + 4 * pass (pass 4: 0; low 4 bits masked); the object's next ref is at +0x1C.
        void refDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_refPassDraws[getRegU32(ctx, 6) & 7];
            if (g_frameCapturing)
            {
                const uint32_t pass = getRegU32(ctx, 6) & 0xFF, index = getRegU32(ctx, 5) & 0xFFFF;
                uint32_t ref = getRegU32(ctx, 4);
                // passes 0, 1, 4 (main) and 2 (second layer); 3 is the lightmap pass
                for (unsigned link = 0; ref && link < 4096 && (pass == 0 || pass == 1 || pass == 2 || pass == 4); ++link)
                {
                    if (g_frameSeen.insert((uint64_t(ref) << 16) | index | (uint64_t(1) << 63) | alphaPassBit(rdram)).second)
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

        // GFX particles, FUN_004584C0(element) (called by FUN_00458FC0 once the render state, fog and
        // material are set): element +0x14 -> particle object P; description D = *P (+0x14 flags,
        // +0x60/+0x68 axis sizes, +0xE0..+0xEC colour keys, +0xF0/+0xF1 sprite-sheet columns/rows,
        // +0xF5/+0xF6 axis choice); generators = P[10] (array, count), 0xD0 bytes each: +0xC2 u16
        // particle count, +0xC4 particles, 0x30 bytes each: +0 flags (bits 8-14 sheet frame), +4 xyz
        // position, +0x20 * +0x24 life, +0x28/+0x2C size. Billboard mode only (description flag
        // 0x1000 clear, first particle without flag 8): quad = position +- axis1 * sx +- axis2 * sy,
        // axes from PAG_GetGeneratorAxis (0x459840): camera right/up (*(gp+0x110) +0x160/+0x170)
        // unless the description's flags pick the generator matrix rows; colour = lerp of the keys
        // over the life (FUN_00460100), alpha faded (FUN_00460040). Calibrated against the GS frame.
        PS2Runtime::RecompiledFunction g_origParticles = nullptr;
        const float kParticleHalf = std::getenv("POP_PARTICLE_FULL") ? 1.0f : 0.5f; // size = full or half extent
        uint32_t lerpColour(float t, uint32_t a, uint32_t b)
        {
            if (t >= 0.98f)
                return b;
            if (t <= 0.02f)
                return a;
            uint32_t o = 0;
            for (int k = 0; k < 4; ++k)
            {
                const float ca = float((a >> (8 * k)) & 0xFF), cb = float((b >> (8 * k)) & 0xFF);
                o |= uint32_t(int(ca + (cb - ca) * t) & 0xFF) << (8 * k);
            }
            return o;
        }
        void particles(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing)
            {
                const uint32_t element = getRegU32(ctx, 4);
                const uint32_t p1 = guestAny(rdram, element + 0x14);
                const uint32_t P = p1 ? guestAny(rdram, p1) : 0;
                const uint32_t D = P ? guestAny(rdram, P) : 0;
                const uint32_t gens = P ? guestAny(rdram, P + 0x28) : 0;
                if (D && gens)
                {
                    const uint32_t flags = guestAny(rdram, D + 0x14), genArray = guestAny(rdram, gens), genCount = guestAny(rdram, gens + 4);
                    const uint32_t firstParts = genCount ? guestAny(rdram, genArray + 0xC4) : 0;
                    // billboards: one pair of axes for every particle; oriented particles (flag 0x1000, or
                    // particle flag 8 on the first one): axes per particle (size, rotation, quaternion)
                    const bool billboard = !(flags & 0x1000) && !(firstParts && (guestAny(rdram, firstParts) & 8));
                    static const bool noOriented = std::getenv("POP_NATIVE_NOORIENTED") != nullptr;
                    if (billboard || !noOriented)
                    {
                        // axis choice (PAG_GetGeneratorAxis, the common branch)
                        const uint32_t cam = guestAny(rdram, 0x006749F0u + 0x110u);
                        uint32_t g0 = genArray, src = guestAny(rdram, g0 + 0x40) ? g0 : guestAny(rdram, P + 0xC);
                        uint32_t ax1 = cam + 0x160, ax2 = cam + 0x170;
                        if (flags & 0x4000000)
                            ax1 = src + 0x10, ax2 = src + 0x20;
                        else if (flags & 0x80000)
                            ax1 = src, ax2 = src + 0x20;
                        else if (flags & 0x8000000)
                            ax1 = src, ax2 = src + 0x10;
                        // PAG_GetGeneratorAxis: fixed sizes (D+0x60 == D+0x64, D+0x68 == D+0x6C, no flag 1)
                        // give axes scaled by them; otherwise the raw axes, scaled per particle
                        const float s1 = guestF(rdram, D + 0x60), s2 = (flags & 4) ? s1 : guestF(rdram, D + 0x68);
                        const bool fixedSize = guestF(rdram, D + 0x64) == s1 && guestF(rdram, D + 0x6C) == guestF(rdram, D + 0x68) && !(flags & 1);
                        const float k1 = fixedSize ? s1 : 1.0f, k2 = fixedSize ? s2 : 1.0f;
                        float a1[3], a2[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            a1[k] = guestF(rdram, ax1 + 4 * k) * k1;
                            a2[k] = guestF(rdram, (flags & 4) ? ax1 + 4 * k : ax2 + 4 * k) * k2;
                        }
                        const uint32_t cols = std::max(1u, guest8(rdram, D + 0xF0)), rows = std::max(1u, guest8(rdram, D + 0xF1));
                        const uint32_t cA = guestAny(rdram, D + 0xE0), cB = guestAny(rdram, D + 0xE4), cC = guestAny(rdram, D + 0xE8),
                                       cD = guestAny(rdram, D + 0xEC);
                        pop_assets::Batch b;
                        for (uint32_t g = 0; g < genCount && g < 64; ++g)
                        {
                            const uint32_t gen = genArray + g * 0xD0, count = guest16(rdram, gen + 0xC2), parts = guestAny(rdram, gen + 0xC4);
                            for (uint32_t i = 0; parts && i < count && i < 4096; ++i)
                            {
                                const uint32_t q = parts + i * 0x30, pf = guestAny(rdram, q);
                                const float life = guestF(rdram, q + 0x20) * guestF(rdram, q + 0x24);
                                float sx = guestF(rdram, q + 0x28), sy = guestF(rdram, q + 0x2C);
                                uint32_t colour;
                                if (!(pf & 1))
                                {
                                    if (!(pf & 2))
                                        colour = lerpColour(life, cB, cA);
                                    else
                                    {
                                        const uint32_t c7 = lerpColour(life, cD, cB);
                                        colour = c7;
                                        if (flags & 8)
                                        {
                                            const uint32_t faded = lerpColour(life, 0, c7); // c7 * life (FUN_00460040)
                                            colour = (flags & 2) ? faded : ((faded & 0xFF000000u) | (c7 & 0xFFFFFF));
                                        }
                                        if (flags & 0x10)
                                            sx *= life, sy *= life;
                                        else if (flags & 0x20)
                                        {
                                            const float k = (1.0f - life) * guestF(rdram, D + 0x88) + 1.0f;
                                            sx *= k, sy *= k;
                                        }
                                    }
                                }
                                else
                                {
                                    const float t = 1.0f - life;
                                    const uint32_t c7 = lerpColour(t, cC, cA);
                                    colour = c7;
                                    if (flags & 0x100)
                                    {
                                        const uint32_t faded = lerpColour(t, 0, c7);
                                        colour = (flags & 2) ? faded : ((faded & 0xFF000000u) | (c7 & 0xFFFFFF));
                                    }
                                    if (flags & 0x200)
                                        sx *= t, sy *= t;
                                }
                                const float px = guestF(rdram, q + 4), py = guestF(rdram, q + 8), pz = guestF(rdram, q + 12);
                                float u0 = 0, v0 = 0, du = 1, dv = 1;
                                if (cols * rows >= 2)
                                {
                                    const uint32_t idx = (pf >> 8) & 0x7F;
                                    du = 1.0f / cols;
                                    dv = 1.0f / rows;
                                    u0 = float(idx % cols) * du;
                                    // FUN_004584C0 sends the sheet row as rows - 1 - idx / cols (counted from the
                                    // bottom); POP_PARTICLE_ROWTOP=1 = the old top-down reading
                                    static const bool rowTop = std::getenv("POP_PARTICLE_ROWTOP") != nullptr;
                                    v0 = float(rowTop ? idx / cols : (rows - 1) - idx / cols) * dv;
                                }
                                // the quad's two half-axes: billboards = the shared axes times the particle's
                                // size; oriented (FUN_004584C0's other branch): the axes times the size (or
                                // pre-scaled for fixed sizes), turned by the particle's angle (+0x1C, sine
                                // table at 0x6D3400), or the rows of its quaternion (+0x10) for flag 8
                                float e1[3], e2[3];
                                for (int k = 0; k < 3; ++k)
                                {
                                    e1[k] = a1[k] * ((billboard || !fixedSize) ? sx : 1.0f);
                                    e2[k] = a2[k] * ((billboard || !fixedSize) ? sy : 1.0f);
                                }
                                if (!billboard)
                                {
                                    const uint32_t rot = guestAny(rdram, q + 0x1C);
                                    if (rot)
                                    {
                                        const uint32_t idx = rot >> 22;
                                        const float sn = guestF(rdram, 0x006D3400u + 4 * idx), cs = guestF(rdram, 0x006D3400u + 4 * ((idx + 0x100) % 0x3FF));
                                        float r1[3], r2[3];
                                        for (int k = 0; k < 3; ++k)
                                        {
                                            r1[k] = e1[k] * cs + sn * e2[k];  // FUN_00495D60: out = a + s * b
                                            r2[k] = e2[k] * cs - sn * e1[k];
                                        }
                                        std::memcpy(e1, r1, sizeof e1);
                                        std::memcpy(e2, r2, sizeof e2);
                                    }
                                    if (pf & 8)
                                    {
                                        // MATH_ConvertQuaternionToMatrix (x, y, z, w at +0x10): rows 0 and 1
                                        const float qx = guestF(rdram, q + 0x10), qy = guestF(rdram, q + 0x14), qz = guestF(rdram, q + 0x18),
                                                    qw = guestF(rdram, q + 0x1C);
                                        const float m0[3] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw)};
                                        const float m1[3] = {2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw)};
                                        const float qsx = guestF(rdram, q + 0x28), qsy = guestF(rdram, q + 0x2C);
                                        for (int k = 0; k < 3; ++k)
                                        {
                                            e1[k] = m0[k] * qsx;
                                            e2[k] = m1[k] * qsy;
                                        }
                                    }
                                }
                                const uint32_t base = uint32_t(b.vertices.size());
                                const float cx[4] = {-1, 1, 1, -1}, cy[4] = {-1, -1, 1, 1};
                                for (int c = 0; c < 4; ++c)
                                {
                                    pop_native::Vertex v{};
                                    const float hx = cx[c] * kParticleHalf, hy = cy[c] * kParticleHalf;
                                    v.pos[0] = px + e1[0] * hx + e2[0] * hy;
                                    v.pos[1] = py + e1[1] * hx + e2[1] * hy;
                                    v.pos[2] = pz + e1[2] * hx + e2[2] * hy;
                                    v.uv[0] = u0 + (cx[c] > 0 ? du : 0);
                                    v.uv[1] = v0 + (cy[c] > 0 ? 0 : dv);
                                    for (int k = 0; k < 4; ++k)
                                        v.colour[k] = float((colour >> (8 * k)) & 0xFF) / 128.0f;
                                    b.vertices.push_back(v);
                                }
                                b.indices.insert(b.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                            }
                        }
                        static int logged = 0, loggedOriented = 0;
                        if (billboard ? logged++ < 6 : loggedOriented++ < 6)
                            std::fprintf(stderr, "[pop:native] particles element %08x (%s): %u generators, %zu quads, flags %08x sheet %ux%u sizes %.2f/%.2f%s\n",
                                         element, billboard ? "billboard" : "oriented", genCount, b.vertices.size() / 4, flags, cols, rows, s1, s2,
                                         fixedSize ? " fixed" : "");
                        if (!b.indices.empty())
                        {
                            // the particles' matrix: pool entry 0x70000090 (FUN_00458FC0 sets it from the list entry)
                            std::array<float, 16> model;
                            const uint32_t m = guestAny(rdram, 0x70000260u) + (guestAny(rdram, 0x70000090u) & 0xFFFF) * 0x40u;
                            for (int k = 0; k < 16; ++k)
                                model[k] = guestF(rdram, m + 4u * k);
                            if (std::getenv("POP_PARTICLE_WORLD"))
                                model = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            static const bool partLog = std::getenv("POP_NATIVE_PARTLOG") != nullptr;
                            if (partLog)
                            {
                                const auto &v0 = b.vertices[0];
                                std::fprintf(stderr, "[pop:part] el %08x matrix %u at (%.1f %.1f %.1f): first quad (%.2f %.2f %.2f) colour %.2f %.2f %.2f %.2f\n",
                                             element, guestAny(rdram, 0x70000090u) & 0xFFFF, model[12], model[13], model[14], v0.pos[0], v0.pos[1],
                                             v0.pos[2], v0.colour[0], v0.colour[1], v0.colour[2], v0.colour[3]);
                            }
                            std::vector<pop_assets::Batch> bs;
                            bs.push_back(std::move(b));
                            DrawMaterial dm = currentMaterial(rdram);
                            dm.hasUvm = false; // particle program (VU 4): sprite-sheet UVs of its own
                            g_frameRefs.emplace_back(std::move(bs), std::make_pair(model, dm));
                        }
                    }
                }
            }
            g_origParticles(rdram, ctx, runtime);
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
                                      sub ? popTextureKey(guestAny(rdram, sub) & 0xFFFF) : 0u, sub ? guestAny(rdram, sub + 4) : 0u, g_currentList});
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
        // CAfterEffect::ColorDiffusion (retail 0x4AE100; prototype 0x46F680): (this, ?, levels, FIX,
        // ?, mask, ?, convolution, strength on the stack) - the glow
        PS2Runtime::RecompiledFunction g_origGlow = nullptr;
        uint32_t g_glowCalls = 0, g_glow[5] = {};
        void colorDiffusion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing || g_frameDone)
            {
                if (!g_glowCalls || g_frameCapturing)
                {
                    g_glow[0] = getRegU32(ctx, 6);
                    g_glow[1] = getRegU32(ctx, 7);
                    g_glow[2] = getRegU32(ctx, 9);
                    g_glow[3] = getRegU32(ctx, 11);
                    g_glow[4] = guestAny(rdram, getRegU32(ctx, 29));
                }
                if (g_glowCalls++ < 4)
                    std::fprintf(stderr, "[pop:native] ColorDiffusion levels %u FIX %u mask %u conv %u strength %u (a1 %08x t0 %08x t2 %08x)\n",
                                 g_glow[0], g_glow[1], g_glow[2], g_glow[3], g_glow[4], getRegU32(ctx, 5), getRegU32(ctx, 8), getRegU32(ctx, 10));
            }
            g_origGlow(rdram, ctx, runtime);
        }
        // CAfterEffect::AE_ProcessSmartBlur (retail 0x4A93C0; prototype 0x466240): the haze. Reads only
        // at entry (code after calling the original may run mid-call on the SotC runtime). It draws
        // when +0x128 == 0 or a fade runs (+0x140); colour/strength at +0x124, fade factor at +0x144
        // (updated by the function itself first: approximated from +0x14C/+0x148 as it does).
        PS2Runtime::RecompiledFunction g_origHaze = nullptr;
        uint32_t g_hazeCalls = 0, g_haze[2] = {}; // colour, fade (float bits)
        void smartBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing || g_frameDone)
            {
                const uint32_t self = getRegU32(ctx, 4);
                const uint32_t fading = guestAny(rdram, self + 0x140u), off = guestAny(rdram, self + 0x128u);
                if ((!g_hazeCalls || g_frameCapturing) && (off == 0u || fading != 0u))
                {
                    float fade = 0.0f;
                    if (fading)
                    {
                        const int32_t n = int32_t(guestAny(rdram, self + 0x14Cu)), total = int32_t(guestAny(rdram, self + 0x148u));
                        if (total != 0 && n != total)
                        {
                            const float t = 1.0f - float(n) / float(total);
                            fade = t * t;
                        }
                    }
                    g_haze[0] = guestAny(rdram, self + 0x124u);
                    std::memcpy(&g_haze[1], &fade, 4);
                    if (g_hazeCalls++ < 4)
                        std::fprintf(stderr, "[pop:native] SmartBlur colour %08x fade %.3f\n", g_haze[0], double(fade));
                }
            }
            g_origHaze(rdram, ctx, runtime);
        }
        struct OtherDraw
        {
            int path;
            uint32_t textureKey, flags, object, list;
        };
        std::vector<OtherDraw> g_frameOther;
        template <int N>
        void otherDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing)
            {
                const uint32_t sub = guestAny(rdram, 0x70000034u);
                g_frameOther.push_back({N, sub ? popTextureKey(guestAny(rdram, sub) & 0xFFFF) : 0u, sub ? guestAny(rdram, sub + 4) : 0u,
                                        getRegU32(ctx, 4), g_currentList});
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
                if (g_frameCapturing && !g_elements.count(element) && !g_skipped.count(element) && !g_skinned.count(element))
                {
                    // Skinned, never seen at load (a save-state scene): the drawn pass's packet in memory
                    // (+0x20 + 4 * pass, pass 4 -> 1) holds the bind-space batches inline (cnt tags up to the
                    // RET), as in the file. Characters often have no pass-0 packet, only pass 1.
                    const uint32_t palette = guest32(rdram, element + 0x48);
                    const uint32_t packet = (pass == 0 || pass == 1 || pass == 4)
                                                ? guest32(rdram, element + 0x20 + 4 * (pass == 4 ? 1 : pass)) & 0x01FFFFF0u
                                                : 0u;
                    if (palette && guest8(rdram, palette) && packet)
                    {
                        uint32_t pc = packet;
                        for (int steps = 0; steps < 4096 && pc + 16 <= kRam; ++steps)
                        {
                            const uint32_t t0 = guest32(rdram, pc), id = (t0 >> 28) & 7;
                            pc += 16 + (id == 1 || id == 6 || id == 7 ? (t0 & 0xFFFF) * 16 : 0);
                            if (id != 1)
                                break;
                        }
                        std::vector<pop_assets::SkinnedBatch> skinned;
                        const bool ok = pop_assets::skinnedFromPacket(rdram, kRam, packet, pc - packet, skinned);
                        static const bool skinLog = std::getenv("POP_NATIVE_SKINLOG") != nullptr;
                        if (skinLog)
                            std::fprintf(stderr, "[pop:skin] el %08x from memory: packet %08x, %u bytes, %s, %zu batches\n", element, packet,
                                         pc - packet, ok ? "decoded" : "FAILED", skinned.size());
                        if (ok)
                            g_skinned[element] = std::move(skinned);
                        else
                            g_skipped[element] = 's';
                    }
                }
                ++g_passDraws[pass & 7][g_skinned.count(element) ? 1 : 0];
                const auto it = g_elements.find(element);
                const auto sk = g_skinned.find(element);
                if (sk != g_skinned.end() && g_frameCapturing && (pass == 0 || pass == 1 || pass == 4) &&
                    g_frameSeen.insert((uint64_t(element) << 16) | index | alphaPassBit(rdram)).second)
                {
                    std::vector<pop_assets::Batch> posed;
                    const LightEnv light = g_noLighting ? LightEnv{} : readLightEnv(rdram);
                    if (poseSkinned(rdram, element, index, sk->second, posed, &light))
                    {
                        static const bool skinLog = std::getenv("POP_NATIVE_SKINLOG") != nullptr;
                        if (skinLog)
                        {
                            // per skinned draw: normals or colours, the light set, the mean vertex colour
                            double mean[3] = {0, 0, 0}, at[3] = {0, 0, 0};
                            size_t nv = 0, withNormals = 0;
                            for (const auto &b : sk->second)
                                withNormals += b.normals.empty() ? 0 : 1;
                            for (const auto &b : posed)
                                for (const auto &v : b.vertices)
                                {
                                    for (int c = 0; c < 3; ++c)
                                    {
                                        mean[c] += v.colour[c];
                                        at[c] += v.pos[c];
                                    }
                                    ++nv;
                                }
                            if (nv)
                                std::fprintf(stderr, "[pop:skin] el %08x idx %u: %zu vertices around (%.1f, %.1f, %.1f)\n", element, index, nv,
                                             at[0] / nv, at[1] / nv, at[2] / nv);
                            const uint32_t set = guestAny(rdram, 0x700004F0u) + (guestAny(rdram, 0x70000088u) & 0xFF) * 0xF0u;
                            std::fprintf(stderr,
                                         "[pop:skin] el %08x idx %u pass %u batches %zu normals %zu | set %u amb %08x n %u pt %u | const %u %08x | "
                                         "lit %d amb %.0f %.0f %.0f lights %d | mean colour %.3f %.3f %.3f\n",
                                         element, index, pass, sk->second.size(), withNormals, guestAny(rdram, 0x70000088u) & 0xFF,
                                         guestAny(rdram, set + 4), guest8(rdram, set + 8), guest8(rdram, set + 1), guest8(rdram, 0x70000039u),
                                         guestAny(rdram, 0x7000003Cu), int(light.valid), double(light.amb[0]), double(light.amb[1]),
                                         double(light.amb[2]), light.count, nv ? mean[0] / nv : 0, nv ? mean[1] / nv : 0, nv ? mean[2] / nv : 0);
                            static int setDumps = 0;
                            if (setDumps++ < 3)
                                for (int l = 0; l < 4; ++l)
                                    std::fprintf(stderr, "[pop:skin]   light %d type %08x colour %08x dir/pos %.3f %.3f %.3f %.3f\n", l,
                                                 guestAny(rdram, set + 0xE0 + 4 * l), guestAny(rdram, set + 0xD0 + 4 * l), double(guestF(rdram, set + 0x10 + 16 * l)),
                                                 double(guestF(rdram, set + 0x14 + 16 * l)), double(guestF(rdram, set + 0x18 + 16 * l)),
                                                 double(guestF(rdram, set + 0x1C + 16 * l)));
                        }
                        DrawMaterial dm = currentMaterial(rdram);
                        dm.hasUvm = false; // the skin driver has no UV matrix
                        g_framePosed.emplace_back(std::move(posed), dm);
                    }
                }
                if (pass == 3 && g_frameCapturing)
                {
                    // Lightmap pass (CLMElementList::Render 0x4B7B80): FUN_004B2000(state, texture index,
                    // 0x30284, 0) leaves the lightmap's texture index at render state 0x70000008 and the
                    // flags at 0x70000000 (blend kind 3 with 0x80: GS ALPHA 0x46 = Cd * (1 - As), no
                    // depth write). The pass-3 packet (element +0x2C) uploads V3-16 lightmap UVs (ITOF15,
                    // 1/32768) and V3-32 positions, no colours: the lightmap's alpha darkens the scene.
                    const uint32_t key = popTextureKey(guestAny(rdram, 0x70000008u) & 0xFFFFu);
                    g_frameLightmap[(uint64_t(element) << 16) | index] = key;
                    const uint32_t packet = guest32(rdram, element + 0x2C) & ~0xFu;
                    auto &batches = g_fromPacket[(uint64_t(element) << 8) | 3];
                    if (batches.empty() && packet)
                        livePacketBatches(rdram, packet, 32768.0, batches);
                    if (!batches.empty() && key && g_frameSeen.insert((uint64_t(element) << 16) | index | (uint64_t(3) << 56)).second)
                    {
                        DrawMaterial dm;
                        dm.seq = g_frameSeq++;
                        dm.textureKey = key;
                        dm.flags = guestAny(rdram, 0x70000000u);
                        dm.fbmsk = 0xFF000000u; // flags 0x284: colour written, alpha kept
                        dm.list = g_currentList;
                        const uint32_t m = guestAny(rdram, 0x70000260u) + index * 0x40u;
                        std::array<float, 16> model;
                        for (int k = 0; k < 16; ++k)
                            model[k] = guestF(rdram, m + 4u * k);
                        g_frameRefs.emplace_back(batches, std::make_pair(model, dm));
                        ++g_stats.drawsLightmap;
                    }
                }
                else if (it == g_elements.end())
                {
                    ++g_stats.drawsUnknown;
                    const auto sk = g_skipped.find(element);
                    ++g_unknownBy[sk == g_skipped.end() ? '?' : sk->second];
                    // Never seen at load (a save-state load runs no ReadElement): decode the pass packet
                    // from guest memory as sent, like pass 2 and CJIdxMesh. Static elements only (a
                    // skinned element has a bone palette at +0x48); the decode is kept per (element,
                    // pass), the packets do not change after load.
                    const uint32_t palette = guest32(rdram, element + 0x48);
                    if (sk == g_skipped.end() && !(palette && guest8(rdram, palette)) && g_frameCapturing &&
                        (pass == 0 || pass == 1 || pass == 4) &&
                        g_frameSeen.insert((uint64_t(element) << 16) | index | alphaPassBit(rdram)).second)
                    {
                        const uint32_t packet = guest32(rdram, element + 0x20 + 4 * (pass == 4 ? 1 : pass)) & ~0xFu;
                        auto &batches = g_fromPacket[(uint64_t(element) << 8) | pass];
                        if (batches.empty() && packet)
                            livePacketBatches(rdram, packet, 512.0, batches);
                        if (!batches.empty())
                        {
                            const uint32_t m = guestAny(rdram, 0x70000260u) + index * 0x40u;
                            std::array<float, 16> model;
                            for (int k = 0; k < 16; ++k)
                                model[k] = guestF(rdram, m + 4u * k);
                            g_frameRefs.emplace_back(batches, std::make_pair(model, currentMaterial(rdram)));
                            ++g_stats.drawsFromPacket;
                        }
                    }
                }
                else if (pass == 2 && g_frameCapturing && !g_skinned.count(element) &&
                         g_frameSeen.insert((uint64_t(element) << 16) | index | (uint64_t(2) << 56) | alphaPassBit(rdram)).second)
                {
                    // pass 2 (a material's second layer, CJMeshElement::Display with the constant
                    // colour / local alpha of ExtractLocalAlphaAndConstantColor): its own packet at
                    // element +0x28, decoded from guest memory with its UVs and colours as sent
                    ++g_stats.drawsKnown;
                    const uint32_t packet = guest32(rdram, element + 0x28) & ~0xFu;
                    std::vector<pop_assets::Batch> batches;
                    if (packet)
                        livePacketBatches(rdram, packet, 512.0, batches);
                    if (!batches.empty())
                    {
                        const uint32_t m = guestAny(rdram, 0x70000260u) + index * 0x40u;
                        std::array<float, 16> model;
                        for (int k = 0; k < 16; ++k)
                            model[k] = guestF(rdram, m + 4u * k);
                        g_frameRefs.emplace_back(std::move(batches), std::make_pair(model, currentMaterial(rdram)));
                    }
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
                    if (g_frameCapturing && g_frameSeen.insert((uint64_t(element) << 16) | index | alphaPassBit(rdram)).second)
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
        // the GS picture the runtime last presented (frame N when frame N + 1's capture ends), for the
        // live window's comparison toggle. Not every runtime has copyLatchedHostPresentationFrame (the
        // Mac `pop` one does; the PC's GPU renderer keeps its frames on the GPU), hence the detection.
        PS2Runtime *g_runtimePtr = nullptr;
        template <typename G>
        auto copyGsFrame(G &gs, std::vector<uint8_t> &px, uint32_t &w, uint32_t &h, int) -> decltype(gs.copyLatchedHostPresentationFrame(px, w, h, nullptr), bool())
        {
            uint32_t fbp = 0;
            return gs.copyLatchedHostPresentationFrame(px, w, h, &fbp);
        }
        template <typename G>
        bool copyGsFrame(G &, std::vector<uint8_t> &, uint32_t &, uint32_t &, long) { return false; }

        void putBytes(std::vector<uint8_t> &out, const void *p, size_t n)
        {
            const uint8_t *b = static_cast<const uint8_t *>(p);
            out.insert(out.end(), b, b + n);
        }
        std::unordered_map<uint32_t, pop_native::Image> g_liveImages; // live window: decoded textures by key
        std::unordered_set<uint32_t> g_liveSent;                       // texture keys the viewer already has
        std::deque<std::pair<std::vector<uint8_t>, int>> g_liveDelayed; // frames waiting for GS pictures (count so far)
        uint64_t g_liveConnection = 0;

        void writeVertices(std::vector<uint8_t> &f, const std::vector<pop_native::Vertex> &vertices, const DrawMaterial &m)
        {
            if (m.constColour < 0 && m.constAlpha < 0 && !m.hasUvm)
            {
                putBytes(f, vertices.data(), sizeof(pop_native::Vertex) * vertices.size());
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
                if (m.hasUvm)
                {
                    const float u = x.uv[0], v = x.uv[1];
                    x.uv[0] = u * m.uvm[0] + v * m.uvm[2] + m.uvm[4];
                    x.uv[1] = u * m.uvm[1] + v * m.uvm[3] + m.uvm[5];
                }
            }
            putBytes(f, v.data(), sizeof(pop_native::Vertex) * v.size());
        }

        // File: "PNF1", clip[16], draw count; per draw: model[16], batch count; per batch: vertex
        // count, index count, vertices (pop_native::Vertex), indices (u32).
        std::function<uint32_t(uint32_t)> g_rdramForDump;
        // live: the frame goes to the live native window instead of a file, and the one-off
        // diagnostics (side files, stderr reports) are skipped
        void writeFrame(bool live = false)
        {
            std::vector<uint8_t> f;
            f.reserve(8u << 20);
            // "PNF5": clip[16]; images (count; per image: w, h, RGBA); draws in the game's order (count;
            // per draw: model[16], image index (-1: none), render state flags, AREF word, fog a, fog b,
            // FOGCOL, Z test, FBMSK, batch count, batches); then "GLOW" + ColorDiffusion's parameters
            // (calls, levels, FIX, mask, convolution, strength) when it ran in the frame
            // live: "PNF6" = PNF5 with each image as (texture key, w, h, RGBA), RGBA only the first time
            // the viewer gets that key on this connection (w = h = 0 afterwards: it keeps the texture)
            putBytes(f, live ? "PNF6" : "PNF5", 4);
            putBytes(f, g_frameClip, size_t(4) * size_t(16));
            std::unordered_map<uint32_t, int32_t> imageOf;
            std::vector<pop_native::Image> images;
            std::vector<uint32_t> imageKeys;
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
                    bool found = false;
                    if (live)
                    {
                        // decoded once per texture key (the packs stay loaded); misses are retried
                        auto it = g_liveImages.find(m.textureKey);
                        if (it == g_liveImages.end() && findTexture(m.textureKey, img))
                            it = g_liveImages.emplace(m.textureKey, std::move(img)).first;
                        if ((found = it != g_liveImages.end()))
                            img = it->second;
                    }
                    else
                        found = findTexture(m.textureKey, img);
                    imageOf[m.textureKey] = found ? int32_t(images.size()) : -1;
                    if (found)
                    {
                        images.push_back(std::move(img));
                        imageKeys.push_back(m.textureKey);
                    }
                }
            if (!live)
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
            if (!live)
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
            putBytes(f, &ni0, size_t(4) * size_t(1));
            if (live && g_liveConnection != pop_live_client::connection())
            {
                g_liveConnection = pop_live_client::connection(); // a new viewer has none of our textures
                g_liveSent.clear();
            }
            for (size_t i = 0; i < images.size(); ++i)
            {
                const auto &img = images[i];
                if (live)
                {
                    putBytes(f, &imageKeys[i], 4);
                    if (!g_liveSent.insert(imageKeys[i]).second)
                    {
                        const uint32_t zero[2] = {0, 0};
                        putBytes(f, zero, 8);
                        continue;
                    }
                }
                putBytes(f, &img.width, size_t(4) * size_t(1));
                putBytes(f, &img.height, size_t(4) * size_t(1));
                putBytes(f, img.rgba.data(), img.rgba.size());
            }
            const uint32_t n = uint32_t(g_frameDraws.size() + g_framePosed.size() + g_frameRefs.size());
            putBytes(f, &n, size_t(4) * size_t(1));
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
            // side file: per written draw its kind, source index, render state, texture key and mean
            // vertex colour (0x80 = 1.0), to identify draws found by bisecting a frame in the viewer
            FILE *dl = live ? nullptr : std::fopen((g_framePath + ".draws.txt").c_str(), "w");
            uint32_t drawNo = 0;
            for (const auto &o : order)
            {
                if (dl)
                {
                    const DrawMaterial &m = o[1] == 0 ? g_frameMaterials[o[2]] : o[1] == 1 ? g_framePosed[o[2]].second : g_frameRefs[o[2]].second.second;
                    const auto &bs = o[1] == 0 ? g_elements[g_frameDraws[o[2]].first].meshes : o[1] == 1 ? g_framePosed[o[2]].first : g_frameRefs[o[2]].first;
                    double c[4] = {0, 0, 0, 0};
                    size_t nv = 0;
                    for (const auto &b : bs)
                        for (const auto &v : b.vertices)
                        {
                            for (int k = 0; k < 4; ++k)
                                c[k] += v.colour[k];
                            ++nv;
                        }
                    std::fprintf(dl, "%u %s %u element %08x flags %08x ref %08x tex %08x batches %zu verts %zu colour %.3f %.3f %.3f %.3f\n", drawNo,
                                 o[1] == 0 ? "static" : o[1] == 1 ? "skinned" : "instanced", o[2],
                                 o[1] == 0 ? g_frameDraws[o[2]].first : 0u, m.flags, m.ref, m.textureKey, bs.size(), nv, nv ? c[0] / nv : 0,
                                 nv ? c[1] / nv : 0, nv ? c[2] / nv : 0, nv ? c[3] / nv : 0);
                }
                ++drawNo;
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
                putBytes(f, model, size_t(4) * size_t(16));
                putBytes(f, &image, size_t(4) * size_t(1));
                putBytes(f, &mat.flags, size_t(4) * size_t(1));
                putBytes(f, &mat.ref, size_t(4) * size_t(1));
                putBytes(f, &mat.fogA, size_t(4) * size_t(1));
                putBytes(f, &mat.fogB, size_t(4) * size_t(1));
                putBytes(f, &mat.fogColour, size_t(4) * size_t(1));
                putBytes(f, &mat.zTest, size_t(4) * size_t(1));
                putBytes(f, &mat.fbmsk, size_t(4) * size_t(1));
                putBytes(f, &nb, size_t(4) * size_t(1));
                for (const auto &b : *batches)
                {
                    const uint32_t nv = uint32_t(b.vertices.size()), ni = uint32_t(b.indices.size());
                    putBytes(f, &nv, size_t(4) * size_t(1));
                    putBytes(f, &ni, size_t(4) * size_t(1));
                    writeVertices(f, b.vertices, mat);
                    putBytes(f, b.indices.data(), size_t(4) * size_t(ni));
                    tris += ni / 3;
                }
            }
            if (dl)
                std::fclose(dl);
            putBytes(f, "GLOW", size_t(1) * size_t(4));
            putBytes(f, &g_glowCalls, size_t(4) * size_t(1));
            putBytes(f, g_glow, size_t(4) * size_t(5));
            putBytes(f, "HAZE", size_t(1) * size_t(4));
            putBytes(f, &g_hazeCalls, size_t(4) * size_t(1));
            putBytes(f, g_haze, size_t(4) * size_t(2));
            // POP_NATIVE_WINDOW_GS=<d>: the emulated GS picture for the viewer's comparison toggle
            // ("GSFB" + w + h + RGBA). The picture the runtime presents lags the frame being drawn, so
            // a frame waits for the pictures latched at the next d frame boundaries and carries all
            // of them (the first is the one shown; the viewer scores each, to find the right lag).
            static const char *gsEnv = std::getenv("POP_NATIVE_WINDOW_GS");
            static const int gsDelay = gsEnv && *gsEnv ? std::max(1, std::atoi(gsEnv)) : 0;
            if (live && gsDelay > 0 && g_runtimePtr)
            {
                std::vector<uint8_t> px;
                uint32_t w = 0, h = 0;
                const bool have = copyGsFrame(g_runtimePtr->gs(), px, w, h, 0) && w && h && px.size() >= size_t(w) * h * 4;
                // every frame still waiting gets this boundary's picture
                for (auto &pending : g_liveDelayed)
                    if (have)
                    {
                        putBytes(pending.first, "GSFB", 4);
                        putBytes(pending.first, &w, 4);
                        putBytes(pending.first, &h, 4);
                        putBytes(pending.first, px.data(), size_t(w) * h * 4);
                        ++pending.second;
                    }
                g_liveDelayed.emplace_back(std::move(f), 0);
                while (!g_liveDelayed.empty() && int(g_liveDelayed.front().second) >= gsDelay)
                {
                    pop_live_client::waitIdle(); // comparison mode: every frame is sent, the game waits
                    pop_live_client::submit(std::move(g_liveDelayed.front().first));
                    g_liveDelayed.pop_front();
                }
                return;
            }
            if (live)
            {
                pop_live_client::submit(std::move(f));
                return;
            }
            if (FILE *out = std::fopen(g_framePath.c_str(), "wb"))
            {
                std::fwrite(f.data(), 1, f.size(), out);
                std::fclose(out);
            }
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
                int alphaWriters = 0;
                std::map<uint32_t, int> lists;
                for (const DrawMaterial &m : allMaterials)
                {
                    alphaWriters += (m.fbmsk & 0xFF000000u) == 0;
                    ++lists[m.list];
                }
                std::fprintf(stderr, "[pop:native] draws writing alpha (glow mask): %d; draws per list:", alphaWriters);
                for (const auto &kv : lists)
                    std::fprintf(stderr, " +0x%x x%d", kv.first, kv.second);
                std::fprintf(stderr, "\n");
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
                    std::fprintf(stderr, "[pop:native]   gfx %zu: type %02x element %08x tex %08x flags %08x list +0x%x\n", i, g_frameGfx[i].type,
                                 g_frameGfx[i].element, g_frameGfx[i].textureKey, g_frameGfx[i].flags, g_frameGfx[i].list);
            }
            for (const OtherDraw &o : g_frameOther)
            {
                pop_native::Image img;
                const bool ok = o.textureKey && findTexture(o.textureKey, img);
                std::fprintf(stderr, "[pop:native]   %s: object %08x tex %08x (%ux%u, image %d) flags %08x list +0x%x\n", g_other[o.path].name,
                             o.object, o.textureKey, ok ? img.width : 0u, ok ? img.height : 0u,
                             imageOf.count(o.textureKey) ? imageOf[o.textureKey] : -1, o.flags, o.list);
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

        void listRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_currentList = getRegU32(ctx, 4) - g_drawManager;
            g_origListRender(rdram, ctx, runtime);
        }

        // live window: empties everything one frame's capture collected, for the next frame
        void resetFrameCapture()
        {
            g_frameDraws.clear();
            g_frameSub.clear();
            g_frameMaterials.clear();
            g_framePosed.clear();
            g_frameRefs.clear();
            g_frameOther.clear();
            g_frameGfx.clear();
            g_frameSeen.clear();
            g_frameLightmap.clear();
            g_frameSeq = 0;
            g_glowCalls = 0;
            g_hazeCalls = 0;
            g_haloAddsInWindow = 0;
        }

        void display3D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdramPtr = rdram;
            g_runtimePtr = runtime;
            g_drawManager = getRegU32(ctx, 4);
            if (g_frameCapturing)
            {
                g_rdramForDump = [rdram](uint32_t a) { return guestAny(rdram, a); };
                g_frameCapturing = false;
                if (g_liveWindow)
                {
                    static const bool gsMode = std::getenv("POP_NATIVE_WINDOW_GS") != nullptr;
                    if (gsMode || !pop_live_client::busy())
                        writeFrame(true);
                    resetFrameCapture();
                }
                else
                {
                    g_frameDone = true;
                    writeFrame();
                }
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

    namespace
    {
        // POP_NATIVE_SKIP=<groups> (measurement only, the picture is wrong): the draw paths the native
        // renderer already covers return without building their DMA, so the frame rate shows what
        // drawing them natively would save on VIF1/VU1/GS work. Groups (comma-separated, or "all"):
        // static (CJMeshElement::Display of elements without a bone palette, CJMeshElementRef::Display,
        // CJIdxMesh::Display), skinned (CJMeshElement::Display with a bone palette at +0x48),
        // lightmap (CLMElementList::Render), gfx (FUN_00458FC0). Without POP_NATIVE_LIVE only.
        enum SkipGroup : unsigned
        {
            kSkipStatic = 1,
            kSkipSkinned = 2,
            kSkipLightmap = 4,
            kSkipGfx = 8
        };
        unsigned g_skip = 0;
        PS2Runtime::RecompiledFunction g_skipOrig[5] = {};
        uint64_t g_skipCount[5] = {}, g_kept[5] = {};
        constexpr uint32_t kSkipAddress[5] = {kMeshDisplay, kRefDisplay, 0x004BDB80u, 0x004B7B80u, kGfxDisplay};

        // Both mesh Display paths also switch the VU1 driver (state byte 0x700000a8: 0 = one bone, 1 = more;
        // element without a palette: no switch) before a skinned draw, through CVuManager::DoSetShadowDriver
        // or FUN_004B7480 (MPG uploads). Skipping a switch left the wrong program for the next draws (both
        // skip groups dropped to ~7 fps, 19 Oct), so a chain is skipped only if none of it would switch.
        // The palette is the element's +0x48; a CJMeshElementRef's element is at +0x20; chains: +0x4C / +0x1C.
        bool chainSwitchesDriver(uint8_t *rdram, uint32_t node, bool ref)
        {
            uint8_t driver = guest8(rdram, 0x700000A8u);
            for (unsigned link = 0; node && link < 4096; ++link)
            {
                const uint32_t palette = guest32(rdram, (ref ? guest32(rdram, node + 0x20u) : node) + 0x48u);
                if (palette)
                {
                    const uint8_t want = int32_t(guest32(rdram, palette)) < 2 ? 0 : 1;
                    if (want != driver)
                        return true;
                    driver = want;
                }
                node = guest32(rdram, node + (ref ? 0x1Cu : 0x4Cu));
            }
            return false;
        }

        bool skipCall(int n, uint8_t *rdram, R5900Context *ctx)
        {
            switch (n)
            {
            case 0: // CJMeshElement: the kind judged by the chain's head
            case 1: // CJMeshElementRef
            {
                const uint32_t node = getRegU32(ctx, 4);
                const uint32_t element = n == 1 ? guest32(rdram, node + 0x20u) : node;
                const unsigned group = guest32(rdram, element + 0x48u) ? kSkipSkinned : kSkipStatic;
                return (g_skip & group) != 0 && !chainSwitchesDriver(rdram, node, n == 1);
            }
            case 2:
                return (g_skip & kSkipStatic) != 0;
            case 3:
                return (g_skip & kSkipLightmap) != 0;
            default:
                return (g_skip & kSkipGfx) != 0;
            }
        }

        template <int N>
        void skippable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (skipCall(N, rdram, ctx))
            {
                ++g_skipCount[N]; // like a stub: leave pc, the dispatcher returns to the caller
                return;
            }
            ++g_kept[N];
            g_skipOrig[N](rdram, ctx, runtime);
        }

        void installSkip(PS2Runtime &runtime, const std::string &spec)
        {
            for (const auto &[name, bit] : {std::pair<const char *, unsigned>{"static", kSkipStatic}, {"skinned", kSkipSkinned},
                                            {"lightmap", kSkipLightmap}, {"gfx", kSkipGfx}, {"all", 15u}})
                if (spec.find(name) != std::string::npos)
                    g_skip |= bit;
            const PS2Runtime::RecompiledFunction hooks[5] = {skippable<0>, skippable<1>, skippable<2>, skippable<3>, skippable<4>};
            for (int n = 0; n < 5; ++n)
                if ((g_skipOrig[n] = runtime.lookupFunction(kSkipAddress[n])))
                    runtime.replaceFunction(kSkipAddress[n], hooks[n]);
            std::fprintf(stderr, "[pop:native] POP_NATIVE_SKIP=%s: groups %x (measurement only)\n", spec.c_str(), g_skip);
            std::thread([] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                    std::fprintf(stderr, "[pop:native] skipped/kept: mesh %llu/%llu ref %llu/%llu idx %llu/%llu lm %llu/%llu gfx %llu/%llu\n",
                                 (unsigned long long)g_skipCount[0], (unsigned long long)g_kept[0], (unsigned long long)g_skipCount[1],
                                 (unsigned long long)g_kept[1], (unsigned long long)g_skipCount[2], (unsigned long long)g_kept[2],
                                 (unsigned long long)g_skipCount[3], (unsigned long long)g_kept[3], (unsigned long long)g_skipCount[4],
                                 (unsigned long long)g_kept[4]);
                }
            }).detach();
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("POP_NATIVE_LIVE");
        if (const char *skip = std::getenv("POP_NATIVE_SKIP"); skip && *skip)
        {
            if (v && std::strcmp(v, "1") == 0)
                std::fprintf(stderr, "[pop:native] POP_NATIVE_SKIP ignored with POP_NATIVE_LIVE=1\n");
            else
                installSkip(runtime, skip);
            return;
        }
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
        const char *frameSpec = std::getenv("POP_NATIVE_FRAME"), *windowSpec = std::getenv("POP_NATIVE_WINDOW");
        if ((frameSpec && *frameSpec) || (windowSpec && *windowSpec))
        {
            if (windowSpec && *windowSpec)
            {
                // the live native window: every frame, streamed to pop_native_view --live <port>
                g_liveWindow = std::atoi(windowSpec);
                g_frameAfter = 0.0;
                pop_live_client::start(g_liveWindow);
            }
            else
            {
                const std::string spec = frameSpec;
                const auto colon = spec.rfind(':');
                g_framePath = spec.substr(0, colon);
                g_frameAfter = colon != std::string::npos ? std::atof(spec.c_str() + colon + 1) : 60.0;
            }
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
            g_origListRender = runtime.lookupFunction(0x004B9440u);
            if (g_origListRender)
                runtime.replaceFunction(0x004B9440u, listRender);
            g_origGlow = runtime.lookupFunction(0x004AE100u);
            if (g_origGlow)
                runtime.replaceFunction(0x004AE100u, colorDiffusion);
            g_origHaze = runtime.lookupFunction(0x004A93C0u);
            if (g_origHaze)
                runtime.replaceFunction(0x004A93C0u, smartBlur);
            g_origAddHalo = runtime.lookupFunction(0x004AB1C0u);
            if (g_origAddHalo)
                runtime.replaceFunction(0x004AB1C0u, addHalo);
            g_origParticles = runtime.lookupFunction(0x004584C0u);
            if (g_origParticles)
                runtime.replaceFunction(0x004584C0u, particles);
            g_origGfx = runtime.lookupFunction(kGfxDisplay);
            if (g_origGfx)
                runtime.replaceFunction(kGfxDisplay, gfxDisplay);
        }
        g_lastReport = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[pop:native] live conversion check on\n");
    }
}
