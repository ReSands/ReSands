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
#if __has_include("runtime/ps2_save_state.h") // the SotC runtime's save states (POP_NATIVE_FLASH_SAVE)
#include "runtime/ps2_save_state.h"
#define POP_LIVE_QUICKSAVE 1
#else
#define POP_LIVE_QUICKSAVE 0
#endif

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
#include <tuple>
#include <thread>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Sprites (CJadeSpriteElement, list kind 8) are placed by VU1 program P9: its C++ port (tools/vu1port/ported/p9.cpp,
// judged against the interpreter on whole runs), with the runtime's VU1 interpreter as the fallback at a pc no
// stage of the port starts at (pop-sotc: the interpreter's source sits next to the runner directory this file is
// copied to; offline, pop_state_frame_vu1 defines POP_SPRITE_VU1). Both hand XGKICK packets to the thread's packet
// sink (vu1_impl::packetSink, an inline variable of ps2_vu1_impl.inl, declared here identically); with forward =
// false they never reach the GS.
#if defined(POP_SPRITE_VU1) || __has_include("../lib/vu/ps2_vu1_impl.inl")
#undef POP_SPRITE_VU1
#define POP_SPRITE_VU1 1
#include "runtime/ps2_vu1.h"
#include <xmmintrin.h>
namespace vu1_impl
{
    struct Vu1PacketSink
    {
        std::vector<std::vector<uint8_t>> packets;
        bool forward = true;
    };

    inline thread_local Vu1PacketSink *packetSink = nullptr;
}
#define VU_TASK_R 1 // P9 uses R (VuState carries it)
#define VU_PORT_NO_STAGE // two ports in one file: no harness entry
#include "../../../../../../tools/vu1port/vu_gif.h"
#include "../../../../../../tools/vu1port/ported/p9.cpp"
#include "../../../../../../tools/vu1port/ported/p5.cpp" // GFX particles
#include "../../../../../../tools/vu1port/ported/p6.cpp" // water
// the port's XGKICK: the packet as the interpreter's transfer would send it (the judge compares the two)
void xgkick(VuState &vu, uint32_t qwordAddress)
{
    if (vu1_impl::Vu1PacketSink *sink = vu1_impl::packetSink)
        sink->packets.push_back(vuGifPacket(vu.mem, qwordAddress));
}
namespace pop_native_live
{
    // offline only (pop_state_frame_vu1 POP_P9_TRACE, tools/native/src/p9trace.cpp): runs a sprite VU run itself and
    // records it in the runtime's PS2X_VU1_TRACE format, the cases of the P9 port (tools/vu1port); null in the game
    void (*g_spriteRunHook)(VU1Interpreter &vu, uint8_t *code, uint8_t *data, GS &gs, int32_t pc, uint32_t tops) = nullptr;
    // offline only (pop_state_frame_vu1): a GFX draw's particle packets (SetParticleEnv + CGraphicParticle DMA records,
    // [start, end)) in the save's last chain, as FUN_004584C0 wrote them; 0 = none. Null in the game (captured there).
    uint32_t (*g_particlePackets)(uint32_t element, uint32_t &end) = nullptr;
}
#else
#define POP_SPRITE_VU1 0
#endif

uint32_t popTextureKey(uint32_t index); // pop_slus20743.cpp: texture index -> bigfile key
namespace pop_native_world // pop_native_world.cpp: draws the native world frames into the GS memory
{
    bool active(const GSRegisters *display);                  // the runtime can inject (GS host hook + renderer); display: DISPFB
    void submitFrame(uint32_t seq, uint32_t generation, std::vector<uint8_t> &&pnf); // frame `seq`'s native draws (PNF7)
    uint32_t sentGeneration(); // the kept-mesh generation a frame is written in (sampled before writing it)
    bool meshSent(uint64_t key, uint32_t generation); // true if the renderer keeps this batch already; false: send it
    void noteFrame(uint32_t seq, std::string &&text); // glitch capture: the frame's record (camera, fog, purity)
}
namespace pop_hdtex
{
    bool readEntry(const std::string &path, uint32_t key, std::vector<uint8_t> &out); // pop_hdtextures.cpp
}

namespace pop_native_live
{
    void onBigfileLookup(uint32_t key); // below: load a world pack once

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
            uint32_t serial = 0;                    // which conversion (an element address can be reused): world batch keys
        };
        uint32_t g_elementSerial = 0;

        uint64_t mix64(uint64_t h, uint64_t v)
        {
            h = (h ^ v) * 0x9E3779B97F4A7C15ull;
            return h ^ (h >> 29);
        }
        // the part of a material writeVertices applies to the vertices (a batch's bytes depend on it)
        uint64_t vertexVariant(const struct DrawMaterial &m);
        // world batch key of a batch whose contents are not cached per element (instanced, CJIdxMesh)
        uint64_t contentKey(const pop_assets::Batch &b)
        {
            uint64_t h = 0x243F6A8885A308D3ull ^ b.vertices.size() ^ (uint64_t(b.indices.size()) << 32);
            const auto words = [&h](const void *p, size_t bytes) {
                const uint8_t *c = static_cast<const uint8_t *>(p);
                size_t i = 0;
                for (; i + 8 <= bytes; i += 8)
                {
                    uint64_t w;
                    std::memcpy(&w, c + i, 8);
                    h = mix64(h, w);
                }
                for (; i < bytes; ++i)
                    h = mix64(h, c[i]);
            };
            words(b.vertices.data(), b.vertices.size() * sizeof(b.vertices[0]));
            words(b.indices.data(), b.indices.size() * 4);
            return h;
        }
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
            // 0x70000270 + id * 16 to VU 0x10 = (max, min, a, b); F = a + b * w clamped to min..max by
            // the VU1 drivers (MINIx / MAXy with vf31): min 102 keeps distant buildings 40% visible)
            // and FOGCOL as the render state sends it (0x70000024: the zone colour, 0 for
            // additive/darkening blends)
            float fogA = 255.0f, fogB = 0.0f, fogMin = 0.0f, fogMax = 255.0f;
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
            bool native = false; // POP_NATIVE_WORLD: drawn natively (its Display call skipped)
            // geometry made anew each frame (water, halos, sprites, particles): sent with key 0, never kept by the renderer
            // (as a content key it filled the 512 MB kept-mesh budget and forced whole-world resends; review 30 Oct)
            bool frameOnly = false;
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

        // POP_NATIVE_WORLD=1 (renderer step 1, docs/native_renderer.md "Native world in the game
        // window"): opaque static draws of the first lists are drawn natively and their Display calls
        // skipped (no VIF1/VU1/GS work); the native picture and its GS Z go into the GS frame and Z
        // buffers where the frame's 3D starts, so the GS draws everything else over it, depth-tested.
        // Display3DElements puts a marker in the frame's DMA list (graphics object +0xEC, the list
        // being built): VIF FLUSHA + DIRECT of a GIF A+D write to GS register 0x7F (none; the GS
        // ignores it) with "PNWD" and the frame number. At the frame's DMA kick (from PS2_Flip) the
        // frame's native draws go to pop_native_world, whose GS hook swaps the marker for the images.
        bool g_world = false;     // the mode is on
        // renderer step 2: opaque skinned draws (characters) too, posed on the CPU (POP_NATIVE_CHARACTERS=1; off: on the GS)
        const bool g_worldCharacters = [] {
            const char *e = std::getenv("POP_NATIVE_CHARACTERS");
            return e && *e == '1';
        }();
        bool g_worldMarked = false; // this frame has a marker (Display3DElements ran since the kick)
        // Load-flash capture (POP_NATIVE_WORLD_DUMP): Display3DElements calls this frame, and the world -> clip
        // matrix at each later call and at the kick when it differs from the one the frame was captured with
        uint32_t g_worldViews = 0;
        bool g_worldOpen = false; // draws may go native: the frame's first Display3DElements call
        uint32_t g_worldSeq = 0;
        std::unordered_set<uint64_t> g_worldBase; // (element << 16 | matrix index) drawn natively this frame
        std::unordered_set<uint32_t> g_worldLmRefs; // instanced elements with a lightmap entry (not native yet)
        struct WorldStats
        {
            uint64_t frames = 0, skipped = 0, kept = 0, lightmaps = 0, noTexture = 0, notRecorded = 0, bytes = 0;
            uint64_t batchesSent = 0, batchesKnown = 0; // PNF7: geometry sent vs referenced (the renderer keeps it)
            uint64_t posed = 0, drivers = 0; // skinned draws drawn natively; VU1 driver switches replayed for skipped calls
            uint64_t skinGs[3] = {};         // skinned Display calls left to the GS: render state/list, not decoded, texture
            uint64_t litStatic = 0;          // static draws of the lit driver (pass 1/4) left to the GS (see isLitStaticPass)
            uint64_t litNative = 0, envNative = 0; // lit static passes drawn natively: pass 1 lit, pass 2 env map (litDriverPass)
            uint64_t particles = 0;                // GFX particle draws (FUN_004584C0) drawn natively
        } g_worldStats;
        constexpr uint32_t kWorldMagic = 0x504E5744u; // "PNWD"
        bool worldTextureReady(uint32_t key); // decoded (or decodable) for the frame writer

        // The render state of a Display call allows a native draw: lists +200, +0x2C, +0x3C (before
        // Display3DElements' first Z-test-less and alpha passes), opaque (blend kind 0; alpha test
        // allowed), writes depth and RGB, Z test on, not the alpha-only pass.
        bool worldStateOk(uint8_t *rdram);
        // the records a Display call added (kind 0: g_frameMaterials index, 1: g_framePosed index, 2: g_frameRefs index)
        struct WorldRecord
        {
            int kind;
            size_t index;
        };
        size_t worldCount0();
        size_t worldCount1();
        size_t worldCount2();
        DrawMaterial &worldMaterial(const WorldRecord &r);
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

        std::vector<std::shared_ptr<LoadedPack>> loadedPacks()
        {
            std::vector<std::shared_ptr<LoadedPack>> packs;
            std::lock_guard<std::mutex> lock(g_packsMutex);
            for (const auto &p : g_packs)
                if (p.second)
                    packs.push_back(p.second);
            return packs;
        }
        bool findTexture(uint32_t key, pop_native::Image &out)
        {
            for (const auto &p : loadedPacks())
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

        // Palettes changed in memory (N1, 25 Oct): effects recolour a texture by rewriting its palette and
        // re-uploading it (GAO_ModifierSfx_UpdateMaterialPalette 0x421100: the dagger's slow blends an enemy's
        // palette towards sand), while the packs hold the disc palette. A 256-colour texture whose palette in
        // memory differs is decoded with that one for the frame, under the key kPaletteKey + its texture index
        // (inside the shadow key range: sent with its pixels every frame, the renderer updates its image).
        // TEX_List_GetTexture 0x44FA00 / GetPalette 0x44F940: entries of 16 bytes (+10 index, +0xF bit 5 = in use);
        // texture +4 palette index, +8 flags (0x40: paletted); palette +4 colours (+0x60 when its +8 has 0x40),
        // 256-colour when (+8 & 0x41) is 0 or 0x40.
        constexpr uint32_t kPaletteKey = 0xFE5D8000u;
        // the water's colour pass (waterRecord): written as image -2, the frame copy read through the refraction buffer
        constexpr uint32_t kRefractionKey = 0xFE5C0000u;
        // a halo's visibility (haloRecord): written as image -3, the frame copy's alpha averaged over the core square
        constexpr uint32_t kHaloAlphaKey = 0xFE5C0001u;
        // the glow mask put back after a halo: image -4, the same read of the frame copy without a new copy
        constexpr uint32_t kHaloRestoreKey = 0xFE5C0002u;
        std::unordered_map<uint32_t, pop_native::Image> g_framePaletteImages; // texture index -> recoloured this frame
        std::unordered_map<uint32_t, uint32_t> g_framePaletteKeys;            // texture index -> key used this frame
        uint32_t texListEntry(uint8_t *rdram, uint32_t countAddr, uint32_t listAddr, uint32_t index)
        {
            const uint32_t count = guestAny(rdram, countAddr), list = guestAny(rdram, listAddr);
            if (!list || index >= count)
                return 0;
            const auto ok = [&](uint32_t e) { return guest16(rdram, e + 10) == index && (guest8(rdram, e + 0xF) & 0x20); };
            if (ok(list + 16 * index))
                return list + 16 * index;
            for (uint32_t i = 0; i < count && i < 8192; ++i)
                if (ok(list + 16 * i))
                    return list + 16 * i;
            return 0;
        }
        uint32_t recolouredKey(uint8_t *rdram, uint32_t index, uint32_t key)
        {
            const auto done = g_framePaletteKeys.find(index);
            if (done != g_framePaletteKeys.end())
                return done->second;
            uint32_t result = key;
            const uint32_t tex = texListEntry(rdram, 0x006CBC50u, 0x006CBC58u, index);
            const uint32_t pal = tex && (guest16(rdram, tex + 8) & 0x40) ? texListEntry(rdram, 0x006CBC68u, 0x006CBC70u, guest16(rdram, tex + 4)) : 0;
            const uint32_t pflags = pal ? guest8(rdram, pal + 8) & 0x41 : 1;
            if (pal && (pflags == 0 || pflags == 0x40) && guestAny(rdram, pal + 4))
            {
                // the pack's palette, kept once found (after a save-state load the packs load on texture misses)
                static std::unordered_map<uint32_t, std::vector<uint8_t>> disc;
                auto it = disc.find(key);
                if (it == disc.end())
                {
                    std::vector<uint8_t> bytes;
                    for (const auto &p : loadedPacks())
                        if (p->index.records.count(key) && pop_assets::texturePalette(p->index, key, bytes))
                            break;
                    if (bytes.empty())
                    {
                        static int missed = 0;
                        if (missed++ < 20)
                            std::fprintf(stderr, "[pop:native] paletted texture %08x (index %u): no pack palette (yet)\n", key, index);
                        return g_framePaletteKeys[index] = key;
                    }
                    it = disc.emplace(key, std::move(bytes)).first;
                }
                const uint8_t *ram = getConstMemPtr(rdram, guestAny(rdram, pal + 4) + (pflags == 0x40 ? 0x60 : 0));
                if (index < 0x8000 && std::memcmp(ram, it->second.data(), 1024) != 0)
                {
                    pop_native::Image img;
                    for (const auto &p : loadedPacks())
                        if (p->index.records.count(key) && pop_assets::decodeTexture(p->index, key, img, ram))
                        {
                            result = kPaletteKey + index;
                            static uint64_t made = 0;
                            if (++made <= 20 || made % 600 == 0)
                                std::fprintf(stderr, "[pop:native] recoloured texture %08x (index %u, %ux%u): palette changed in memory (%llu so far)\n",
                                             key, index, img.width, img.height, (unsigned long long)made);
                            g_framePaletteImages[index] = std::move(img);
                            break;
                        }
                }
            }
            return g_framePaletteKeys[index] = result;
        }

        // POP_NATIVE_LATE=1 (24 Oct, being judged; needs POP_NATIVE_TRANSLUCENT=1): the lists after the shadows too,
        // +0x88 (with nearly all GFX particles) and +0xA8, but only while the frame is "pure": the picture goes in
        // where the frame's 3D starts, so every draw left to the GS lands on top of every native one, and a native
        // late draw keeps the game's order only if no draw before it went to the GS (worldOnGs ends it). Display3DElements:
        // ... +0x5C, shadows, +0x88, EraseAlphaBuffer, +0x98 (render state 0x44 = alpha only), +0xA8, halos. +0x98
        // and EraseAlphaBuffer stay on the GS and write only alpha (the glow mask), so they leave the order of the
        // colours alone; a +0xA8 draw goes native only if it writes neither Z (+0x98 tests against it) nor alpha
        // (EraseAlphaBuffer would clear it). Census of 203 saves (entries): +0x88 17,750 (CJIdxMesh 8,898, mesh 3,676,
        // GFX 3,049, sprites 1,128, ref 923, water 76), +0x98 2,526 (mesh/ref), +0xA8 1,189 (all CJIdxMesh); only 37% of
        // the +0x88 entries come before its first sprite or water draw (both on the GS until 0.13).
        const bool g_worldLate = [] {
            const char *e = std::getenv("POP_NATIVE_LATE");
            return e && *e == '1';
        }();
        bool g_worldPure = false; // no draw of this frame has gone to the GS yet (from the marker on)
        bool lateList(uint32_t list) { return list == 0x88u || list == 0x98u || list == 0xA8u; }
        struct LateStats
        {
            uint64_t frames = 0, pureAtLate = 0, closedInLate = 0; // frames reaching +0x88 pure; purity ended within the late lists
            uint64_t native = 0, gs = 0;                            // late-list calls (+0x88, +0xA8) drawn natively / on the GS
            std::map<std::string, uint64_t> closedBy;               // what ended purity first (path and list)
        } g_lateStats;
        std::string g_frameClosedBy; // what ended this frame's purity (glitch capture record)
        // a draw call of the open frame goes to the GS: ends the frame's purity (the late lists stay on the GS from now on)
        void worldOnGs(uint8_t *rdram, const char *path)
        {
            if (!g_world || !g_worldOpen)
                return;
            if (g_currentList == 0x88u || g_currentList == 0xA8u)
                ++g_lateStats.gs;
            if (!g_worldPure)
                return;
            if (g_currentList == 0x98u)
            {
                // alpha only: harmless unless it writes Z (a later native +0xA8 draw would not be tested against it)
                const uint32_t sub = guestAny(rdram, 0x70000034u);
                if (sub && (guestAny(rdram, sub + 4) & 0x200))
                    return;
            }
            g_worldPure = false;
            if (lateList(g_currentList))
                ++g_lateStats.closedInLate;
            char why[48];
            std::snprintf(why, sizeof why, "%s +0x%x", path, g_currentList);
            ++g_lateStats.closedBy[why];
            g_frameClosedBy = why;
        }

        // POP_NATIVE_TRANSLUCENT=1 (23 Oct): every draw of the lists Display3DElements draws before
        // the shadows (200, +0x2C, +0x3C, +0x68, +0x78, +0x4C; the lightmaps +0x5C are native already), any blend.
        // The picture is an upload that replaces pixels, so only a prefix of the game's order can be native:
        // the lists after the shadows wait for POP_NATIVE_LATE (above), and blended draws need a pure frame (29 Oct).
        // Frame alpha needs nothing here: only +0x98 writes it (EraseAlphaBuffer runs before it).
        bool worldStateOk(uint8_t *rdram)
        {
            static const bool translucent = [] {
                const char *e = std::getenv("POP_NATIVE_TRANSLUCENT");
                return e && *e == '1';
            }();
            if (!g_worldOpen)
                return false;
            const bool opaqueList = g_currentList == 200u || g_currentList == 0x2Cu || g_currentList == 0x3Cu;
            const bool prefixList = opaqueList || g_currentList == 0x68u || g_currentList == 0x78u || g_currentList == 0x4Cu;
            const bool late = translucent && g_worldLate && g_worldPure && (g_currentList == 0x88u || g_currentList == 0xA8u);
            if (!(translucent ? prefixList || late : opaqueList))
                return false;
            const uint32_t sub = guestAny(rdram, 0x70000034u);
            if (!sub)
                return false;
            if (late && g_currentList == 0xA8u)
            {
                // neither Z (flag 0x200 = no depth write) nor alpha (FBMSK as currentMaterial computes it)
                const uint32_t flags = guestAny(rdram, sub + 4);
                const bool alpha = (flags & 0x60) == 0x40 || (flags & 0x28);
                return (flags & 0x200) && !alpha;
            }
            const uint32_t flags = guestAny(rdram, sub + 4);
            const bool opaque = ((flags >> 16) & 0xF) == 0 && !(flags & 0x200) && !(flags & 0x40) && guest8(rdram, 0x70000038u) &&
                                guestAny(rdram, 0x70000044u) == 0xFFFFFFFFu;
            // a blended or Z-less draw keeps the game's order only while nothing before it went to the GS (N13/N14/N16:
            // instanced walls with lightmaps go to the GS in +0x2C and covered the native +0x4C corner stones over them)
            return opaque || (translucent && g_worldPure);
        }

        // The render state's current sub-material (0x70000034, set by CElementList::Render before
        // Display): word 0 = texture index (low 16 bits; flags in the top byte), word 1 = the record's
        // +0x1C = the render state flags (blend kind in bits 16-19, 0x10 alpha test, 0x200 no depth
        // write; decoded by pop_native::materialFromGsFlags as SetBlending 0x4B2500 does), +0x10 = its
        // +0x28 (AREF in the top byte); 16 Oct.
        // Texture index -> bigfile key from the engine's own texture list (TEX_List: count 0x6CBC50, entries at
        // *0x6CBC58, 16 bytes: key, ..., +10 u16 index), as pop_state_frame reads it, read at draw time. The table
        // PS2_Texture_Load fills (popTextureKey) went stale for textures loaded mid-level: enemies drawn with
        // another texture (24 Oct, saves1024b). Positions are cached and re-checked on use; popTextureKey is the
        // fallback for an index the list does not hold.
        struct TexListStats
        {
            uint64_t lookups = 0, rebuilt = 0, differ = 0, fallback = 0;
        } g_texListStats;
        uint32_t textureKey(uint8_t *rdram, uint32_t index)
        {
            static std::unordered_map<uint32_t, uint32_t> position; // index -> entry number
            static uint32_t cachedList = 0, cachedCount = 0;
            ++g_texListStats.lookups;
            const uint32_t count = guestAny(rdram, 0x006CBC50u), list = guestAny(rdram, 0x006CBC58u);
            const auto entryOk = [&](uint32_t pos) { return pos < count && guest16(rdram, list + 16 * pos + 10) == index; };
            auto it = position.find(index);
            if (list != cachedList || count != cachedCount || it == position.end() || !entryOk(it->second))
            {
                position.clear();
                cachedList = list;
                cachedCount = count;
                ++g_texListStats.rebuilt;
                if (list && count < 8192)
                    for (uint32_t i = 0; i < count; ++i)
                        position[guest16(rdram, list + 16 * i + 10)] = i;
                it = position.find(index);
            }
            const uint32_t registered = popTextureKey(index);
            if (it == position.end() || !entryOk(it->second) || !guestAny(rdram, list + 16 * it->second))
            {
                ++g_texListStats.fallback;
                return registered;
            }
            const uint32_t key = guestAny(rdram, list + 16 * it->second);
            if (registered && registered != key)
            {
                static int logged = 0;
                if (g_texListStats.differ++ < 1 || logged < 16)
                {
                    ++logged;
                    std::fprintf(stderr, "[pop:native] texture index %u: TEX_List key %08x, PS2_Texture_Load registered %08x (using the list's)\n",
                                 index, key, registered);
                }
            }
            return key;
        }

        // the current fog zone's VU 0x10 constants (FUN_004B7200: table 0x70000270 + zone * 16) as the draw's fog
        void zoneFog(uint8_t *rdram, DrawMaterial &dm)
        {
            const uint32_t e = 0x70000270u + (guestAny(rdram, 0x70000084u) & 0xFF) * 16, fmax = guestAny(rdram, e),
                           fmin = guestAny(rdram, e + 4), fa = guestAny(rdram, e + 8), fb = guestAny(rdram, e + 12);
            std::memcpy(&dm.fogA, &fa, 4);
            std::memcpy(&dm.fogB, &fb, 4);
            std::memcpy(&dm.fogMin, &fmin, 4);
            std::memcpy(&dm.fogMax, &fmax, 4);
        }

        DrawMaterial currentMaterial(uint8_t *rdram)
        {
            DrawMaterial dm;
            dm.seq = g_frameSeq++;
            const uint32_t sub = guestAny(rdram, 0x70000034u);
            if (!sub)
                return dm;
            // word 0: texture index in the low bits, flags in the top byte (0x88/0xB8/0xC0/0xF8.. seen)
            dm.textureKey = textureKey(rdram, guestAny(rdram, sub) & 0xFFFFu);
            if (dm.textureKey)
                dm.textureKey = recolouredKey(rdram, guestAny(rdram, sub) & 0xFFFFu, dm.textureKey);
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
            zoneFog(rdram, dm);
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
        // Vertex colour = ambient + sum max(0, n . d_i) * colour_i (tools/chainskin.py; game-checked)
        // + per point light colour * clamp(a + b * d^2, 0, 1) * max(0, n . unit(pos - vertex)) (checked
        // against the VU1 program's own output, tools/vu1port/lightprobe.cpp, 22 Oct).
        struct LightEnv
        {
            bool valid = false;
            int count = 0, points = 0;
            float dir[4][3] = {}, col[4][3] = {}, amb[3] = {};
            float pos[4][3] = {}, pcol[4][3] = {}, fall[4][2] = {}; // point lights: world position, colour, (a, b)
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
                // As CVuManager::DoSetSkinLightEnv (0x4B64C0, decompiled 22 Oct) uploads them. Sets with +1 != 0
                // (or no lights): `count` slots as directional lights without looking at their types, vector at
                // +0x10 + 16i (not normalised), colour bytes at +0xD0 + 4i (slots with a zero colour word
                // skipped). The title scene's set 0 holds a position and floats there; the game lights with them
                // as they are (the backlit curtains come out bright, as in the GS frame).
                // Sets with +1 == 0 (the characters' sets in play) take its full path, by slot type at +0xE0 + 4i:
                // type 1 = directional, its direction (unit) at +0x10 (uploaded to VU 0x3FC); type 0 = point
                // light, position at +0x50 (w 1) and two falloff parameters at +0x90 (e.g. 1, -0.00163), its +0x10
                // left over (junk, e.g. 1.6e29: lit with it, the Prince flashed white when facing +x, user 22 Oct).
                // The falloff, measured by replaying the VU1 program (character program 31829861 in the
                // trace of save 192521, 22 Oct): colour * clamp(a + b * d^2, 0, 1) * max(0, n . unit(pos - vertex)).
                // Other types (2: spot?) are not seen yet and stay out.
                const bool typed = guest8(rdram, set + 1) == 0;
                for (int i = 0; i < count && i < 4; ++i)
                {
                    const uint32_t c = guestAny(rdram, set + 0xD0 + 4 * i);
                    const uint32_t type = typed ? guestAny(rdram, set + 0xE0 + 4 * i) : 1u;
                    if (c != 0 && type == 0)
                    {
                        for (int k = 0; k < 3; ++k)
                        {
                            e.pos[e.points][k] = guestF(rdram, set + 0x50 + 16 * i + 4 * k);
                            e.pcol[e.points][k] = float((c >> (8 * k)) & 0xFF);
                        }
                        e.fall[e.points][0] = guestF(rdram, set + 0x90 + 16 * i);
                        e.fall[e.points][1] = guestF(rdram, set + 0x94 + 16 * i);
                        ++e.points;
                        continue;
                    }
                    if (c == 0 || type != 1)
                        continue;
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
        // objectSpace (lit statics, no palette): lit in world space, but the vertices keep their object-space positions for
        // a draw with the object's model matrix, as the base pass is drawn: the same transform gives the second layer the
        // base's exact depth (pre-transformed on the CPU it z-fought with it: mirrors flickering, user 22 Oct)
        bool poseSkinned(uint8_t *rdram, uint32_t element, uint32_t index, const std::vector<pop_assets::SkinnedBatch> &bind,
                         std::vector<pop_assets::Batch> &out, const LightEnv *light = nullptr, bool objectSpace = false)
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
                        for (int l = 0; l < light->points; ++l)
                        {
                            float to[3], d2 = 0, dot = 0;
                            for (int c = 0; c < 3; ++c)
                            {
                                to[c] = light->pos[l][c] - out1.pos[c];
                                d2 += to[c] * to[c];
                                dot += nacc[c] * to[c];
                            }
                            const float att = std::clamp(light->fall[l][0] + light->fall[l][1] * d2, 0.0f, 1.0f);
                            if (att <= 0 || dot <= 0 || d2 <= 0)
                                continue;
                            const float k = att * dot / (nl * std::sqrt(d2));
                            for (int c = 0; c < 3; ++c)
                                c3[c] += k * light->pcol[l][c];
                        }
                        for (int c = 0; c < 3; ++c)
                            out1.colour[c] = std::min(c3[c], 255.0f) / 128.0f;
                    }
                    out1.uv2[0] = out1.uv2[1] = 0;
                    if (objectSpace)
                        std::memcpy(out1.pos, v, sizeof out1.pos);
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
        // v4Positions: V4-32 positions to VU 0x3 (x, y, z, w) count too, as the load converter takes them (objects
        // with their own matrix; the V4-32 x3 header goes to VU 0). Pass-0 packets only: the same layout in pass 1
        // carries normals at 0x83 (isLitStaticPass), and pass 2's V4-8 at 0x43 is not decoded yet.
        // The VIF stream of a pass packet in guest memory: its DMA tags' VIF words and the data they send, in order
        // (cnt/next/ref/refs; refe and anything else end it after their data).
        std::vector<uint32_t> packetWords(uint8_t *rdram, uint32_t packet)
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
            return words;
        }

        void livePacketBatches(uint8_t *rdram, uint32_t packet, double uvscale, std::vector<pop_assets::Batch> &out,
                               bool v4Positions = false)
        {
            const std::vector<uint32_t> words = packetWords(rdram, packet);
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
                        // up to 2x as GS MODULATE, alpha too (P1 passes the byte on): matches GS-only frames, docs/renderer_audit.md items 2, 9
                        for (int k = 0; k < 4; ++k)
                            vx.colour[k] = 4 * v + 4 <= col.size() ? std::min(col[4 * v + k] / 128.0f, 2.0f) : 1.0f;
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
                    const bool v4Pos = v4Positions && fmt == 0xC && (w & 0x3FF) == 3;
                    if (fmt == 8 || fmt == 9 || fmt == 0xC || fmt == 0xE)
                    {
                        const unsigned slot = v4Pos ? 16u : fmt; // the V4-32 header and V4-32 positions: one each per batch
                        if (have & (1u << slot))
                            flush();
                        have |= 1u << slot;
                        if (v4Pos)
                        {
                            pos.resize(12 * size_t(num));
                            for (uint32_t v = 0; v < num; ++v)
                                std::memcpy(&pos[12 * v], raw + 16 * v, 12);
                            posNum = num;
                        }
                        else if (fmt == 9)
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

        // Lit static passes (measured 22 Oct on the VU1 trace of save 192521, tools/vu1port/inprobe.cpp; docs/native_renderer.md
        // "Lit statics measured"). CElementList::Render picks the VU1 driver of a CJMeshElement pass from the table at
        // 0x700000B0 (index pass + 5 when element +0x47 bit 1 is set); kind 1/2 runs the character program (P4) and then:
        // pass 1 gets the light env (SetSkinDriver), its packet carries normals at VU 0x83; pass 2 gets an environment map
        // (FUN_004B6D80), its packet carries normals at 0x43 and colours at 0x83; pass 4 gets SetMirrorParameterForJsm
        // (not decoded: stays on the GS). Kind 0 is the static driver (unlit, the converted world).
        uint32_t driverKind(uint8_t *rdram, uint32_t element, uint32_t pass)
        {
            const uint32_t i = pass + ((guest8(rdram, element + 0x47) >> 1) & 1) * 5;
            return i < 16 ? guest8(rdram, 0x700000B0u + i) & 0xF : 0;
        }
        bool litDriverPass(uint8_t *rdram, uint32_t element, uint32_t pass)
        {
            static const bool off = std::getenv("POP_NATIVE_NOLIT") != nullptr; // A/B: lit static passes left as before
            const uint32_t kind = driverKind(rdram, element, pass);
            return !off && (pass == 1 || pass == 2) && (kind == 1 || kind == 2);
        }

        // A lit pass packet as one rigid part's SkinnedBatches (posed by the object matrix: poseSkinned without a palette).
        // A batch = the uploads up to a VU1 kick (MSCAL/MSCNT; what a batch does not upload stays in VU memory): V4-32
        // positions at 3 (w = +-1, unused), V4-8 normals (pass 1 at 0x83, pass 2 at 0x43; fourth byte 1 = no triangle,
        // the strip flag) or V3-8 normals (most instanced refs' pass 1: the strip flag is then the UV's third value),
        // pass 1's V3-16 UVs at 0x43 (1/512), pass 2's V4-8 colours at 0x83 (0x80 = 1.0). Normals are kept as the VU
        // sees them (signed bytes / 128), the V4-32 x3 header at 0 is skipped.
        void litPacketBatches(uint8_t *rdram, uint32_t packet, bool env, std::vector<pop_assets::SkinnedBatch> &out)
        {
            const std::vector<uint32_t> words = packetWords(rdram, packet);
            static const uint8_t kElementBytes[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};
            std::vector<uint8_t> uv, nrm, col, pos;
            uint32_t posNum = 0, nrmBytes = 4; // per normal: 4 (V4-8) or 3 (V3-8)
            auto kick = [&]() {
                const uint32_t num = posNum;
                if (!num || nrm.size() < nrmBytes * size_t(num))
                    return;
                pop_assets::SkinnedBatch b;
                b.positions.resize(3 * size_t(num));
                b.uvs.assign(2 * size_t(num), 0.0f);
                b.colours.assign(4 * size_t(num), 1.0f);
                b.slots.assign(3 * size_t(num), 0);
                b.weights.assign(3 * size_t(num), 0.0f);
                b.normals.resize(3 * size_t(num));
                std::vector<int32_t> flags(num, 0);
                for (uint32_t v = 0; v < num; ++v)
                {
                    std::memcpy(&b.positions[3 * v], &pos[16 * v], 12);
                    for (int k = 0; k < 3; ++k)
                        b.normals[3 * v + k] = float(int8_t(nrm[nrmBytes * v + k])) / 128.0f;
                    if (nrmBytes == 4)
                        flags[v] = nrm[4 * v + 3];
                    else if (6 * v + 6 <= uv.size())
                    {
                        int16_t flag;
                        std::memcpy(&flag, &uv[6 * v + 4], 2);
                        flags[v] = flag;
                    }
                    b.weights[3 * v] = 1.0f;
                    if (!env && 6 * v + 6 <= uv.size())
                    {
                        int16_t st[2];
                        std::memcpy(st, &uv[6 * v], 4);
                        b.uvs[2 * v] = float(st[0] / 512.0);
                        b.uvs[2 * v + 1] = float(st[1] / 512.0);
                    }
                    if (env)
                        for (int k = 0; k < 4; ++k)
                            b.colours[4 * v + k] = 4 * v + 4 <= col.size() ? col[4 * v + k] / 128.0f : 1.0f; // GS modulate: 0xFF = ~2x
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
            };
            for (size_t i = 0; i < words.size();)
            {
                const uint32_t w = words[i++];
                const uint32_t cmd = (w >> 24) & 0x7F, num = ((w >> 16) & 0xFF) ? ((w >> 16) & 0xFF) : 256u;
                if (cmd >= 0x60)
                {
                    const uint32_t bytes = kElementBytes[cmd & 0xF] * num, n = (bytes + 3) / 4, addr = w & 0x3FF;
                    if (i + n > words.size())
                        break;
                    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&words[i]);
                    const unsigned fmt = cmd & 0xF;
                    if (fmt == 0xC && addr == 3)
                    {
                        pos.assign(raw, raw + bytes);
                        posNum = num;
                    }
                    else if (fmt == 9 && addr == 0x43)
                        uv.assign(raw, raw + bytes);
                    else if ((fmt == 0xE || fmt == 0xA) && addr == (env ? 0x43u : 0x83u))
                    {
                        nrm.assign(raw, raw + bytes);
                        nrmBytes = fmt == 0xE ? 4 : 3;
                    }
                    else if (fmt == 0xE && env && addr == 0x83)
                        col.assign(raw, raw + bytes);
                    i += n;
                }
                else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17)
                    kick();
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

        // Pass 2's environment map as FUN_004B6D80 uploads it (V2-32 x4 to VU 4): R = the object's matrix-pool rotation x
        // the camera matrix at *(gp 0x6749F0 + 0x110) + 0x1B0 (row vectors, FUN_00494500), then per vertex
        // u, v = 0.5 - 0.5 * (n . R[.][0], n . R[.][1]) with the packet's normal n (checked against every env upload of
        // saves 204040 / 204108: 230 of 233 within 1e-4, the rest moving objects).
        void envUvs(uint8_t *rdram, uint32_t index, std::vector<pop_assets::SkinnedBatch> &batches)
        {
            const uint32_t m = guestAny(rdram, 0x70000260u) + (index & 0xFFFF) * 0x40u;
            const uint32_t cam = guestAny(rdram, 0x006749F0u + 0x110u) + 0x1B0u;
            float r[3][2];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 2; ++j)
                {
                    float a = 0;
                    for (int k = 0; k < 3; ++k)
                        a += guestF(rdram, m + 16u * i + 4u * k) * guestF(rdram, cam + 16u * k + 4u * j);
                    r[i][j] = -0.5f * a;
                }
            for (auto &b : batches)
                for (size_t v = 0; 3 * v + 2 < b.normals.size(); ++v)
                {
                    const float *n = &b.normals[3 * v];
                    for (int j = 0; j < 2; ++j)
                        b.uvs[2 * v + j] = 0.5f + n[0] * r[0][j] + n[1] * r[1][j] + n[2] * r[2][j];
                }
        }
        std::unordered_map<uint64_t, std::vector<pop_assets::SkinnedBatch>> g_litBatches; // (element << 8 | pass) -> lit pass packet
        std::array<float, 16> poolMatrix(uint8_t *rdram, uint32_t index)
        {
            const uint32_t m = guestAny(rdram, 0x70000260u) + (index & 0xFFFF) * 0x40u;
            std::array<float, 16> model;
            for (int k = 0; k < 16; ++k)
                model[k] = guestF(rdram, m + 4u * k);
            return model;
        }
        // A lit static pass's vertices for this draw, in object space (drawn with poolMatrix(index)): pass 1 lit with the
        // light env, pass 2 with the environment map's UVs and the packet's colours (element: no palette)
        void litPose(uint8_t *rdram, uint32_t element, uint32_t index, uint32_t pass, const std::vector<pop_assets::SkinnedBatch> &bind,
                     std::vector<pop_assets::Batch> &out)
        {
            if (pass == 1)
            {
                const LightEnv light = g_noLighting ? LightEnv{} : readLightEnv(rdram);
                poseSkinned(rdram, element, index, bind, out, &light, true);
                return;
            }
            std::vector<pop_assets::SkinnedBatch> env = bind;
            envUvs(rdram, index, env);
            poseSkinned(rdram, element, index, env, out, nullptr, true);
        }

        std::vector<std::pair<std::vector<pop_assets::Batch>, std::pair<std::array<float, 16>, DrawMaterial>>> g_frameRefs; // instanced draws
        std::unordered_map<uint64_t, std::vector<pop_assets::Batch>> g_fromPacket; // (element << 8 | pass) -> decoded pass packet

        size_t worldCount0() { return g_frameMaterials.size(); }
        size_t worldCount1() { return g_framePosed.size(); }
        size_t worldCount2() { return g_frameRefs.size(); }
        DrawMaterial &worldMaterial(const WorldRecord &r)
        {
            return r.kind == 0 ? g_frameMaterials[r.index] : r.kind == 1 ? g_framePosed[r.index].second : g_frameRefs[r.index].second.second;
        }
        // A skipped CJMeshElement::Display still switches the VU1 skin driver as the call would have (the
        // draws the GS still does after it need the right one; skipping the switches dropped every save to
        // ~7 fps, 19 Oct). Before a skinned draw Display compares the state byte 0x700000A8 (0 = one-bone
        // driver, 1 = more) with the palette's bone count (word at the palette; < 2: one bone) and, if it
        // differs, moves the driver index 0x70000080 by -/+ the word 0x700000AC and sets the byte:
        // CVuManager::DoSetShadowDriver (0x4B5BC0, word 0x700000AC = 2) or FUN_004B7480 (otherwise), both
        // leaf functions that append one 2-qword packet (CNT 1; FLUSH; UNPACK V4-32 to VU 0x1D or 0x1E:
        // 0x8000, 0x303EC000, 0x412, index) and store the index at 0x70000080.
        void worldReplayDriver(uint8_t *rdram, uint32_t palette)
        {
            const uint8_t want = int32_t(guestAny(rdram, palette)) < 2 ? 0 : 1;
            if (guest8(rdram, 0x700000A8u) == want)
                return;
            const uint32_t step = guestAny(rdram, 0x700000ACu), now = guestAny(rdram, 0x70000080u);
            const uint32_t index = want ? now + step : now - step;
            const uint32_t gs = guestAny(rdram, 0x00675148u), cursor = guestAny(rdram, gs + 0xECu), next = cursor + 0x20u;
            const uint32_t qw[8] = {0x10000001u, 0u, 0x13000000u, step == 2 ? 0x6C01001Du : 0x6C01001Eu, 0x8000u, 0x303EC000u, 0x412u, index};
            std::memcpy(getMemPtr(rdram, cursor), qw, sizeof qw);
            std::memcpy(getMemPtr(rdram, gs + 0xECu), &next, 4);
            std::memcpy(getMemPtr(rdram, 0x70000080u), &index, 4);
            *getMemPtr(rdram, 0x700000A8u) = want;
            ++g_worldStats.drivers;
        }
        // A Display call's records go native if each has its texture: marks them, notes the drawn
        // bases (for their lightmap passes); false: the call runs as usual (its records stay GS-only)
        bool worldFinish(const std::vector<WorldRecord> &recs, const std::vector<uint64_t> &base)
        {
            for (const WorldRecord &r : recs)
            {
                const DrawMaterial &m = worldMaterial(r);
                if (!m.textureKey || !worldTextureReady(m.textureKey))
                {
                    static const bool whyLog = std::getenv("POP_NATIVE_WORLD_WHY") != nullptr;
                    if (whyLog)
                        std::fprintf(stderr, "[pop:world] no texture: key %08x list +0x%x flags %08x\n", m.textureKey, m.list, m.flags);
                    ++g_worldStats.noTexture;
                    ++g_worldStats.kept;
                    return false;
                }
            }
            for (const WorldRecord &r : recs)
                worldMaterial(r).native = true;
            g_worldBase.insert(base.begin(), base.end());
            ++g_worldStats.skipped;
            return true;
        }

        // CJMeshElementRef::Display(ref, matrix index, pass): the instance's pass packet is the word
        // at ref + 4 * pass (pass 4: 0; low 4 bits masked); the object's next ref is at +0x1C.
        void refDisplayRecord(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_refPassDraws[getRegU32(ctx, 6) & 7];
            // POP_NATIVE_WORLD: as CJMeshElement::Display; never instances with a lightmap entry
            // (instanced lightmaps are not drawn natively yet)
            bool worldOk = false;
            std::vector<WorldRecord> worldRecs;
            std::vector<uint64_t> worldBase;
            if (g_frameCapturing)
            {
                const uint32_t pass = getRegU32(ctx, 6) & 0xFF, index = getRegU32(ctx, 5) & 0xFFFF;
                uint32_t ref = getRegU32(ctx, 4);
                worldOk = g_world && pass != 3 && worldStateOk(rdram);
                // passes 0, 1, 4 (main) and 2 (second layer); 3 is the lightmap pass
                for (unsigned link = 0; ref && link < 4096 && (pass == 0 || pass == 1 || pass == 2 || pass == 4); ++link)
                {
                    // as the game: a ref without a packet for the pass draws nothing and ends the chain
                    // (CJMeshElementRef::Display's recursion is inside its packet test)
                    if (!(guestAny(rdram, ref + 4 * (pass == 4 ? 0 : pass)) & ~0xFu))
                        break;
                    // pass 2 (the second layer) has its own key: sharing pass 0's lost one of the two (22 Oct)
                    const uint64_t key = (uint64_t(ref) << 16) | index | (uint64_t(1) << 63) | alphaPassBit(rdram) |
                                         (pass == 2 ? uint64_t(2) << 56 : 0);
                    const uint64_t base = (uint64_t(ref) << 16) | index | (uint64_t(1) << 63);
                    const bool seen = worldOk && g_frameSeen.count(key);
                    const size_t world2 = worldOk ? worldCount2() : 0;
                    if (worldOk)
                    {
                        const uint32_t el = guestAny(rdram, ref + 0x20u);
                        if ((el && guestAny(rdram, el + 0x48u)) || g_worldLmRefs.count(ref))
                            worldOk = false;
                    }
                    if (g_frameSeen.insert(key).second)
                    {
                        const uint32_t packet = guestAny(rdram, ref + 4 * (pass == 4 ? 0 : pass)) & ~0xFu;
                        const uint32_t el = guestAny(rdram, ref + 0x20u);
                        if (el && !guestAny(rdram, el + 0x48u) && litDriverPass(rdram, el, pass))
                        {
                            // a lit pass (as CJMeshElement::Display's, litDriverPass with the ref's element): litPose
                            auto &bind = g_litBatches[(uint64_t(ref) << 8) | pass];
                            if (bind.empty() && packet)
                                litPacketBatches(rdram, packet, pass == 2, bind);
                            std::vector<pop_assets::Batch> posed;
                            litPose(rdram, el, index, pass, bind, posed);
                            if (!posed.empty())
                            {
                                DrawMaterial dm = currentMaterial(rdram);
                                dm.hasUvm = false; // the character program has no UV matrix
                                g_frameRefs.emplace_back(std::move(posed), std::make_pair(poolMatrix(rdram, index), dm));
                                ++(pass == 1 ? g_worldStats.litNative : g_worldStats.envNative);
                            }
                        }
                        else
                        {
                            std::vector<pop_assets::Batch> batches;
                            // texture UVs 1/512 (VU1 drivers: ITOF4 * 1/32); pass 4 sends pass 0's packet here
                            livePacketBatches(rdram, packet, 512.0, batches, pass == 0 || pass == 4);
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
                    }
                    if (worldOk)
                    {
                        if (worldCount2() > world2)
                            worldRecs.push_back({2, world2});
                        else if (!(seen && g_worldBase.count(base)))
                        {
                            worldOk = false;
                            ++g_worldStats.notRecorded;
                            static const bool whyLog = std::getenv("POP_NATIVE_WORLD_WHY") != nullptr;
                            if (whyLog)
                            {
                                const uint32_t packet = guestAny(rdram, ref + 4 * (pass == 4 ? 0 : pass)) & 0x01FFFFF0u;
                                std::fprintf(stderr, "[pop:world] not decoded: ref %08x pass %u list +0x%x element %08x packet %08x first tag %u\n", ref,
                                             pass, g_currentList, guestAny(rdram, ref + 0x20u), packet, packet ? (guestAny(rdram, packet) >> 28) & 7 : 0u);
                            }
                        }
                        worldBase.push_back(base);
                    }
                    ref = guestAny(rdram, ref + 0x1C);
                }
            }
            if (worldOk && worldFinish(worldRecs, worldBase))
            {
                if (g_currentList == 0x88u || g_currentList == 0xA8u)
                    ++g_lateStats.native;
                return;
            }
            worldOnGs(rdram, "ref");
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
#if POP_SPRITE_VU1
        bool particleRecord(uint8_t *rdram, uint32_t start, uint32_t end, bool &recorded); // P5, below
        const bool g_p5 = [] { const char *e = std::getenv("POP_NATIVE_P5"); return !(e && *e == '0'); }(); // on by default (user check 29 Oct)
        // true: handled (the original ran, or offline the P5 geometry is in the frame); false: the CPU quads below
        bool particlesP5(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            bool recorded = false;
            if (g_particlePackets)
            {
                // FUN_004584C0 writes nothing without a particle object: such a draw takes no range
                const uint32_t p1 = guestAny(rdram, getRegU32(ctx, 4) + 0x14);
                if (!p1 || !guestAny(rdram, p1))
                    return true;
                uint32_t end = 0;
                const uint32_t start = g_particlePackets(getRegU32(ctx, 4), end);
                particleRecord(rdram, start, end, recorded);
                return recorded;
            }
            // the game builds the packets; drawn natively, the DMA pointer goes back so the GS never sees them
            const uint32_t gso = guestAny(rdram, 0x00675148u), before = guestAny(rdram, gso + 0xEC), ra = getRegU32(ctx, 31);
            g_origParticles(rdram, ctx, runtime);
            if (ctx->pc != ra)
                return true; // the scheduler unwound inside the call (override rule): its packets go to the GS
            const uint32_t after = guestAny(rdram, gso + 0xEC);
            if (particleRecord(rdram, before, after, recorded))
            {
                std::memcpy(getMemPtr(rdram, gso + 0xEC), &before, 4);
                if (g_currentList == 0x88u || g_currentList == 0xA8u)
                    ++g_lateStats.native;
            }
            else if (after != before)
                worldOnGs(rdram, "gfx");
            return true;
        }
#endif
        void particles(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
#if POP_SPRITE_VU1
            if (g_frameCapturing && g_p5 && particlesP5(rdram, ctx, runtime))
                return;
#endif
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
                            dm.frameOnly = true;
                            g_frameRefs.emplace_back(std::move(bs), std::make_pair(model, dm));
                            // POP_NATIVE_WORLD with POP_NATIVE_LATE: drawn natively, the call skipped; FUN_004584C0 leaves
                            // the particle driver index at 0x70000080 (0 billboards, 1 oriented: read by the next SetParticleEnv)
                            std::vector<WorldRecord> recs{{2, g_frameRefs.size() - 1}};
                            if (g_world && g_worldLate && worldStateOk(rdram) && worldFinish(recs, {}))
                            {
                                const uint32_t driver = billboard ? 0u : 1u;
                                std::memcpy(getMemPtr(rdram, 0x70000080u), &driver, 4);
                                ++g_worldStats.particles;
                                if (g_currentList == 0x88u || g_currentList == 0xA8u)
                                    ++g_lateStats.native;
                                return;
                            }
                        }
                    }
                }
            }
            if (g_world && g_worldOpen)
            {
                // to the GS only if it draws anything (FUN_004584C0 sums the generators' counts first)
                const uint32_t element = getRegU32(ctx, 4), p1 = guestAny(rdram, element + 0x14), P = p1 ? guestAny(rdram, p1) : 0;
                const uint32_t gens = P ? guestAny(rdram, P + 0x28) : 0;
                uint32_t total = 0;
                for (uint32_t g = 0, n = gens ? guestAny(rdram, gens + 4) : 0; g < n && g < 64; ++g)
                    total += guest16(rdram, guestAny(rdram, gens) + g * 0xD0 + 0xC2);
                if (total)
                    worldOnGs(rdram, "gfx");
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
                                      sub ? textureKey(rdram, guestAny(rdram, sub) & 0xFFFF) : 0u, sub ? guestAny(rdram, sub + 4) : 0u, g_currentList});
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

#if POP_SPRITE_VU1
        // CJadeSpriteElement::Display (0x4C6200; element = *(0x6E1598) + u16 * 0x20, matrix index, kind 8 flag), 24 Oct.
        // Up to 4 slots: packet word at +4i (low 31 bits; 0 ends), sprite count at +0x10 + 4i. Per slot FUN_004B5F80
        // (kind 8; kind 7 takes SetParticleEnv and program 4: left to the GS) sends FLUSHA + UNPACK V4-32 of 19 qw to
        // VU 0 and DMA-CALLs the packet: qw 0-3 the model matrix (pool entry 0x70000090), 4-7 / 8-11 the graphics
        // object's +0x200 (world -> GS screen) / +0x240, 13 / 14 the camera's right / up (camera object +0x160 /
        // +0x170) turned into object space (sceVu0InversMatrix of the Display matrix: transposed rotation), 15 the fog
        // zone (0x70000270 + (0x70000084 & 0xFF) * 16, zero for zone -1), 16 the GIF tag (count | 0x8000, 0x603B4000,
        // 0x412412, 0: PRE, sprite with TME/FGE/ABE, ST RGBAQ XYZF2 x 2), 17.xy / 18.xy the screen box (2048 -+ half
        // the display size 0x66D03C / 0x66D040). The packet (STCYCL, BASE 0x18, OFFSET 0 = one buffer, MSCALF 0x68,
        // UNPACKs, MSCNT) runs P9 (ELF MPG packet 0x4E338C): 0x340 sets up and ends, the MSCNT goes on from there;
        // the first element of a group uploads its curve (8 points at TOP+0x10, 8 directions at TOP+0x3A), the next
        // ones only a colour (TOP+7) and their sprites (count TOP+100, 3 qw each from TOP+101), so the VU data memory
        // and the program's position carry over from draw to draw: one interpreter and one data image for all.
        // Output: GS sprites split into 32-pixel strips; each strip becomes a quad unprojected through +0x200.
        // The port (POP_NATIVE_P9=0: the interpreter only) keeps the registers in `port`; its data memory is the one
        // both run on. A run from 0x340 ends at the E bit at 0x380, so the packet's MSCNT goes on at 0x390; the
        // port does not report where other runs end (pc 0xffffffff), the interpreter does.
        // GFX particles (P5, ELF MPG 0x4D948C) run the same way: MSCALF 0x68 = 0x340 calls the matrix set-up at 0 and
        // ends at the E bit at 0x370, the MSCNT goes on at 0x380 (docs/native_renderer.md "P4 and P5 in the game").
        struct VuProg
        {
            const char *name;                 // for the log
            uint32_t mpg, size, resume;       // ELF MPG chain, its code bytes, where an MSCNT goes on after a run from 0x340
            void (*portRun)(VuState &);
            const char *env;                  // "0" or "interp": the interpreter only (to judge the port)
            std::unique_ptr<VU1Interpreter> vu;
            std::unique_ptr<VuState> port;
            std::vector<uint8_t> code;
            uint32_t tops = 0, base = 0, offset = 0, cl = 4, wl = 4, next = 0xFFFFFFFFu;
            bool dbf = false, loaded = false, broken = false, usePort = false;
            uint64_t runs = 0, interpreted = 0;
            uint8_t *data() { return port->mem; }
        };
        VuProg g_spriteVu{"sprite", 0x004E338Cu, 0x13A0u, 0x390u, vu1_p9::run, "POP_NATIVE_P9"};
        VuProg g_particleVu{"particle", 0x004D948Cu, 0x17C0u, 0x380u, vu1_p5::run, "POP_NATIVE_P5"};
        VuProg g_waterVu{"water", 0x004DAC8Cu, 0x2630u, 0xFFFFFFFFu, vu1_p6::run, "POP_NATIVE_P6"}; // MSCAL 0 runs only
        alignas(16) unsigned char g_spriteGsStandIn[64]; // the GS reference execute() wants; never touched (forward = false)
        struct SpriteStats
        {
            uint64_t draws = 0, native = 0, quads = 0, failed = 0;
        } g_spriteStats, g_particleStats, g_waterStats;

        bool vuLoad(VuProg &s, uint8_t *rdram)
        {
            // MPG chain as tools/vu1progs.py reads it: each MPG word followed by its code, the next MPG word after it
            s.code.assign(0x4000, 0);
            uint32_t p = s.mpg, next = 0;
            for (int k = 0; k < 64; ++k)
            {
                const uint32_t w = guestAny(rdram, p);
                if ((w >> 24) != 0x4A)
                    break;
                const uint32_t num = ((w >> 16) & 0xFF) ? ((w >> 16) & 0xFF) : 256u, addr = (w & 0xFFFF) * 8;
                if (addr != next || addr + num * 8 > 0x4000)
                    break;
                std::memcpy(s.code.data() + addr, getConstMemPtr(rdram, p + 4), num * 8);
                next = addr + num * 8;
                p += 4 + num * 8 + 4;
            }
            if (next != s.size)
            {
                std::fprintf(stderr, "[pop:native] %s program: read 0x%x bytes at 0x%X (expected 0x%x); %ss stay on the GS\n", s.name, next, s.mpg,
                             s.size, s.name);
                return false;
            }
            s.vu = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
            s.port = std::make_unique<VuState>();
            std::memset(s.port.get(), 0, sizeof(VuState));
            s.port->vf[0][3] = 1.0f;
            const char *e = std::getenv(s.env);
            // the P9 trace records interpreter runs
            s.usePort = !(&s == &g_spriteVu && g_spriteRunHook) && !(e && (*e == '0' || *e == 'i'));
            return true;
        }

        // the port's registers to the interpreter and back (stage borders: nothing in flight)
        void spriteToInterpreter(const VuState &v, VU1State &i)
        {
            std::memcpy(i.vf, v.vf, sizeof i.vf);
            std::memcpy(i.vi, v.vi, sizeof i.vi);
            std::memcpy(i.acc, v.acc, sizeof i.acc);
            i.q = v.q, i.p = v.p, i.i = v.i, i.r = v.r, i.mac = v.mac, i.clip = v.clip, i.status = v.status;
        }
        void spriteFromInterpreter(const VU1State &i, VuState &v)
        {
            std::memcpy(v.vf, i.vf, sizeof v.vf);
            std::memcpy(v.vi, i.vi, sizeof v.vi);
            std::memcpy(v.acc, i.acc, sizeof v.acc);
            v.q = i.q, v.p = i.p, v.i = i.i, v.r = i.r, v.mac = i.mac, v.clip = i.clip, v.status = i.status;
        }

        // one VU run: from `pc` (MSCAL/MSCALF) or on from where the last one ended (MSCNT, pc < 0); false = the
        // port cannot tell where to go on (the sprites then stay on the GS for the session)
        bool vuRun(VuProg &s, int32_t pc)
        {
            GS &gs = *reinterpret_cast<GS *>(g_spriteGsStandIn);
            const unsigned csr = _mm_getcsr();
            _mm_setcsr(csr | 0x6000u); // round toward zero, as the runtime runs VU code
            ++s.runs;
            if (s.usePort)
            {
                VuState &v = *s.port;
                const uint32_t entry = pc >= 0 ? uint32_t(pc) : s.next;
                if (entry == 0xFFFFFFFFu)
                {
                    _mm_setcsr(csr);
                    std::fprintf(stderr, "[pop:native] %s program: MSCNT after a run the port cannot place; %ss stay on the GS\n", s.name, s.name);
                    return false;
                }
                v.pc = entry;
                v.top = s.tops;
                s.portRun(v);
                s.next = entry == 0x340 ? s.resume : 0xFFFFFFFFu;
                if (v.pc != 0xFFFFFFFFu)
                {
                    // a pc no stage starts at (a branch the traces never took): the interpreter takes the run over
                    if (s.interpreted++ < 8)
                        std::fprintf(stderr, "[pop:native] %s program: no port stage at pc 0x%x (run from 0x%x); interpreter\n", s.name, v.pc, entry);
                    spriteToInterpreter(v, s.vu->state());
                    s.vu->execute(s.code.data(), 0x4000, s.data(), 0x4000, gs, nullptr, v.pc, s.tops, 0, 1u << 20);
                    spriteFromInterpreter(s.vu->state(), v);
                    s.next = s.vu->state().pc;
                }
            }
            else if (g_spriteRunHook && &s == &g_spriteVu)
                g_spriteRunHook(*s.vu, s.code.data(), s.data(), gs, pc, s.tops);
            else if (pc >= 0)
                s.vu->execute(s.code.data(), 0x4000, s.data(), 0x4000, gs, nullptr, uint32_t(pc), s.tops, 0, 1u << 20);
            else
                s.vu->resume(s.code.data(), 0x4000, s.data(), 0x4000, gs, nullptr, s.tops, 0, 1u << 20);
            _mm_setcsr(csr);
            return true;
        }

        // VIF codes of one DMA packet (source chain, TTE: each tag's upper 64 bits are two VIF codes; CALL / RET nest),
        // up to `end` if given (the particle records are a run of CNTs)
        bool vuVif(VuProg &s, uint8_t *rdram, uint32_t start, uint32_t end = 0)
        {
            std::vector<uint32_t> words;
            uint32_t addr = start;
            std::vector<uint32_t> stack;
            for (int rec = 0;; ++rec)
            {
                if (rec > 4096 || words.size() > (1u << 18))
                    return false;
                if (end && addr >= end)
                    break;
                const uint32_t t0 = guestAny(rdram, addr), t1 = guestAny(rdram, addr + 4);
                const uint32_t qwc = t0 & 0xFFFF, id = (t0 >> 28) & 7, ta = t1 & 0x7FFFFFF0u;
                words.push_back(guestAny(rdram, addr + 8));
                words.push_back(guestAny(rdram, addr + 12));
                const uint32_t data = (id == 0 || id == 3 || id == 4) ? ta : addr + 16, after = addr + 16 + ((id == 0 || id == 3 || id == 4) ? 0 : qwc * 16);
                for (uint32_t k = 0; k < qwc * 4; ++k)
                    words.push_back(guestAny(rdram, data + 4 * k));
                if (id == 0 || id == 7)
                    break;
                if (id == 2)
                    addr = ta;
                else if (id == 5)
                    stack.push_back(after), addr = ta;
                else if (id == 6)
                {
                    if (stack.empty())
                        break;
                    addr = stack.back();
                    stack.pop_back();
                }
                else
                    addr = after;
            }
            for (size_t i = 0; i < words.size();)
            {
                const uint32_t w = words[i++], cmd = (w >> 24) & 0x7F, num = (w >> 16) & 0xFF, imm = w & 0xFFFF;
                if (cmd >= 0x60)
                {
                    if (cmd & 0x10)
                        return false; // masked unpack: not used by these packets
                    const uint32_t vl = cmd & 3, vn = (cmd >> 2) & 3, comps = vn + 1, n = num ? num : 256;
                    if (vl == 3)
                        return false;
                    const uint32_t bytesPer = (4u >> vl) * comps, writes = n;
                    const uint32_t srcVectors = s.cl >= s.wl ? writes : (writes / s.wl) * s.cl + std::min(writes % s.wl, s.cl);
                    const size_t wordsIn = (size_t(srcVectors) * bytesPer + 3) / 4;
                    if (i + wordsIn > words.size())
                        return false;
                    const uint8_t *src = reinterpret_cast<const uint8_t *>(words.data() + i);
                    uint32_t vu = imm & 0x3FF;
                    if (imm & 0x8000)
                        vu = (vu + s.tops) & 0x3FF;
                    const bool usn = (imm & 0x4000) != 0;
                    for (uint32_t k = 0; k < writes; ++k)
                    {
                        const uint32_t dest = (vu + (k / s.wl) * s.cl + k % s.wl) & 0x3FF;
                        uint32_t v[4];
                        std::memcpy(v, s.data() + dest * 16, 16);
                        const uint8_t *sv = src + size_t(k) * bytesPer;
                        for (uint32_t c = 0; c < comps; ++c)
                        {
                            if (vl == 0)
                                std::memcpy(&v[c], sv + 4 * c, 4);
                            else if (vl == 1)
                            {
                                uint16_t r;
                                std::memcpy(&r, sv + 2 * c, 2);
                                v[c] = usn ? r : uint32_t(int32_t(int16_t(r)));
                            }
                            else
                                v[c] = usn ? sv[c] : uint32_t(int32_t(int8_t(sv[c])));
                        }
                        if (comps == 1)
                            v[1] = v[2] = v[3] = v[0];
                        std::memcpy(s.data() + dest * 16, v, 16);
                    }
                    i += wordsIn;
                    continue;
                }
                switch (cmd)
                {
                case 0x00: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13: case 0x04:
                    break; // NOP, MSKPATH3, MARK, FLUSHE, FLUSH, FLUSHA, ITOP (unused by P9)
                case 0x01:
                    s.cl = imm & 0xFF ? imm & 0xFF : 1;
                    s.wl = (imm >> 8) & 0xFF ? (imm >> 8) & 0xFF : 1;
                    break;
                case 0x02: // OFFSET also resets the double buffer to BASE
                    s.offset = imm & 0x3FF;
                    s.dbf = false;
                    s.tops = s.base;
                    break;
                case 0x03:
                    s.base = imm & 0x3FF;
                    break;
                case 0x05:
                    if (imm & 3)
                        return false; // STMOD add / accumulate: not used here
                    break;
                case 0x14: case 0x15:
                case 0x17:
                    if (!vuRun(s, cmd == 0x17 ? -1 : int32_t(imm) * 8))
                    {
                        s.broken = true;
                        return false;
                    }
                    s.dbf = !s.dbf; // the run took TOPS; the next unpacks go to the other buffer
                    s.tops = (s.base + (s.dbf ? s.offset : 0)) & 0x3FF;
                    break;
                case 0x20:
                    ++i;
                    break; // STMASK
                case 0x30: case 0x31:
                    i += 4;
                    break; // STROW / STCOL
                case 0x4A:
                    return false; // MPG inside a sprite packet: not expected
                case 0x50: case 0x51: // DIRECT: GS registers (the water packets set TEX0 etc.), not VU data
                    i += size_t(imm ? imm : 65536) * 4;
                    break;
                default:
                    return false;
                }
            }
            return true;
        }

        // XGKICK packets (PACKED ST / RGBAQ / XYZF2) -> world-space geometry: each GS screen point unprojected through the
        // graphics object's +0x200 (world -> GS screen); sprites (two corners) as quads, triangle strips / fans as triangles.
        // false: a primitive these programs do not send (the call then goes to the GS). fog: each triangle vertex's XYZF2 F
        bool gifToBatch(uint8_t *rdram, const std::vector<std::vector<uint8_t>> &packets, pop_assets::Batch &b, uint64_t &prims,
                        std::vector<float> *fog = nullptr)
        {
            const uint32_t gso = guestAny(rdram, 0x00675148u);
            bool ok = true;
            // world <- GS screen: solve M p = s for the screen point (x, y, z) (M = +0x200, PS2 layout: rows of memory are
            // the columns: s_c = sum_k M[k][c] p_k + M[3][c], divided by the same with c = 3)
            float M[4][4];
            for (int k = 0; k < 4; ++k)
                for (int c = 0; c < 4; ++c)
                    M[k][c] = guestF(rdram, gso + 0x200u + 16 * k + 4 * c);
            const auto unproject = [&M](double x, double y, double z, float out[3]) {
                const double sc[3] = {x, y, z};
                double A[3][4];
                for (int c = 0; c < 3; ++c)
                {
                    for (int k = 0; k < 3; ++k)
                        A[c][k] = double(M[k][c]) - sc[c] * double(M[k][3]);
                    A[c][3] = sc[c] * double(M[3][3]) - double(M[3][c]);
                }
                for (int col = 0; col < 3; ++col) // Gaussian elimination with partial pivoting
                {
                    int piv = col;
                    for (int r = col + 1; r < 3; ++r)
                        if (std::fabs(A[r][col]) > std::fabs(A[piv][col]))
                            piv = r;
                    std::swap(A[col], A[piv]);
                    if (std::fabs(A[col][col]) < 1e-12)
                        return false;
                    for (int r = 0; r < 3; ++r)
                        if (r != col)
                        {
                            const double f = A[r][col] / A[col][col];
                            for (int k = col; k < 4; ++k)
                                A[r][k] -= f * A[col][k];
                        }
                }
                for (int k = 0; k < 3; ++k)
                    out[k] = float(A[k][3] / A[k][k]);
                return true;
            };
            uint32_t prim = 6, nv = 0;
            pop_native::Vertex tri[3]{};
            bool ok3[3]{};
            float triF[3]{};
            for (const auto &pk : packets)
            {
                size_t q = 0;
                const size_t nq = pk.size() / 16;
                while (q < nq)
                {
                    uint64_t lo, hi;
                    std::memcpy(&lo, pk.data() + q * 16, 8);
                    std::memcpy(&hi, pk.data() + q * 16 + 8, 8);
                    ++q;
                    const uint32_t nloop = uint32_t(lo & 0x7FFF), flg = uint32_t(lo >> 58) & 3, nreg = (uint32_t(lo >> 60) & 0xF) ? uint32_t(lo >> 60) & 0xF : 16;
                    const bool eop = (lo >> 15) & 1;
                    if (flg != 0)
                    {
                        q += flg == 1 ? (size_t(nloop) * nreg + 1) / 2 : nloop; // not PACKED: not P9's
                        if (eop)
                            break;
                        continue;
                    }
                    if ((lo >> 46) & 1)
                    {
                        prim = uint32_t(lo >> 47) & 0x7FF;
                        nv = 0;
                        if ((prim & 7) < 4 || (prim & 7) > 6)
                        {
                            ok = false; // P5 sends strips and fans, P9 sprites; anything else goes to the GS
                            break;
                        }
                    }
                    float S = 0, T = 0, Q = 1;
                    uint32_t rgba = 0;
                    struct Corner
                    {
                        float x, y, u, v;
                        uint32_t z;
                    } c[2];
                    int nc = 0;
                    for (uint32_t l = 0; l < nloop && q < nq; ++l)
                        for (uint32_t r = 0; r < nreg && q < nq; ++r, ++q)
                        {
                            const uint32_t reg = uint32_t(hi >> (4 * r)) & 0xF;
                            uint32_t v[4];
                            std::memcpy(v, pk.data() + q * 16, 16);
                            if (reg == 2) // ST (+ Q in PACKED)
                            {
                                std::memcpy(&S, &v[0], 4);
                                std::memcpy(&T, &v[1], 4);
                                std::memcpy(&Q, &v[2], 4);
                            }
                            else if (reg == 1)
                                rgba = (v[0] & 0xFF) | (v[1] & 0xFF) << 8 | (v[2] & 0xFF) << 16 | (v[3] & 0xFF) << 24;
                            else if ((reg == 4 || reg == 5) && (prim & 7) != 6) // XYZF2 / XYZ2 of a triangle strip or fan
                            {
                                const bool adc = (v[3] >> 15) & 1;
                                const float qq = Q != 0.0f ? Q : 1.0f;
                                pop_native::Vertex vx{};
                                const bool good = unproject(float(v[0] & 0xFFFF) / 16.0f, float(v[1] & 0xFFFF) / 16.0f,
                                                            double(reg == 4 ? (v[2] >> 4) & 0xFFFFFF : v[2]), vx.pos);
                                vx.uv[0] = S / qq;
                                vx.uv[1] = T / qq;
                                for (int ch = 0; ch < 4; ++ch)
                                    vx.colour[ch] = float((rgba >> (8 * ch)) & 0xFF) / 128.0f;
                                // the GS vertex queue: strip = the last three, fan = the first and the last two
                                if ((prim & 7) == 4 && nv == 3)
                                    tri[0] = tri[1], tri[1] = tri[2], ok3[0] = ok3[1], ok3[1] = ok3[2], triF[0] = triF[1], triF[1] = triF[2], nv = 2;
                                else if ((prim & 7) == 5 && nv == 3)
                                    tri[1] = tri[2], ok3[1] = ok3[2], triF[1] = triF[2], nv = 2;
                                triF[nv] = reg == 4 ? float(v[3] >> 4 & 0xFF) : 255.0f;
                                tri[nv] = vx;
                                ok3[nv++] = good;
                                if (nv == 3 && !adc && ok3[0] && ok3[1] && ok3[2])
                                {
                                    const uint32_t base0 = uint32_t(b.vertices.size());
                                    for (int k = 0; k < 3; ++k)
                                    {
                                        b.vertices.push_back(tri[k]);
                                        if (fog)
                                            fog->push_back(triF[k]);
                                        if (!(prim & 8)) // flat shading: the last vertex's colour
                                            std::memcpy(b.vertices.back().colour, tri[2].colour, sizeof tri[2].colour);
                                    }
                                    b.indices.insert(b.indices.end(), {base0, base0 + 1, base0 + 2});
                                    ++prims;
                                }
                            }
                            else if (reg == 4 || reg == 5) // XYZF2 / XYZ2 of a sprite
                            {
                                const bool adc = (v[3] >> 15) & 1;
                                const float qq = Q != 0.0f ? Q : 1.0f;
                                c[nc] = {float(v[0] & 0xFFFF) / 16.0f, float(v[1] & 0xFFFF) / 16.0f, S / qq, T / qq,
                                         reg == 4 ? (v[2] >> 4) & 0xFFFFFF : v[2]};
                                if (++nc == 2)
                                {
                                    nc = 0;
                                    if (adc)
                                        continue;
                                    static const bool spriteLog = std::getenv("POP_SPRITE_LOG") != nullptr;
                                    static int spriteLogged = 0;
                                    if (spriteLog && spriteLogged++ < 24)
                                        std::fprintf(stderr, "[pop:sprite] (%.1f %.1f) uv (%.3f %.3f) - (%.1f %.1f) uv (%.3f %.3f) z %u rgba %08x Q %.4g\n", c[0].x,
                                                     c[0].y, c[0].u, c[0].v, c[1].x, c[1].y, c[1].u, c[1].v, c[1].z, rgba, Q);
                                    // a sprite: corners 0 and 1, colour and Z of the second
                                    const float xs[4] = {c[0].x, c[1].x, c[1].x, c[0].x}, ys[4] = {c[0].y, c[0].y, c[1].y, c[1].y};
                                    const float us[4] = {c[0].u, c[1].u, c[1].u, c[0].u}, vs[4] = {c[0].v, c[0].v, c[1].v, c[1].v};
                                    const uint32_t base0 = uint32_t(b.vertices.size());
                                    bool good = true;
                                    for (int k = 0; k < 4 && good; ++k)
                                    {
                                        pop_native::Vertex vx{};
                                        good = unproject(xs[k], ys[k], double(c[1].z), vx.pos);
                                        vx.uv[0] = us[k];
                                        vx.uv[1] = vs[k];
                                        for (int ch = 0; ch < 4; ++ch)
                                            vx.colour[ch] = float((rgba >> (8 * ch)) & 0xFF) / 128.0f;
                                        b.vertices.push_back(vx);
                                    }
                                    if (!good)
                                    {
                                        b.vertices.resize(base0);
                                        continue;
                                    }
                                    b.indices.insert(b.indices.end(), {base0, base0 + 1, base0 + 2, base0, base0 + 2, base0 + 3});
                                    ++prims;
                                }
                            }
                        }
                    if (eop || !ok)
                        break;
                }
            }
            return ok;
        }

        // the frame's sprites drawn natively: false = this call goes to the GS (it is emulated all the same, so the
        // data later elements reuse stays right)
        bool spriteRecord(uint8_t *rdram, R5900Context *ctx)
        {
            VuProg &s = g_spriteVu;
            static const bool off = [] { const char *e = std::getenv("POP_NATIVE_SPRITES"); return e && *e == '0'; }();
            if (off || s.broken || getRegU32(ctx, 6) == 0)
                return false; // kind 7: SetParticleEnv and program 4, not read
            if (!s.loaded)
            {
                s.loaded = true;
                s.broken = !vuLoad(s, rdram);
                if (s.broken)
                    return false;
            }
            ++g_spriteStats.draws;
            // the world frame already went back to the GS: its native records are not drawn, so do not interpret
            if (g_world && !(g_worldLate && worldStateOk(rdram)))
                return false;
            const uint32_t element = getRegU32(ctx, 4), index = getRegU32(ctx, 5) & 0xFFFF;
            const uint32_t gso = guestAny(rdram, 0x00675148u), cam = guestAny(rdram, 0x006749F0u + 0x110u);
            const uint32_t pool = guestAny(rdram, 0x70000260u), model = pool + (guest16(rdram, 0x70000090u)) * 0x40u, disp = pool + index * 0x40u;
            float right[3], up[3];
            for (int j = 0; j < 3; ++j)
            {
                // sceVu0InversMatrix (rotation transposed), then sceVu0ApplyMatrix with w = 0: component j = row j . v
                right[j] = up[j] = 0;
                for (int k = 0; k < 3; ++k)
                {
                    right[j] += guestF(rdram, disp + 16 * j + 4 * k) * guestF(rdram, cam + 0x160 + 4 * k);
                    up[j] += guestF(rdram, disp + 16 * j + 4 * k) * guestF(rdram, cam + 0x170 + 4 * k);
                }
            }
            const float w = float(int32_t(guestAny(rdram, 0x0066D03Cu))), h = float(int32_t(guestAny(rdram, 0x0066D040u)));
            vu1_impl::Vu1PacketSink sink;
            sink.forward = false;
            bool ok = true;
            for (uint32_t slot = 0; slot < 4; ++slot)
            {
                const uint32_t packet = guestAny(rdram, element + 4 * slot) & 0x7FFFFFFFu;
                if (!guestAny(rdram, element + 4 * slot))
                    break;
                uint8_t *d = s.data();
                std::memcpy(d + 0 * 16, getConstMemPtr(rdram, model), 64);
                std::memcpy(d + 4 * 16, getConstMemPtr(rdram, gso + 0x200u), 64);
                std::memcpy(d + 8 * 16, getConstMemPtr(rdram, gso + 0x240u), 64);
                std::memset(d + 12 * 16, 0, 16); // not written by FUN_004B5F80 (stale DMA buffer words; P9 never reads them)
                const float r4[4] = {right[0], right[1], right[2], 0}, u4[4] = {up[0], up[1], up[2], 0};
                std::memcpy(d + 13 * 16, r4, 16);
                std::memcpy(d + 14 * 16, u4, 16);
                const uint32_t zone = guestAny(rdram, 0x70000084u);
                if (zone == 0xFFFFFFFFu)
                    std::memset(d + 15 * 16, 0, 16);
                else
                    std::memcpy(d + 15 * 16, getConstMemPtr(rdram, 0x70000270u + (zone & 0xFF) * 16), 16);
                const uint32_t tag[4] = {guestAny(rdram, element + 0x10 + 4 * slot) | 0x8000u, 0x603B4000u, 0x412412u, 0};
                std::memcpy(d + 16 * 16, tag, 16);
                const float lo[4] = {2048.0f - w / 2.0f, 2048.0f - h / 2.0f, 0, 0}, hi[4] = {2048.0f + w / 2.0f, 2048.0f + h / 2.0f, 0, 0};
                std::memcpy(d + 17 * 16, lo, 16);
                std::memcpy(d + 18 * 16, hi, 16);
                vu1_impl::packetSink = &sink;
                ok = vuVif(s, rdram, packet) && ok;
                vu1_impl::packetSink = nullptr;
            }
            if (!ok)
            {
                if (g_spriteStats.failed++ < 4)
                    std::fprintf(stderr, "[pop:native] sprite element %08x: packet not replayed; on the GS\n", element);
                return false;
            }
            pop_assets::Batch b;
            ok = gifToBatch(rdram, sink.packets, b, g_spriteStats.quads);
            static int logged = 0;
            if (logged++ < 6)
                std::fprintf(stderr, "[pop:native] sprite element %08x matrix %u: %zu packets, %zu quads%s\n", element, index, sink.packets.size(),
                             b.vertices.size() / 4, ok ? "" : " (unexpected primitive: on the GS)");
            if (!ok)
                return false;
            if (b.indices.empty())
            {
                // every sprite culled or clipped away: the call draws nothing, so skipping it keeps the frame pure
                if (g_world && g_worldLate && worldStateOk(rdram))
                {
                    ++g_spriteStats.native;
                    return true;
                }
                return false;
            }
            std::vector<pop_assets::Batch> bs;
            bs.push_back(std::move(b));
            DrawMaterial dm = currentMaterial(rdram);
            dm.hasUvm = false;
            dm.constColour = dm.constAlpha = -1; // P9 writes RGBAQ itself
            dm.frameOnly = true;
            if (logged <= 6)
                std::fprintf(stderr, "[pop:native] sprite material: texture %08x flags %08x ref %08x\n", dm.textureKey, dm.flags, dm.ref);
            const std::array<float, 16> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            g_frameRefs.emplace_back(std::move(bs), std::make_pair(identity, dm));
            std::vector<WorldRecord> recs{{2, g_frameRefs.size() - 1}};
            if (g_world && g_worldLate && worldStateOk(rdram) && worldFinish(recs, {}))
            {
                ++g_spriteStats.native;
                return true;
            }
            return false;
        }

        // GFX particles through P5 (on by default, POP_NATIVE_P5=0 off; =interp: the interpreter only): the DMA records
        // FUN_004584C0 wrote, [start, end) (SetParticleEnv's 23 qw at VU 0, then CGraphicParticle's batch at TOPS: STCYCL,
        // BASE 0x18, OFFSET 0, MSCALF 0x68, UNPACKs, MSCNT; tools/p5packets.py lists them in a save), replayed into the
        // program's data image; its triangle strips become world-space triangles. recorded: the geometry is in the
        // frame (offline that is all); true: drawn natively, the packets must not reach the GS.
        bool particleRecord(uint8_t *rdram, uint32_t start, uint32_t end, bool &recorded)
        {
            VuProg &s = g_particleVu;
            recorded = false;
            if (s.broken || end <= start)
                return false;
            if (!s.loaded)
            {
                s.loaded = true;
                s.broken = !vuLoad(s, rdram);
                if (s.broken)
                    return false;
            }
            ++g_particleStats.draws;
            if (g_world && !(g_worldLate && worldStateOk(rdram)))
                return false;
            vu1_impl::Vu1PacketSink sink;
            sink.forward = false;
            vu1_impl::packetSink = &sink;
            const bool ok = vuVif(s, rdram, start, end);
            vu1_impl::packetSink = nullptr;
            pop_assets::Batch b;
            if (!ok || !gifToBatch(rdram, sink.packets, b, g_particleStats.quads))
            {
                if (g_particleStats.failed++ < 4)
                    std::fprintf(stderr, "[pop:native] particle packets %08x-%08x: not replayed; on the GS\n", start, end);
                return false;
            }
            static int logged = 0;
            if (logged++ < 6)
                std::fprintf(stderr, "[pop:native] particles (P5 %s) %08x-%08x: %zu packets, %zu triangles\n", s.usePort ? "port" : "interpreter",
                             start, end, sink.packets.size(), b.indices.size() / 3);
            recorded = true;
            if (!b.indices.empty())
            {
                std::vector<pop_assets::Batch> bs;
                bs.push_back(std::move(b));
                DrawMaterial dm = currentMaterial(rdram);
                dm.hasUvm = false;
                dm.constColour = dm.constAlpha = -1; // P5 writes RGBAQ itself
                dm.frameOnly = true;
                const std::array<float, 16> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                g_frameRefs.emplace_back(std::move(bs), std::make_pair(identity, dm));
                std::vector<WorldRecord> recs{{2, g_frameRefs.size() - 1}};
                if (!(g_world && g_worldLate && worldStateOk(rdram) && worldFinish(recs, {})))
                    return false;
            }
            else if (!(g_world && g_worldLate && worldStateOk(rdram)))
                return false; // every particle clipped away: skipping the call keeps the frame pure
            ++g_particleStats.native;
            ++g_worldStats.particles;
            return true;
        }

        // water through P6 (on by default, user check 29 Oct; POP_NATIVE_P6=0 off; =interp: the interpreter only). CJSmWaterElement::Display
        // CALLs the element's ready packet (*element & 0x0FFFFFFF: BASE / OFFSET (double buffered), the 12 matrix qw
        // from 0x6C5BC0 at VU 1, a DIRECT of GS registers, then per patch 4 UNPACKs at TOPS and MSCAL 0; tools/p6packets.py
        // lists a save's). A refracting water (element +4) is drawn by CElementList::Render as FUN_004A76C0 (the frame's
        // alpha cleared to 0), Display (the alpha mask: alpha 0x80 where the water passes ZGEQUAL), FUN_004A6F40 (the
        // frame copied into a 256x256 buffer where the mask is), Display (the colour pass, RGB only, ALPHA 0x44 from the
        // buffer at P6's ST): docs/native_renderer.md "The refraction copy". Here the colour pass's call records all
        // three as draws: the clear (a full-screen sprite), the mask (the same triangles) and the colour pass (image -2:
        // the frame copied before it, read through the buffer in mesh.frag). Live the mask call (returning to 0x4B9FF8) is
        // skipped when the colour pass can be native (a GS mask would close the frame's purity); FUN_004A76C0 and the copy
        // still run on the GS (alpha and the 256x256 buffer only). shortcut: if the colour pass then fails to replay, the GS
        // draws it with no mask, so that water is missing for the frame (logged; none in the saves); skip the copy at 0.2.
        bool waterRecord(uint8_t *rdram, R5900Context *ctx)
        {
            VuProg &s = g_waterVu;
            static const bool on = [] { const char *e = std::getenv("POP_NATIVE_P6"); return !(e && *e == '0'); }();
            const uint32_t element = getRegU32(ctx, 4);
            if (!on || s.broken || !guestAny(rdram, element + 4))
                return false; // a water without refraction: none in the saves (its packet's DIRECT would set the texture)
            if (getRegU32(ctx, 31) == 0x004B9FF8u)
                return g_world && g_worldLate && worldStateOk(rdram); // the mask: drawn by the colour pass's call
            if (!s.loaded)
            {
                s.loaded = true;
                s.broken = !vuLoad(s, rdram);
                if (s.broken)
                    return false;
            }
            ++g_waterStats.draws;
            if (g_world && !(g_worldLate && worldStateOk(rdram)))
                return false;
            const uint32_t packet = guestAny(rdram, element) & 0x0FFFFFFFu;
            vu1_impl::Vu1PacketSink sink;
            sink.forward = false;
            vu1_impl::packetSink = &sink;
            const bool ok = vuVif(s, rdram, packet);
            vu1_impl::packetSink = nullptr;
            pop_assets::Batch b;
            std::vector<float> fogF;
            if (!ok || !gifToBatch(rdram, sink.packets, b, g_waterStats.quads, &fogF))
            {
                if (g_waterStats.failed++ < 4)
                    std::fprintf(stderr, "[pop:native] water element %08x packet %08x: not replayed; on the GS\n", element, packet);
                return false;
            }
            static int logged = 0;
            if (logged++ < 6)
                std::fprintf(stderr, "[pop:native] water (P6 %s) element %08x packet %08x: %zu packets, %zu triangles\n",
                             s.usePort ? "port" : "interpreter", element, packet, sink.packets.size(), b.indices.size() / 3);
            if (b.indices.empty())
                return false;
            // P6's own fog: F per vertex (XYZF2), about 225-255 in the saves, far less than the zone's line (the colour pass
            // copies a frame that is fogged already). The renderer's F is a + b * w per draw: a least-squares line through
            // P6's (w, F), clamped to their range. shortcut: up to ~18 off (fountain, 225..255, < 5 elsewhere); F per vertex if it shows.
            const uint32_t gso = guestAny(rdram, 0x00675148u);
            double sw = 0, sf = 0, sww = 0, swf = 0, fmin = 255, fmax = 0;
            for (size_t i = 0; i < b.vertices.size() && i < fogF.size(); ++i)
            {
                double w = guestF(rdram, gso + 0x27Cu); // w = the clip matrix's (+0x240, row vectors) last column
                for (int k = 0; k < 3; ++k)
                    w += double(guestF(rdram, gso + 0x240u + 16 * k + 12)) * b.vertices[i].pos[k];
                sw += w, sf += fogF[i], sww += w * w, swf += w * fogF[i];
                fmin = std::min(fmin, double(fogF[i])), fmax = std::max(fmax, double(fogF[i]));
            }
            const double nf = double(std::min(b.vertices.size(), fogF.size())), det = nf * sww - sw * sw;
            const double fogB = nf > 1 && std::fabs(det) > 1e-9 ? (nf * swf - sw * sf) / det : 0.0, fogA = nf > 0 ? (sf - fogB * sw) / nf : 255.0;
            // the clear: a GS sprite over the display (2048 -+ half its size, as FUN_004A76C0 draws it), colour and alpha 0,
            // at the water's first GS Z (any Z inside the view: the clear does not test depth)
            uint32_t z = 0;
            if (sink.packets[0].size() >= 64)
                std::memcpy(&z, sink.packets[0].data() + 3 * 16 + 8, 4), z = (z >> 4) & 0xFFFFFF;
            const uint32_t w = guestAny(rdram, 0x0066D03Cu), h = guestAny(rdram, 0x0066D040u);
            std::vector<uint8_t> clear(64, 0);
            const uint64_t tag[2] = {1u | (1u << 15) | (uint64_t(1) << 46) | (uint64_t(6) << 47) | (uint64_t(3) << 60), 0x551};
            const uint32_t xy0[4] = {(2048 - w / 2) * 16, (2048 - h / 2) * 16, z, 0}, xy1[4] = {(2048 + w / 2) * 16, (2048 + h / 2) * 16, z, 0};
            std::memcpy(clear.data(), tag, 16);
            std::memcpy(clear.data() + 32, xy0, 16);
            std::memcpy(clear.data() + 48, xy1, 16);
            pop_assets::Batch cb;
            uint64_t unused = 0;
            if (!gifToBatch(rdram, {clear}, cb, unused) || cb.indices.empty())
                return false;
            pop_assets::Batch after = cb, mask = b;
            for (auto &v : mask.vertices)
                v.colour[0] = v.colour[1] = v.colour[2] = v.colour[3] = 1.0f; // RGBAQ 0x80808080 (FUN_004A76C0)
            DrawMaterial dm = currentMaterial(rdram);
            dm.hasUvm = false;
            dm.constColour = dm.constAlpha = -1; // P6 writes RGBAQ itself
            dm.frameOnly = true;
            dm.ref = 0;
            const std::array<float, 16> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::vector<WorldRecord> recs;
            // (geometry, texture key, render state flags: 0x200 = no depth write, blend kind 1 = ALPHA 0x44; Z test; FBMSK)
            const auto add = [&](pop_assets::Batch &&g, uint32_t key, uint32_t flags, uint32_t zTest, uint32_t fbmsk) {
                DrawMaterial m = dm;
                m.seq = g_frameSeq++;
                m.textureKey = key;
                m.flags = flags;
                m.zTest = zTest;
                m.fbmsk = fbmsk;
                std::vector<pop_assets::Batch> bs;
                bs.push_back(std::move(g));
                g_frameRefs.emplace_back(std::move(bs), std::make_pair(identity, m));
                recs.push_back({2, g_frameRefs.size() - 1});
            };
            add(std::move(cb), 0, 0x200, 0, 0x00FFFFFFu);              // alpha only, no Z test
            add(std::move(mask), 0, 0x200, 1, 0x00FFFFFFu);            // alpha only, ZGEQUAL
            add(std::move(b), kRefractionKey, 0x10200, 1, 0xFF000000u); // RGB only
            DrawMaterial &cm = g_frameRefs.back().second.second;
            cm.fogA = float(fogA), cm.fogB = float(fogB), cm.fogMin = float(fmin), cm.fogMax = float(fmax);
            // the water leaves its mask in the frame's alpha; before list +0x98 EraseAlphaBuffer clears it on the GS, while
            // natively draws before +0x98 never write alpha (currentMaterial): clear it again so that stays true
            if (g_currentList != 0xA8u)
                add(std::move(after), 0, 0x200, 0, 0x00FFFFFFu);
            if (!(g_world && g_worldLate))
                return false; // offline: the draws stay in the frame, the GS emulation is not used
            for (const WorldRecord &r : recs) // not worldFinish: these draws use no texture (key 0 or the frame copy)
                worldMaterial(r).native = true;
            ++g_waterStats.native;
            return true;
        }

        // Halos (FUN_004AA240 = AE_ProcessOneHalo, per entry of the halo array from FUN_004ABFC0 at the end of Display3DElements).
        // The game projects the light (+0x240, then the viewport +0x280), takes a core square (+0x10 / w of half the display
        // width, at least 8 px, clipped to the display) and ProcessLensFlare (FUN_004AB500) measures how much of it is in front
        // of the scene: alpha 0 over the core, the core again at the light's own Z with ZGEQUAL writing alpha = the colour's top
        // byte * the core's on-screen fraction (f20: a stack argument Ghidra's C drops), halved bilinearly to one pixel and
        // spread over the halo square (RGBAQ 0x808080FF: R 0xFF, A 0x80, so As = At; RGB masked). Then the halo texture over the core +- (+0x14 / w),
        // ALPHA 0x58 = Cs * Ad + Cd, no Z test; colour 0x808080, or the material's +8 when the node's flags & 0xE000 = 0x4000.
        // Natively the same draws as GS sprites; the averaging is the spread draw's read of the frame copy (mesh.frag bit 256).
        // shortcut: halos with lens-flare ghosts (more texture nodes; tables 0x63F430..) stay on the GS; none in the saves yet.
        // The game saves the frame's alpha (the glow mask) before the halos and restores it after; natively the restore reads
        // the copy the spread took. shortcut: so the core square keeps the test's alpha, not the glow mask's (16x16 px or so).
        struct HaloStats
        {
            uint64_t calls = 0, native = 0, ghosts = 0;
        } g_haloStats;
        bool haloRecord(uint8_t *rdram, uint32_t halo)
        {
            static const bool off = [] { const char *e = std::getenv("POP_NATIVE_HALOS"); return e && *e == '0'; }();
            ++g_haloStats.calls;
            // after the last list (g_currentList +0xA8): the frame must still be pure for the Z test to see only native depth
            if (off || (g_world && !(g_worldLate && worldStateOk(rdram))))
                return false;
            const uint32_t material = guestAny(rdram, halo + 0x1C);
            const uint32_t node = material ? guestAny(rdram, material + 0x10) : 0;
            const bool textured = node && guest16(rdram, node) != 0xFFFF;
            if (textured)
                if (const uint32_t next = guestAny(rdram, node + 0x20); next && guest16(rdram, next) != 0xFFFF)
                {
                    ++g_haloStats.ghosts;
                    return false;
                }
            const uint32_t gso = guestAny(rdram, 0x00675148u);
            const auto apply = [&](uint32_t m, const float v[4], float out[4]) { // sceVu0ApplyMatrix: out = sum_k row k * v[k]
                for (int c = 0; c < 4; ++c)
                {
                    out[c] = 0.0f;
                    for (int k = 0; k < 4; ++k)
                        out[c] += guestF(rdram, m + 16 * k + 4 * c) * v[k];
                }
            };
            float pos[4], clip[4], screen[4];
            for (int k = 0; k < 4; ++k)
                pos[k] = guestF(rdram, halo + 4 * k);
            apply(gso + 0x240u, pos, clip);
            const auto drawn = [&] { // nothing to draw: the GS would draw nothing either
                if (!(g_world && g_worldLate))
                    return false;
                ++g_haloStats.native;
                return true;
            };
            if (!(clip[2] > 0.1f))
                return drawn();
            const float inv = 1.0f / clip[3];
            const float ndc[4] = {clip[0] * inv, clip[1] * inv, clip[2] * inv, 1.0f};
            apply(gso + 0x280u, ndc, screen);
            const int w = int(guestAny(rdram, 0x0066D03Cu)), h = int(guestAny(rdram, 0x0066D040u));
            const float half = float(w >> 1) * 16.0f;
            const int x = int(screen[0] * 16.0f), y = int(screen[1] * 16.0f), z = int(screen[2]);
            const int core = std::max(int(guestF(rdram, halo + 0x10) * inv * half), 0x80), size = int(guestF(rdram, halo + 0x14) * inv * half);
            const int sx0 = (0x800 - (w >> 1)) * 16, sx1 = (0x800 + (w >> 1)) * 16, sy0 = (0x800 - (h >> 1)) * 16, sy1 = (0x800 + (h >> 1)) * 16;
            if (!(sx0 < x + core && x - core < sx1 && sy0 < y + core && y - core < sy1))
                return drawn();
            const int cx0 = std::clamp(x - core, sx0, sx1), cx1 = std::clamp(x + core, sx0, sx1);
            const int cy0 = std::clamp(y - core, sy0, sy1), cy1 = std::clamp(y + core, sy0, sy1);
            const float onScreen = float(cx1 - cx0) / float(2 * core) * float(cy1 - cy0) / float(2 * core);
            const uint32_t alpha = uint32_t(float(guestAny(rdram, halo + 0x18) >> 24) * onScreen) & 0xFF;
            const int px0 = cx0 & ~15, py0 = cy0 & ~15, px1 = cx1 & ~15, py1 = cy1 & ~15; // ProcessLensFlare: whole pixels
            const int ox0 = cx0 - size, oy0 = cy0 - size, ox1 = cx1 + size, oy1 = cy1 + size;
            // a GS sprite (RGBAQ, ST, XYZ2, ST, XYZ2) through gifToBatch, as the water's clear
            const auto sprite = [&](int x0, int y0, int x1, int y1, uint32_t rgba, pop_assets::Batch &b) {
                std::vector<uint8_t> pk(6 * 16, 0);
                const uint64_t tag[2] = {1u | (1u << 15) | (uint64_t(1) << 46) | (uint64_t(6) << 47) | (uint64_t(5) << 60), 0x52521};
                const uint32_t colour[4] = {rgba & 0xFF, rgba >> 8 & 0xFF, rgba >> 16 & 0xFF, rgba >> 24};
                const float st0[4] = {0, 0, 1, 0}, st1[4] = {1, 1, 1, 0};
                const uint32_t xy0[4] = {uint32_t(x0) & 0xFFFF, uint32_t(y0) & 0xFFFF, uint32_t(z), 0};
                const uint32_t xy1[4] = {uint32_t(x1) & 0xFFFF, uint32_t(y1) & 0xFFFF, uint32_t(z), 0};
                std::memcpy(pk.data(), tag, 16);
                std::memcpy(pk.data() + 16, colour, 16);
                std::memcpy(pk.data() + 32, st0, 16);
                std::memcpy(pk.data() + 48, xy0, 16);
                std::memcpy(pk.data() + 64, st1, 16);
                std::memcpy(pk.data() + 80, xy1, 16);
                uint64_t unused = 0;
                return gifToBatch(rdram, {pk}, b, unused) && !b.indices.empty();
            };
            pop_assets::Batch clear, test, spread, glow, restore;
            if (!sprite(px0, py0, px1, py1, 0, clear) || !sprite(px0, py0, px1, py1, alpha << 24, test) ||
                !sprite(ox0, oy0, ox1, oy1, 0x80000000u, spread) || !sprite(ox0, oy0, ox1, oy1, 0x80000000u, restore))
                return false;
            // the spread reads the core square of the frame copy: its corners in frame UV, the same on every vertex
            const float u0 = (px0 / 16.0f - float(0x800 - (w >> 1))) / float(w), v0 = (py0 / 16.0f - float(0x800 - (h >> 1))) / float(h);
            const float u1 = (px1 / 16.0f - float(0x800 - (w >> 1))) / float(w), v1 = (py1 / 16.0f - float(0x800 - (h >> 1))) / float(h);
            for (auto &v : spread.vertices)
                v.uv[0] = u0, v.uv[1] = v0, v.uv2[0] = u1, v.uv2[1] = v1;
            // the restore: a square of no size at each vertex's own frame UV (gifToBatch's corners: x0 x1 x1 x0, y0 y0 y1 y1)
            for (size_t k = 0; k < restore.vertices.size(); ++k)
            {
                auto &v = restore.vertices[k];
                const int cx = (k & 3) == 1 || (k & 3) == 2 ? ox1 : ox0, cy = (k & 3) >= 2 ? oy1 : oy0;
                v.uv[0] = v.uv2[0] = (cx / 16.0f - float(0x800 - (w >> 1))) / float(w);
                v.uv[1] = v.uv2[1] = (cy / 16.0f - float(0x800 - (h >> 1))) / float(h);
            }
            uint32_t colour = 0x80808080u, key = 0;
            if (textured)
            {
                if ((guestAny(rdram, node + 4) & 0xE000) == 0x4000)
                    colour = (guestAny(rdram, material + 8) & 0xFFFFFF) | 0x80000000u;
                const uint32_t index = guest16(rdram, node);
                if ((key = textureKey(rdram, index)))
                    key = recolouredKey(rdram, index, key);
                if (!key)
                    return false; // not in the texture list: untextured would be a flat square where the GS draws the texture
            }
            if (!sprite(ox0, oy0, ox1, oy1, colour, glow))
                return false;
            const std::array<float, 16> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::vector<WorldRecord> recs;
            // (geometry, texture key, render state flags: 0x200 = no depth write, blend kind 6 + 0x1000 = Cs * Ad + Cd; Z test; FBMSK)
            const auto add = [&](pop_assets::Batch &&g, uint32_t k, uint32_t flags, uint32_t zTest, uint32_t fbmsk) {
                DrawMaterial m; // no fog
                m.frameOnly = true;
                m.seq = g_frameSeq++;
                m.textureKey = k;
                m.flags = flags;
                m.zTest = zTest;
                m.fbmsk = fbmsk;
                m.list = g_currentList;
                std::vector<pop_assets::Batch> bs;
                bs.push_back(std::move(g));
                g_frameRefs.emplace_back(std::move(bs), std::make_pair(identity, m));
                recs.push_back({2, g_frameRefs.size() - 1});
            };
            add(std::move(clear), 0, 0x200, 0, 0x00FFFFFFu);              // alpha only, no Z test
            add(std::move(test), 0, 0x200, 1, 0x00FFFFFFu);               // alpha only, ZGEQUAL at the light's Z
            add(std::move(spread), kHaloAlphaKey, 0x200, 0, 0x00FFFFFFu); // alpha only: the core's mean over the halo square
            add(std::move(glow), key, 0x61200, 0, 0xFF000000u);           // RGB only, Cs * Ad + Cd
            const WorldRecord glowRec = recs.back();
            add(std::move(restore), kHaloRestoreKey, 0x200, 0, 0x00FFFFFFu); // alpha only: the glow mask back
            if (!(g_world && g_worldLate))
                return false; // offline: the draws stay in the frame, the GS emulation is not used
            if (key && !worldFinish({glowRec}, {}))
                return false;
            for (const WorldRecord &r : recs) // the others use no texture (key 0 or the frame copy)
                worldMaterial(r).native = true;
            ++g_haloStats.native;
            return true;
        }
        PS2Runtime::RecompiledFunction g_origHalo = nullptr;
        void haloDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing && haloRecord(rdram, getRegU32(ctx, 5)))
                return;
            g_origHalo(rdram, ctx, runtime);
        }
#endif

        // native shadows (below): which records each Display call added, so a shadow list entry finds its geometry
        void noteRecords(uint64_t key, size_t n0, size_t n1, size_t n2);
        // CJIdxMesh::Display(mesh, matrix index) has no pass argument; bit 60 keeps its keys apart from the others
        inline uint64_t idxCallKey(uint32_t mesh, uint32_t index)
        {
            return (uint64_t(1) << 60) | (uint64_t(mesh) << 16) | (index & 0xFFFF);
        }

        template <int N>
        void otherDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_frameCapturing)
            {
                const uint32_t sub = guestAny(rdram, 0x70000034u);
                g_frameOther.push_back({N, sub ? textureKey(rdram, guestAny(rdram, sub) & 0xFFFF) : 0u, sub ? guestAny(rdram, sub + 4) : 0u,
                                        getRegU32(ctx, 4), g_currentList});
#if POP_SPRITE_VU1
                if ((N == 1 && spriteRecord(rdram, ctx)) || (N == 2 && waterRecord(rdram, ctx)))
                {
                    if (g_currentList == 0x88u || g_currentList == 0xA8u)
                        ++g_lateStats.native;
                    return;
                }
#endif
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
                    {
                        g_frameRefs.emplace_back(std::move(batches), std::make_pair(model, currentMaterial(rdram)));
                        noteRecords(idxCallKey(mesh, index), g_frameDraws.size(), g_framePosed.size(), g_frameRefs.size() - 1);
                        // POP_NATIVE_WORLD: one mesh, no chain
                        std::vector<WorldRecord> recs{{2, g_frameRefs.size() - 1}};
                        if (g_world && worldStateOk(rdram) && worldFinish(recs, {}))
                        {
                            if (g_currentList == 0x88u || g_currentList == 0xA8u)
                                ++g_lateStats.native;
                            return;
                        }
                    }
                }
            }
            static const char *const kPath[4] = {"idx", "sprite", "water", "list18"};
            worldOnGs(rdram, kPath[N]);
            g_other[N].original(rdram, ctx, runtime);
        }

        void readElement(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t element = getRegU32(ctx, 4);
            const uint32_t data = guest32(rdram, getRegU32(ctx, 5)) & (kRam - 1);
            ++g_stats.read;
            // a new element at this address (a streamed-in section reusing an unloaded one's memory): forget what
            // was kept for the old one (N8/N9/N11: its lightmap, lit pass or skinned status drawn on the new one)
            g_skipped.erase(element);
            g_skinned.erase(element);
            for (uint64_t pass = 0; pass < 8; ++pass)
            {
                g_fromPacket.erase((uint64_t(element) << 8) | pass);
                g_litBatches.erase((uint64_t(element) << 8) | pass);
            }
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
                c.serial = ++g_elementSerial;
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

        // Flash detector (native characters, 22 Oct: the user saw the Prince's whole body light up for a
        // fraction of a second, too short to quick-save by hand): a posed draw whose mean lit colour jumps
        // by more than 35% from the frame before is logged with both frames' light sets (set index,
        // point-light flag, light count, ambient, constant colour, light vectors and colours). On with
        // POP_NATIVE_FLASHLOG=1 (after the 22 Oct fix it mostly logs real light changes);
        // POP_NATIVE_FLASH_SAVE=1 (implies it) also quick-saves then (at most 5, 10 s apart).
        struct FlashSeen
        {
            uint32_t frame = 0;
            float level = 0;
            std::string light; // the light set as logged
        };
        std::unordered_map<uint64_t, FlashSeen> g_flashSeen; // (element << 16 | matrix index) -> last frame's level
        std::string describeLight(uint8_t *rdram, const LightEnv &light)
        {
            const uint32_t index = guestAny(rdram, 0x70000088u) & 0xFF, table = guestAny(rdram, 0x700004F0u);
            const uint32_t set = table + index * 0xF0u;
            char b[512];
            int n = std::snprintf(b, sizeof b, "set %u pt %u n %u amb %08x const %u %08x | used amb %.0f %.0f %.0f", index, guest8(rdram, set + 1),
                                  guest8(rdram, set + 8), guestAny(rdram, set + 4), guest8(rdram, 0x70000039u), guestAny(rdram, 0x7000003Cu),
                                  double(light.amb[0]), double(light.amb[1]), double(light.amb[2]));
            for (int l = 0; l < light.count && n < int(sizeof b) - 80; ++l)
                n += std::snprintf(b + n, sizeof b - n, "; light (%.2f %.2f %.2f) colour %.0f %.0f %.0f", double(light.dir[l][0]),
                                   double(light.dir[l][1]), double(light.dir[l][2]), double(light.col[l][0]), double(light.col[l][1]),
                                   double(light.col[l][2]));
            return b;
        }
        void flashCheck(uint8_t *rdram, uint32_t element, uint32_t index, const std::vector<pop_assets::Batch> &posed, const LightEnv &light)
        {
            double sum = 0;
            size_t nv = 0;
            for (const auto &b : posed)
                for (const auto &v : b.vertices)
                {
                    sum += v.colour[0] + v.colour[1] + v.colour[2];
                    ++nv;
                }
            if (!nv)
                return;
            const float level = float(sum / (3.0 * nv));
            FlashSeen &seen = g_flashSeen[(uint64_t(element) << 16) | index];
            const bool consecutive = seen.frame + 1 == g_worldSeq;
            if (consecutive && seen.level > 0.05f && level > seen.level * 1.35f)
            {
                static int logged = 0;
                if (logged++ < 200)
                    std::fprintf(stderr, "[pop:flash] frame %u element %08x idx %u: mean colour %.3f -> %.3f\n[pop:flash]   before: %s\n[pop:flash]   now:    %s\n",
                                 g_worldSeq, element, index, double(seen.level), double(level), seen.light.c_str(), describeLight(rdram, light).c_str());
#if POP_LIVE_QUICKSAVE
                static const bool save = [] {
                    const char *e = std::getenv("POP_NATIVE_FLASH_SAVE");
                    return e && *e == '1';
                }();
                static int saves = 0;
                static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(60);
                const auto now = std::chrono::steady_clock::now();
                if (save && saves < 5 && now - last > std::chrono::seconds(10))
                {
                    ++saves;
                    last = now;
                    ps2x::savestate::requestQuickSave();
                    std::fprintf(stderr, "[pop:flash]   quick-save requested\n");
                }
#endif
            }
            if (seen.frame != g_worldSeq) // the first draw of the element in a frame
            {
                seen.frame = g_worldSeq;
                seen.level = level;
                seen.light = describeLight(rdram, light);
            }
        }

        // POP_NATIVE_ELEMLOG=<hex>,<hex>... (debug): what the Display hook records for these elements: path, pass,
        // list, matrix translation, batches and the posed bounding box, texture, flags
        bool elemLogged(uint32_t element)
        {
            static const std::vector<uint32_t> wanted = [] {
                std::vector<uint32_t> v;
                if (const char *e = std::getenv("POP_NATIVE_ELEMLOG"))
                    for (const char *c = e; *c;)
                    {
                        char *end = nullptr;
                        v.push_back(uint32_t(std::strtoul(c, &end, 16)));
                        c = *end ? end + 1 : end;
                    }
                return v;
            }();
            static const bool all = std::getenv("POP_NATIVE_ELEMLOG") && !std::strcmp(std::getenv("POP_NATIVE_ELEMLOG"), "all");
            return all || (!wanted.empty() && std::find(wanted.begin(), wanted.end(), element) != wanted.end());
        }
        void elemLog(const char *path, uint32_t element, uint32_t pass, uint32_t index, const std::array<float, 16> &model,
                     const std::vector<pop_assets::Batch> *batches, const DrawMaterial &dm)
        {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            size_t nv = 0;
            if (batches)
                for (const auto &b : *batches)
                    for (const auto &v : b.vertices)
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            const float w = v.pos[0] * model[c] + v.pos[1] * model[4 + c] + v.pos[2] * model[8 + c] + model[12 + c];
                            lo[c] = std::min(lo[c], w);
                            hi[c] = std::max(hi[c], w);
                        }
                        ++nv;
                    }
            std::fprintf(stderr, "[pop:elem] %s el %08x pass %u list +0x%x matrix %u at (%.2f %.2f %.2f) batches %zu vertices %zu box (%.2f %.2f %.2f)-(%.2f %.2f %.2f) "
                                 "tex %08x flags %08x ref %08x const %d/%d fbmsk %08x\n",
                         path, element, pass, g_currentList, index, double(model[12]), double(model[13]), double(model[14]), batches ? batches->size() : 0, nv,
                         double(lo[0]), double(lo[1]), double(lo[2]), double(hi[0]), double(hi[1]), double(hi[2]), dm.textureKey, dm.flags, dm.ref,
                         dm.constColour, dm.constAlpha, dm.fbmsk);
        }

        // CJMeshElement passes 1 and 4 (4 sends pass 1's packet) use the lit driver: the V4-8 (or V3-8) upload
        // at VU 0x83 holds normals, not colours (V4-8: a spare fourth byte, 0/1). The static converters read it as
        // colours (the Prince's sword: alpha 1/128, alpha-tested away; 22 Oct). Census of 196 saves
        // (tools/staticcensus.py): every static pass-0 packet has colours there, every pass-1 packet normals. Until
        // lit statics are drawn natively (renderer step 2, with the character lighting), a palette-less element
        // drawn with these passes stays on the GS unless the character path posed it (g_skinned, lit: rigid parts
        // with a skinned packet).
        bool isLitStaticPass(uint32_t pass) { return pass == 1 || pass == 4; }

        void displayRecord(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pass = getRegU32(ctx, 6) & 0xFF, index = getRegU32(ctx, 5) & 0xFFFF;
            uint32_t element = getRegU32(ctx, 4);
            // POP_NATIVE_WORLD: the call is skipped if every element of the chain is static and was
            // recorded for the native frame now (or earlier in the frame and drawn natively then);
            // a lightmap pass (CLMElementList::Render) only over natively drawn elements
            bool worldOk = g_world && g_frameCapturing && (pass == 3 ? g_worldOpen && g_currentList == 0x5Cu : worldStateOk(rdram));
            std::vector<WorldRecord> worldRecs;
            std::vector<uint64_t> worldBase;
            std::vector<uint32_t> worldPalettes; // the chain's bone palettes in order (driver switches to replay)
            const bool worldState = worldOk;
            bool skinnedChain = false;
            for (unsigned link = 0; element && link < 4096; ++link)
            {
                // Display draws nothing for an element without a packet for the pass, and does not follow
                // its chain either (the recursion is inside that test; pass 4 uses pass 1's packet)
                if (!guest32(rdram, element + 0x20 + 4 * (pass == 4 ? 1 : pass)))
                    break;
                ++g_stats.draws;
                const uint64_t worldKey = (uint64_t(element) << 16) | index | (pass == 2 ? uint64_t(2) << 56 : 0) |
                                          (pass == 3 ? uint64_t(3) << 56 : alphaPassBit(rdram));
                const bool worldSeen = worldOk && g_frameSeen.count(worldKey);
                const size_t world0 = worldOk ? worldCount0() : 0, world1 = worldOk ? worldCount1() : 0, world2 = worldOk ? worldCount2() : 0;
                if (g_frameCapturing && !g_elements.count(element) && !g_skipped.count(element) && !g_skinned.count(element))
                {
                    // Skinned, never seen at load (a save-state scene): the drawn pass's packet in memory
                    // (+0x20 + 4 * pass, pass 4 -> 1) holds the bind-space batches inline (cnt tags up to the
                    // RET), as in the file. Characters often have no pass-0 packet, only pass 1.
                    const uint32_t palette = guest32(rdram, element + 0x48);
                    const uint32_t packet = (pass == 0 || pass == 1 || pass == 4)
                                                ? guest32(rdram, element + 0x20 + 4 * (pass == 4 ? 1 : pass)) & 0x01FFFFF0u
                                                : 0u;
                    // a rigid part (no palette: the sword on the Prince's back, 22 Oct) can carry a skinned
                    // packet too (inline cnt tags, first tag id 1/6/7 instead of a static packet's refs)
                    const uint32_t firstTag = packet ? (guest32(rdram, packet) >> 28) & 7 : 0;
                    const bool rigidSkinned = !palette && packet && (firstTag == 1 || firstTag == 6 || firstTag == 7);
                    if (((palette && guest8(rdram, palette)) || rigidSkinned) && packet)
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
                        if (ok && !skinned.empty())
                            g_skinned[element] = std::move(skinned);
                        else if (!rigidSkinned)
                            g_skipped[element] = 's';
                    }
                }
                ++g_passDraws[pass & 7][g_skinned.count(element) ? 1 : 0];
                skinnedChain = skinnedChain || guest32(rdram, element + 0x48);
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
                                         "[pop:skin] el %08x idx %u pass %u batches %zu normals %zu | set %u amb %08x n %u pt %u | const %u %08x la %u %02x | "
                                         "lit %d amb %.0f %.0f %.0f lights %d | mean colour %.3f %.3f %.3f\n",
                                         element, index, pass, sk->second.size(), withNormals, guestAny(rdram, 0x70000088u) & 0xFF,
                                         guestAny(rdram, set + 4), guest8(rdram, set + 8), guest8(rdram, set + 1), guest8(rdram, 0x70000039u),
                                         guestAny(rdram, 0x7000003Cu), guest8(rdram, 0x7000003Au), guest8(rdram, 0x70000040u), int(light.valid), double(light.amb[0]), double(light.amb[1]),
                                         double(light.amb[2]), light.count, nv ? mean[0] / nv : 0, nv ? mean[1] / nv : 0, nv ? mean[2] / nv : 0);
                            static int setDumps = 0;
                            if (setDumps++ < 3)
                                for (int l = 0; l < 4; ++l)
                                    std::fprintf(stderr, "[pop:skin]   light %d type %08x colour %08x dir/pos %.3f %.3f %.3f %.3f\n", l,
                                                 guestAny(rdram, set + 0xE0 + 4 * l), guestAny(rdram, set + 0xD0 + 4 * l), double(guestF(rdram, set + 0x10 + 16 * l)),
                                                 double(guestF(rdram, set + 0x14 + 16 * l)), double(guestF(rdram, set + 0x18 + 16 * l)),
                                                 double(guestF(rdram, set + 0x1C + 16 * l)));
                        }
                        static const bool flashLog = std::getenv("POP_NATIVE_FLASHLOG") || std::getenv("POP_NATIVE_FLASH_SAVE");
                        if (g_world && g_worldCharacters && flashLog)
                            flashCheck(rdram, element, index, posed, light);
                        DrawMaterial dm = currentMaterial(rdram);
                        dm.hasUvm = false; // the skin driver has no UV matrix
                        if (elemLogged(element))
                        {
                            std::array<float, 16> identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                            elemLog("posed", element, pass, index, identity, &posed, dm);
                        }
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
                    const uint32_t key = textureKey(rdram, guestAny(rdram, 0x70000008u) & 0xFFFFu);
                    g_frameLightmap[(uint64_t(element) << 16) | index] = key;
                    const uint32_t packet = guest32(rdram, element + 0x2C) & ~0xFu;
                    auto &batches = g_fromPacket[(uint64_t(element) << 8) | 3];
                    if (batches.empty() && packet)
                        livePacketBatches(rdram, packet, 32768.0, batches, true); // positions as the base pass's
                    if (!batches.empty() && key && g_frameSeen.insert((uint64_t(element) << 16) | index | (uint64_t(3) << 56)).second)
                    {
                        DrawMaterial dm;
                        dm.seq = g_frameSeq++;
                        dm.textureKey = key;
                        dm.flags = guestAny(rdram, 0x70000000u);
                        dm.fbmsk = 0xFF000000u; // flags 0x284: colour written, alpha kept
                        // CLMElementList::Render selects fog zone (entry bits 14-17) + 0x10, whose constants are the
                        // base zone's x 127/255; P1 (mode 3) writes that F as the vertex alpha: the lightmap fades with distance
                        zoneFog(rdram, dm);
                        dm.list = g_currentList;
                        const uint32_t m = guestAny(rdram, 0x70000260u) + index * 0x40u;
                        std::array<float, 16> model;
                        for (int k = 0; k < 16; ++k)
                            model[k] = guestF(rdram, m + 4u * k);
                        g_frameRefs.emplace_back(batches, std::make_pair(model, dm));
                        ++g_stats.drawsLightmap;
                    }
                }
                else if (sk == g_skinned.end() && !guest32(rdram, element + 0x48) && litDriverPass(rdram, element, pass))
                {
                    // a lit static pass (litDriverPass): litPose, drawn with the object's model matrix like the base pass
                    ++g_stats.drawsKnown;
                    if (g_frameCapturing && g_frameSeen.insert(worldKey).second)
                    {
                        auto &bind = g_litBatches[(uint64_t(element) << 8) | pass];
                        if (bind.empty())
                            if (const uint32_t packet = guest32(rdram, element + 0x20 + 4 * pass) & 0x01FFFFF0u)
                                litPacketBatches(rdram, packet, pass == 2, bind);
                        std::vector<pop_assets::Batch> posed;
                        litPose(rdram, element, index, pass, bind, posed);
                        if (!posed.empty())
                        {
                            DrawMaterial dm = currentMaterial(rdram);
                            dm.hasUvm = false; // the character program has no UV matrix
                            const std::array<float, 16> model = poolMatrix(rdram, index);
                            if (elemLogged(element))
                                elemLog(pass == 1 ? "lit" : "env", element, pass, index, model, &posed, dm);
                            g_frameRefs.emplace_back(std::move(posed), std::make_pair(model, dm));
                            ++(pass == 1 ? g_worldStats.litNative : g_worldStats.envNative);
                        }
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
                    if (elemLogged(element))
                        std::fprintf(stderr, "[pop:elem] unknown el %08x pass %u: skipped as '%c', palette %08x, capturing %d\n", element, pass,
                                     sk == g_skipped.end() ? '-' : sk->second, palette, int(g_frameCapturing));
                    if (sk == g_skipped.end() && !(palette && guest8(rdram, palette)) && g_frameCapturing && pass == 0 &&
                        g_frameSeen.insert((uint64_t(element) << 16) | index | alphaPassBit(rdram)).second)
                    {
                        const uint32_t packet = guest32(rdram, element + 0x20 + 4 * (pass == 4 ? 1 : pass)) & ~0xFu;
                        auto &batches = g_fromPacket[(uint64_t(element) << 8) | pass];
                        if (batches.empty() && packet)
                            livePacketBatches(rdram, packet, 512.0, batches, true); // pass 0 (lit passes stay on the GS)
                        if (!batches.empty())
                        {
                            const uint32_t m = guestAny(rdram, 0x70000260u) + index * 0x40u;
                            std::array<float, 16> model;
                            for (int k = 0; k < 16; ++k)
                                model[k] = guestF(rdram, m + 4u * k);
                            g_frameRefs.emplace_back(batches, std::make_pair(model, currentMaterial(rdram)));
                            ++g_stats.drawsFromPacket;
                            if (elemLogged(element))
                                elemLog("from packet", element, pass, index, model, &batches, g_frameRefs.back().second.second);
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
                else if (isLitStaticPass(pass))
                {
                    ++g_stats.drawsKnown;
                    if (g_frameCapturing)
                        ++g_worldStats.litStatic; // left to the GS: not recorded, so worldOk drops below
                }
                else if (pass == 0)
                {
                    ++g_stats.drawsKnown;
                    const uint32_t packet = guest32(rdram, element + 0x20 + 4 * (pass == 4 ? 1 : pass));
                    std::vector<float> pos;
                    if (!g_world) // the conversion check (diagnostic; costs game-thread time per draw)
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
                        if (elemLogged(element))
                            elemLog("converted", element, pass, index, model, nullptr, g_frameMaterials.back());
                    }
                }
                if (worldOk)
                {
                    bool recorded = false;
                    if (worldCount0() > world0)
                        worldRecs.push_back({0, world0}), recorded = true;
                    if (worldCount1() > world1)
                    {
                        worldRecs.push_back({1, world1}), recorded = true;
                        worldOk = worldOk && g_worldCharacters; // posed (also rigid skinned parts without a palette)
                    }
                    if (worldCount2() > world2)
                        worldRecs.push_back({2, world2}), recorded = true;
                    const uint64_t base = (uint64_t(element) << 16) | index;
                    if (const uint32_t palette = guest32(rdram, element + 0x48))
                    {
                        if (g_worldCharacters)
                            worldPalettes.push_back(palette); // drawn natively, its driver switches replayed
                        else
                            worldOk = false; // characters stay on the GS
                    }
                    if (pass == 3 && !g_worldBase.count(base))
                        worldOk = false; // its base was drawn by the GS
                    else if (!recorded && !(worldSeen && (pass == 3 || g_worldBase.count(base))))
                    {
                        worldOk = false; // nothing decoded for it
                        ++g_worldStats.notRecorded;
                        static const bool whyLog = std::getenv("POP_NATIVE_WORLD_WHY") != nullptr;
                        if (whyLog)
                        {
                            const uint32_t packet = pass < 8 ? guest32(rdram, element + 0x20 + 4 * (pass == 4 ? 1 : pass)) & 0x01FFFFF0u : 0u;
                            const auto sk = g_skipped.find(element);
                            std::fprintf(stderr, "[pop:world] not decoded: element %08x pass %u list +0x%x palette %08x packet %08x first tag %u "
                                                 "known %d skipped '%c' skinned %d\n",
                                         element, pass, g_currentList, guest32(rdram, element + 0x48), packet,
                                         packet ? (guest32(rdram, packet) >> 28) & 7 : 0u, int(it != g_elements.end()),
                                         sk == g_skipped.end() ? '-' : sk->second, int(g_skinned.count(element)));
                        }
                    }
                    worldBase.push_back(base);
                }
                element = guest32(rdram, element + 0x4C);
            }
            report();
            if (worldOk && worldFinish(worldRecs, pass == 3 ? std::vector<uint64_t>{} : worldBase))
            {
                if (pass == 3)
                    ++g_worldStats.lightmaps;
                if (g_currentList == 0x88u || g_currentList == 0xA8u)
                    ++g_lateStats.native;
                for (const uint32_t palette : worldPalettes)
                    worldReplayDriver(rdram, palette);
                return; // like a stub: leave pc, the dispatcher returns to the caller
            }
            if (g_world && g_worldCharacters && g_frameCapturing && skinnedChain && pass != 3)
            {
                const int why = !worldState ? 0 : !worldOk ? 1 : 2;
                ++g_worldStats.skinGs[why];
                static const bool whyLog = std::getenv("POP_NATIVE_WORLD_WHY") != nullptr; // debug: each skinned call left to the GS
                if (whyLog)
                {
                    const uint32_t sub = guestAny(rdram, 0x70000034u);
                    std::fprintf(stderr, "[pop:world] skinned call on the GS (%s): element %08x pass %u list +0x%x flags %08x Z test %u mask %08x\n",
                                 why == 0 ? "render state" : why == 1 ? "not decoded" : "texture", getRegU32(ctx, 4), pass, g_currentList,
                                 sub ? guestAny(rdram, sub + 4) : 0u, guest8(rdram, 0x70000038u), guestAny(rdram, 0x70000044u));
                }
            }
            worldOnGs(rdram, pass == 3 ? "lightmap" : "mesh");
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
        std::unordered_set<uint32_t> g_worldSent;                      // POP_NATIVE_WORLD: keys pop_native_world has
        std::unordered_set<uint32_t> g_worldMissing;                   // keys not decodable yet (retried each frame)
        // A save-state load looks nothing up in the bigfile, so no world pack loads (onBigfileLookup): a texture
        // missing from the loaded packs loads the pack tools/packindex.py's index names for it (as pop_state_frame
        // does; POP_PACKINDEX, default game/analysis/packindex.txt from the disc folder). Loads in the background:
        // the draws wait on the GS until it is in.
        void loadPackFor(uint32_t key)
        {
            static const std::unordered_map<uint32_t, uint32_t> packOf = [] {
                std::unordered_map<uint32_t, uint32_t> m;
                const char *env = std::getenv("POP_PACKINDEX");
                if (FILE *f = std::fopen(env && *env ? env : "../../analysis/packindex.txt", "r"))
                {
                    unsigned tex, pack;
                    while (std::fscanf(f, "%x %x", &tex, &pack) == 2)
                        m.emplace(tex, pack); // the first pack listed for a texture
                    std::fclose(f);
                }
                std::fprintf(stderr, "[pop:native] pack index: %zu texture keys\n", m.size());
                return m;
            }();
            const auto it = packOf.find(key);
            if (it != packOf.end())
                onBigfileLookup(it->second);
        }

        bool worldTextureReady(uint32_t key)
        {
            if (g_liveImages.count(key))
                return true;
            if (g_worldMissing.count(key))
                return false; // tried this frame
            pop_native::Image img;
            if (!findTexture(key, img))
            {
                g_worldMissing.insert(key);
                loadPackFor(key);
                return false;
            }
            g_liveImages.emplace(key, std::move(img));
            return true;
        }
        std::deque<std::pair<std::vector<uint8_t>, int>> g_liveDelayed; // frames waiting for GS pictures (count so far)
        uint64_t g_liveConnection = 0;

        uint64_t vertexVariant(const DrawMaterial &m)
        {
            if (m.constColour < 0 && m.constAlpha < 0 && !m.hasUvm)
                return 0;
            uint64_t h = mix64(uint64_t(uint32_t(m.constColour)), uint64_t(uint32_t(m.constAlpha)));
            if (m.hasUvm)
                for (float u : m.uvm)
                {
                    uint32_t bits;
                    std::memcpy(&bits, &u, 4);
                    h = mix64(h, bits);
                }
            return h | 1;
        }

        // --- Native shadows (POP_NATIVE_SHADOWS=1; docs/native_renderer.md "Native shadows") ---
        // CJadeShadowManager::DisplayAllShadows (0x4C02C0) runs after the lightmaps; per projector FUN_004C2400 builds
        // its matrices and calls FUN_004C3540 (render-to-texture path, a0 = projector): the casters (list +0, count
        // +4) drawn untextured into the alpha of a 256x256 corner of the displayed buffer (FUN_004C48C0: FBMSK
        // 0x00FFFFFF; cleared to 0, scissor 1..size-2, XYOFFSET 2048 - size/2, no blend: alpha = strength x 128),
        // then the receivers (list +0x10) redrawn into the frame by VU1 program 6 entry 0x2058 with that alpha as
        // texture (CLAMP, bilinear; ALPHA 0x46 = Cd (1 - As), Z test GEQUAL, no Z write, no fog). The receiver loop
        // (0x1D98, checked against the interpreter on 32,940 vertices of save 192521): STQ = T p / T.w with T =
        // projector +0x2B0 (row vectors, world space); A = min(qw22.z + qw22.w * w_cam, 255) * fade, fade = clamp(
        // qw23.z + qw23.w * sum(dir_i * d_i^2), 0, 1), d = the projector position (+0x360) minus the vertex in object
        // space, qw23 = +0x3A0, dir = (0,0,1) (0x63FD30; 0x63FD20 = (1,1,1) with +0x104), qw22 = scratchpad 0x460;
        // triangles wholly outside one plane of the projector frustum (+0x270) dropped. The geometry is the frame's
        // own records of the same (element, matrix index) from the lists drawn before (displayRecord above).
        struct RecordRange
        {
            int kind; // 0 static (g_frameDraws), 1 posed (g_framePosed), 2 instanced (g_frameRefs)
            size_t first, count;
        };
        // (element or ref << 16 | matrix index | pass << 56) -> the records of its first Display call in the frame (later
        // calls of the same draw, e.g. the alpha pass +0x98, would darken twice)
        std::unordered_map<uint64_t, std::vector<RecordRange>> g_frameByCall;
        uint64_t callKey(R5900Context *ctx)
        {
            return (uint64_t(getRegU32(ctx, 4)) << 16) | (getRegU32(ctx, 5) & 0xFFFF) | (uint64_t(getRegU32(ctx, 6) & 0xF) << 56);
        }
        void noteRecords(uint64_t key, size_t n0, size_t n1, size_t n2)
        {
            if (!g_frameCapturing || g_frameByCall.count(key))
                return;
            auto &v = g_frameByCall[key];
            if (g_frameDraws.size() > n0)
                v.push_back({0, n0, g_frameDraws.size() - n0});
            if (g_framePosed.size() > n1)
                v.push_back({1, n1, g_framePosed.size() - n1});
            if (g_frameRefs.size() > n2)
                v.push_back({2, n2, g_frameRefs.size() - n2});
        }
        // the hooks: the recording calls above, plus which records each call added (records are made before the
        // original runs, so the counts after the call are right even if the scheduler unwinds inside it)
        void display(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t key = callKey(ctx);
            const size_t n0 = g_frameDraws.size(), n1 = g_framePosed.size(), n2 = g_frameRefs.size();
            displayRecord(rdram, ctx, runtime);
            noteRecords(key, n0, n1, n2);
        }
        void refDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t key = callKey(ctx);
            const size_t n0 = g_frameDraws.size(), n1 = g_framePosed.size(), n2 = g_frameRefs.size();
            refDisplayRecord(rdram, ctx, runtime);
            noteRecords(key, n0, n1, n2);
        }

        constexpr uint32_t kShadowRtt = 0x004C3540u;
        PS2Runtime::RecompiledFunction g_origShadowRtt = nullptr;
        const bool g_nativeShadows = [] {
            const char *e = std::getenv("POP_NATIVE_SHADOWS");
            return e && *e == '1';
        }();
        // the frame's shadow textures (synthetic texture keys kShadowKey + n) and receiver draws (object space, drawn
        // with the base draw's model matrix: the same transform gives them the base's exact depth for GEQUAL)
        constexpr uint32_t kShadowKey = 0xFE5D0000u;
        std::vector<pop_native::Image> g_frameShadowImages;
        std::vector<std::pair<std::vector<pop_assets::Batch>, std::pair<std::array<float, 16>, DrawMaterial>>> g_frameShadows;
        struct ShadowStats
        {
            uint64_t projectors = 0, nativeProjectors = 0, mirror = 0, casters = 0, casterTris = 0, receivers = 0, receiverTris = 0, culled = 0, noGeometry = 0;
            // why: entries without records (CJIdxMesh, mesh/ref, other kinds), receivers whose base went to the GS
            uint64_t missIdx = 0, missMesh = 0, missOther = 0, missEmpty = 0, baseOnGs = 0;
            uint64_t drawsNothing = 0; // entries without a packet for their pass (the game draws nothing either)
        } g_shadowStats;

        // world position of a record's vertex (row vectors: model is column-major object -> world)
        inline void toWorld(const float *model, const float *p, float out[3])
        {
            for (int c = 0; c < 3; ++c)
                out[c] = p[0] * model[c] + p[1] * model[4 + c] + p[2] * model[8 + c] + model[12 + c];
        }
        // [x y z 1] * M for a game matrix (4 rows of 4 floats in guest memory)
        struct GameMatrix
        {
            float m[16];
            void read(uint8_t *rdram, uint32_t a)
            {
                for (int k = 0; k < 16; ++k)
                    m[k] = guestF(rdram, a + 4u * k);
            }
            void apply(const float p[3], float out[4]) const
            {
                for (int c = 0; c < 4; ++c)
                    out[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
            }
        };

        // the GS rasterises a triangle over the pixels whose integer coordinates it covers (top-left rule)
        void rasterAlpha(pop_native::Image &img, const float a[2], const float b[2], const float c[2], uint8_t alpha, int lo, int hiX, int hiY)
        {
            const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if (area == 0)
                return;
            const float *p0 = a, *p1 = area > 0 ? b : c, *p2 = area > 0 ? c : b;
            const int x0 = std::max(lo, int(std::ceil(std::min({a[0], b[0], c[0]})))), x1 = std::min(hiX, int(std::floor(std::max({a[0], b[0], c[0]}))));
            const int y0 = std::max(lo, int(std::ceil(std::min({a[1], b[1], c[1]})))), y1 = std::min(hiY, int(std::floor(std::max({a[1], b[1], c[1]}))));
            const auto edge = [](const float *u, const float *v, float x, float y) { return (v[0] - u[0]) * (y - u[1]) - (v[1] - u[1]) * (x - u[0]); };
            const auto topLeft = [](const float *u, const float *v) { return (u[1] == v[1] && v[0] < u[0]) || v[1] < u[1]; };
            const bool t0 = topLeft(p1, p2), t1 = topLeft(p2, p0), t2 = topLeft(p0, p1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const float e0 = edge(p1, p2, float(x), float(y)), e1 = edge(p2, p0, float(x), float(y)), e2 = edge(p0, p1, float(x), float(y));
                    if ((e0 > 0 || (e0 == 0 && t0)) && (e1 > 0 || (e1 == 0 && t1)) && (e2 > 0 || (e2 == 0 && t2)))
                        img.rgba[(size_t(y) * img.width + size_t(x)) * 4 + 3] = alpha;
                }
        }

        // a list entry's geometry from the frame's records: (batches, model) pairs; false when the frame has none
        struct RecordPart
        {
            const std::vector<pop_assets::Batch> *batches;
            const float *model;
            bool native; // POP_NATIVE_WORLD: the base record is drawn natively
        };
        using RecordGeometry = std::vector<RecordPart>;
        bool entryGeometry(uint8_t *rdram, uint32_t entry, RecordGeometry &out)
        {
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            uint64_t word;
            std::memcpy(&word, getConstMemPtr(rdram, entry), 8);
            const uint32_t kind = (guest8(rdram, entry + 6) & 0x3F) >> 2, index = uint32_t((word << 26) >> 54);
            uint64_t key;
            if (kind == 4) // FUN_004B80C0: CJIdxMesh::Display(*(0x6DF870) + low 16 bits x 4, index)
                key = idxCallKey(guestAny(rdram, 0x006DF870u) + uint32_t(word & 0xFFFF) * 4u, index);
            else if (kind == 9 || kind == 10)
            {
                const uint32_t obj = uint32_t(word) << 4, pass = guest8(rdram, entry + 0xD) & 0xF;
                // Display draws nothing without a packet for the pass (element +0x20 + 4 x pass, pass 4 -> 1; ref
                // + 4 x pass, pass 4 -> 0), in the shadow pass as anywhere: nothing to record, nothing missing
                const uint32_t packet = kind == 9 ? guestAny(rdram, obj + 0x20 + 4 * (pass == 4 ? 1 : pass)) : guestAny(rdram, obj + 4 * (pass == 4 ? 0 : pass));
                if (!(packet & ~0xFu))
                {
                    ++g_shadowStats.drawsNothing;
                    return true;
                }
                key = (uint64_t(obj) << 16) | index | (uint64_t(pass) << 56);
            }
            else
            {
                ++g_shadowStats.missOther;
                return false;
            }
            const auto it = g_frameByCall.find(key);
            if (it == g_frameByCall.end())
            {
                ++(kind == 4 ? g_shadowStats.missIdx : g_shadowStats.missMesh);
                return false;
            }
            if (it->second.empty())
            {
                // the call drew something but recorded nothing (the first few, to find the path)
                ++g_shadowStats.missEmpty;
                static int logged = 0;
                if (logged++ < 8)
                {
                    const uint32_t obj = uint32_t(word) << 4, el = kind == 10 ? guestAny(rdram, obj + 0x20) : obj;
                    const auto sk = g_skipped.find(el);
                    std::fprintf(stderr, "[pop:shadow] entry without records: kind %u object %08x index %u pass %u, element %08x decoded %d skinned %d skipped '%c'\n",
                                 kind, obj, index, guest8(rdram, entry + 0xD) & 0xF, el, int(g_elements.count(el)), int(g_skinned.count(el)),
                                 sk == g_skipped.end() ? '-' : sk->second);
                }
                return false;
            }
            for (const RecordRange &r : it->second)
                for (size_t i = r.first; i < r.first + r.count; ++i)
                {
                    if (r.kind == 0)
                        out.push_back({&g_elements[g_frameDraws[i].first].meshes, g_frameDraws[i].second.data(), g_frameMaterials[i].native});
                    else if (r.kind == 1)
                        out.push_back({&g_framePosed[i].first, kIdentity, g_framePosed[i].second.native});
                    else
                        out.push_back({&g_frameRefs[i].first, g_frameRefs[i].second.first.data(), g_frameRefs[i].second.second.native});
                }
            return !out.empty();
        }

        // one projector (FUN_004C3540's a0), its matrices and strength as FUN_004C2400 left them for this frame;
        // true if it was recorded completely and every receiver's base record is native (POP_NATIVE_WORLD)
        bool recordShadow(uint8_t *rdram, uint32_t proj)
        {
            ++g_shadowStats.projectors;
            if (guest8(rdram, proj + 0x50))
            {
                ++g_shadowStats.mirror; // mirror projectors draw their casters Z-tested (SetDrawShadowCastersMode): not yet
                return false;
            }
            const int w = int(guestAny(rdram, proj + 0x40C)), h = int(guestAny(rdram, proj + 0x410));
            if (w < 4 || h < 4 || w > 1024 || h > 1024)
                return false;
            bool allNative = true;
            // the casters: SetShadowCastingEnv's colour qw 21 = (128, 128, 128, strength x 128), FTOI0 in the VU
            const float strength = guestF(rdram, proj + 0x45C);
            const uint8_t casterAlpha = uint8_t(std::clamp(int(strength * 128.0f), 0, 255));
            pop_native::Image img;
            img.width = uint32_t(w);
            img.height = uint32_t(h);
            img.rgba.assign(size_t(w) * h * 4, 0xFF);
            for (size_t i = 3; i < img.rgba.size(); i += 4)
                img.rgba[i] = 0;
            GameMatrix toBuffer, tex, frustum;
            toBuffer.read(rdram, proj + 0x230);
            tex.read(rdram, proj + 0x2B0);
            frustum.read(rdram, proj + 0x270);
            const float ox = float(2048 - w / 2), oy = float(2048 - h / 2);
            const uint32_t casters = guestAny(rdram, proj), nc = guestAny(rdram, proj + 4);
            for (uint32_t i = 0; i < nc && i < 4096; ++i)
            {
                RecordGeometry geo;
                if (!entryGeometry(rdram, casters + 16 * i, geo))
                {
                    ++g_shadowStats.noGeometry;
                    allNative = false;
                    continue;
                }
                ++g_shadowStats.casters;
                for (const auto &[batches, model, native] : geo)
                    for (const auto &b : *batches)
                    {
                        std::vector<std::array<float, 3>> px(b.vertices.size());
                        for (size_t v = 0; v < b.vertices.size(); ++v)
                        {
                            float wp[3], c[4];
                            toWorld(model, b.vertices[v].pos, wp);
                            toBuffer.apply(wp, c);
                            px[v] = {c[0] / c[3] - ox, c[1] / c[3] - oy, c[3]};
                        }
                        for (size_t t = 0; t + 2 < b.indices.size(); t += 3)
                        {
                            const auto &p0 = px[b.indices[t]], &p1 = px[b.indices[t + 1]], &p2 = px[b.indices[t + 2]];
                            if (p0[2] <= 0 || p1[2] <= 0 || p2[2] <= 0)
                                continue;
                            rasterAlpha(img, p0.data(), p1.data(), p2.data(), casterAlpha, 1, w - 2, h - 2);
                            ++g_shadowStats.casterTris;
                        }
                    }
            }
            // as the renderer samples it: alpha on the 0x80 = 1.0 scale -> 0..255
            for (size_t i = 3; i < img.rgba.size(); i += 4)
                img.rgba[i] = uint8_t(std::min(255, (int(img.rgba[i]) * 255 + 64) / 128));
            const uint32_t key = kShadowKey + uint32_t(g_frameShadowImages.size());
            g_frameShadowImages.push_back(std::move(img));

            // the receivers: FUN_004B59C0's constants as FUN_004C3540 passes them
            float pos[3], fade[4], dir[3];
            for (int k = 0; k < 3; ++k)
                pos[k] = guestF(rdram, proj + 0x360 + 4 * k);
            for (int k = 0; k < 4; ++k)
                fade[k] = guestF(rdram, proj + 0x3A0 + 4 * k);
            const uint32_t dirAt = guest8(rdram, proj + 0x104) ? 0x0063FD20u : 0x0063FD30u;
            for (int k = 0; k < 3; ++k)
                dir[k] = guestF(rdram, dirAt + 4 * k);
            const float aBase = guestF(rdram, 0x70000468u), aW = guestF(rdram, 0x7000046Cu);
            DrawMaterial dm;
            dm.textureKey = key;
            dm.flags = 0xF0200u; // blend kind 0xF = Cd (1 - As) (ALPHA 0x46), no depth write
            dm.fogA = 255.0f;    // FGE off
            dm.zTest = 1u | 2u;  // Z test on; bit 1: texture clamped (CLAMP_1 5)
            dm.fbmsk = 0xFF000000u;
            dm.list = 0x1000u; // marks the shadow pass in the side files
            const uint32_t receivers = guestAny(rdram, proj + 0x10), nr = guestAny(rdram, proj + 0x14);
            for (uint32_t i = 0; i < nr && i < 4096; ++i)
            {
                RecordGeometry geo;
                if (!entryGeometry(rdram, receivers + 16 * i, geo))
                {
                    ++g_shadowStats.noGeometry;
                    allNative = false;
                    continue;
                }
                ++g_shadowStats.receivers;
                for (const auto &[batches, model, native] : geo)
                {
                    if (!native)
                        ++g_shadowStats.baseOnGs;
                    allNative = allNative && native;
                    // the projector position in object space (the VU setup: (P - W.t) . W's axes)
                    float local[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const float *axis = model + 4 * k;
                        local[k] = (pos[0] - model[12]) * axis[0] + (pos[1] - model[13]) * axis[1] + (pos[2] - model[14]) * axis[2];
                    }
                    std::vector<pop_assets::Batch> out;
                    for (const auto &b : *batches)
                    {
                        pop_assets::Batch o;
                        o.vertices = b.vertices;
                        std::vector<uint32_t> clipFlags(b.vertices.size());
                        for (size_t v = 0; v < o.vertices.size(); ++v)
                        {
                            pop_native::Vertex &x = o.vertices[v];
                            float wp[3], t[4], f[4], cam[4];
                            toWorld(model, x.pos, wp);
                            tex.apply(wp, t);
                            frustum.apply(wp, f);
                            for (int c = 0; c < 4; ++c)
                                cam[c] = wp[0] * g_frameClip[c] + wp[1] * g_frameClip[4 + c] + wp[2] * g_frameClip[8 + c] + g_frameClip[12 + c];
                            uint32_t cf = 0;
                            const float aw = std::fabs(f[3]);
                            for (int c = 0; c < 3; ++c)
                                cf |= (f[c] > aw ? 1u : 0u) << (2 * c) | (f[c] < -aw ? 2u : 0u) << (2 * c);
                            clipFlags[v] = cf;
                            float d2 = 0;
                            for (int k = 0; k < 3; ++k)
                                d2 += dir[k] * (local[k] - x.pos[k]) * (local[k] - x.pos[k]);
                            const float fd = std::clamp(fade[2] + fade[3] * d2, 0.0f, 1.0f);
                            const float a = std::max(std::min(aBase + aW * cam[3], 255.0f) * fd, 0.03125f);
                            x.uv[0] = t[0] / t[3];
                            x.uv[1] = t[1] / t[3];
                            x.colour[0] = x.colour[1] = x.colour[2] = 1.0f;
                            x.colour[3] = float(int(a)) / 128.0f;
                            x.uv2[0] = x.uv2[1] = 0;
                        }
                        for (size_t t = 0; t + 2 < b.indices.size(); t += 3)
                        {
                            const uint32_t i0 = b.indices[t], i1 = b.indices[t + 1], i2 = b.indices[t + 2];
                            if (clipFlags[i0] & clipFlags[i1] & clipFlags[i2])
                            {
                                ++g_shadowStats.culled;
                                continue;
                            }
                            o.indices.insert(o.indices.end(), {i0, i1, i2});
                        }
                        g_shadowStats.receiverTris += o.indices.size() / 3;
                        if (!o.indices.empty())
                            out.push_back(std::move(o));
                    }
                    if (out.empty())
                        continue;
                    DrawMaterial m = dm;
                    m.seq = g_frameSeq++;
                    std::array<float, 16> mm;
                    std::memcpy(mm.data(), model, sizeof(float) * 16);
                    g_frameShadows.emplace_back(std::move(out), std::make_pair(mm, m));
                }
            }
            return allNative;
        }

        // POP_NATIVE_WORLD: a projector whose receivers are all drawn natively is drawn natively too (its call skipped;
        // both lists emptied as FUN_004B8080 does after the draw); otherwise the GS draws it and its records go
        void shadowRtt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t proj = getRegU32(ctx, 4);
            if (g_frameCapturing && g_nativeShadows && (!g_world || g_worldOpen))
            {
                const size_t shadows0 = g_frameShadows.size(), images0 = g_frameShadowImages.size();
                const bool native = recordShadow(rdram, proj);
                if (g_world)
                {
                    if (native)
                    {
                        for (size_t i = shadows0; i < g_frameShadows.size(); ++i)
                            g_frameShadows[i].second.second.native = true;
                        const uint32_t zero = 0;
                        std::memcpy(getMemPtr(rdram, proj + 4), &zero, 4);
                        std::memcpy(getMemPtr(rdram, proj + 0x14), &zero, 4);
                        ++g_shadowStats.nativeProjectors;
                        return;
                    }
                    g_frameShadows.resize(shadows0);
                    g_frameShadowImages.resize(images0);
                }
            }
            g_origShadowRtt(rdram, ctx, runtime);
        }

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
        // worldOut (POP_NATIVE_WORLD): only the draws marked native, as PNF7 for pop_native_world
        // (textures by key with pixels once, batches by key with geometry once), returned instead of written or sent
        uint32_t g_frameGeneration = 0; // POP_NATIVE_WORLD: the kept-mesh generation of the frame being written
        void writeFrame(bool live = false, std::vector<uint8_t> *worldOut = nullptr)
        {
            if (worldOut)
            {
                live = true;
                g_frameGeneration = pop_native_world::sentGeneration();
            }
            std::vector<uint8_t> f;
            f.reserve(8u << 20);
            // "PNF5": clip[16]; images (count; per image: w, h, RGBA); draws in the game's order (count;
            // per draw: model[16], image index (-1: none), render state flags, AREF word, fog a, fog b,
            // FOGCOL, Z test, FBMSK, batch count, batches); then "GLOW" + ColorDiffusion's parameters
            // (calls, levels, FIX, mask, convolution, strength) when it ran in the frame
            // live: "PNF6" = PNF5 with each image as (texture key, w, h, RGBA), RGBA only the first time
            // the viewer gets that key on this connection (w = h = 0 afterwards: it keeps the texture)
            // world: "PNF7" = PNF6 with a 64-bit key before each batch; a batch the renderer keeps already is
            // (key, 0xFFFFFFFF), otherwise (key, vertex count, index count, data) as before; key 0 = this frame
            // only (posed characters: always with their data, never kept)
            // all three are written as "PNF8" + the layout (5, 6, 7), with fog min, max after fog b per draw
            // (the VU1 drivers' clamp of F)
            putBytes(f, "PNF8", 4);
            const uint32_t layout = worldOut ? 7 : live ? 6 : 5;
            putBytes(f, &layout, size_t(4) * size_t(1));
            putBytes(f, g_frameClip, size_t(4) * size_t(16));
            std::unordered_map<uint32_t, int32_t> imageOf;
            std::vector<pop_native::Image> images;
            std::vector<uint32_t> imageKeys;
            std::vector<DrawMaterial> allMaterials;
            for (const DrawMaterial &m : g_frameMaterials)
                if (!worldOut || m.native)
                    allMaterials.push_back(m);
            for (const auto &p : g_framePosed)
                if (!worldOut || p.second.native)
                    allMaterials.push_back(p.second);
            for (const auto &r : g_frameRefs)
                if (!worldOut || r.second.second.native)
                    allMaterials.push_back(r.second.second);
            for (const auto &r : g_frameShadows)
                if (!worldOut || r.second.second.native)
                    allMaterials.push_back(r.second.second);
            for (const OtherDraw &o : worldOut ? std::vector<OtherDraw>{} : g_frameOther) // other paths' textures too (image list only)
            {
                DrawMaterial m;
                m.textureKey = o.textureKey;
                m.flags = o.flags;
                allMaterials.push_back(m);
            }
            for (const GfxDraw &g : worldOut ? std::vector<GfxDraw>{} : g_frameGfx) // GFX textures too (image list only, for inspection)
            {
                DrawMaterial m;
                m.textureKey = g.textureKey;
                m.flags = g.flags;
                allMaterials.push_back(m);
            }
            for (const DrawMaterial &m : allMaterials)
                if (m.textureKey && m.textureKey != kRefractionKey && m.textureKey != kHaloAlphaKey && m.textureKey != kHaloRestoreKey &&
                    !imageOf.count(m.textureKey))
                {
                    pop_native::Image img;
                    bool found = false;
                    if (m.textureKey - kShadowKey < g_frameShadowImages.size())
                    {
                        // this frame's shadow textures (never cached: they change every frame)
                        img = g_frameShadowImages[m.textureKey - kShadowKey];
                        found = true;
                    }
                    else if (m.textureKey - kPaletteKey < 0x8000u && g_framePaletteImages.count(m.textureKey - kPaletteKey))
                    {
                        img = g_framePaletteImages[m.textureKey - kPaletteKey]; // recoloured this frame (never cached)
                        found = true;
                    }
                    else if (live)
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
            if (live && !worldOut && g_liveConnection != pop_live_client::connection())
            {
                g_liveConnection = pop_live_client::connection(); // a new viewer has none of our textures
                g_liveSent.clear();
            }
            std::unordered_set<uint32_t> &sent = worldOut ? g_worldSent : g_liveSent;
            for (size_t i = 0; i < images.size(); ++i)
            {
                const auto &img = images[i];
                if (live)
                {
                    putBytes(f, &imageKeys[i], 4);
                    // shadow textures always with their pixels (the viewer updates the image it has for the key)
                    if (!sent.insert(imageKeys[i]).second && imageKeys[i] - kShadowKey >= 0x10000u)
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
            size_t tris = 0, textured = 0;
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            // every draw in the game's order: (seq, kind 0 static / 1 skinned / 2 instanced / 3 shadow receiver, index)
            std::vector<std::array<uint32_t, 3>> order;
            for (size_t i = 0; i < g_frameDraws.size(); ++i)
                if (!worldOut || g_frameMaterials[i].native)
                    order.push_back({g_frameMaterials[i].seq, 0u, uint32_t(i)});
            for (size_t i = 0; i < g_framePosed.size(); ++i)
                if (!worldOut || g_framePosed[i].second.native)
                {
                    order.push_back({g_framePosed[i].second.seq, 1u, uint32_t(i)});
                    g_worldStats.posed += worldOut ? 1 : 0;
                }
            for (size_t i = 0; i < g_frameRefs.size(); ++i)
                if (!worldOut || g_frameRefs[i].second.second.native)
                    order.push_back({g_frameRefs[i].second.second.seq, 2u, uint32_t(i)});
            for (size_t i = 0; i < g_frameShadows.size(); ++i)
                if (!worldOut || g_frameShadows[i].second.second.native)
                    order.push_back({g_frameShadows[i].second.second.seq, 3u, uint32_t(i)});
            std::sort(order.begin(), order.end());
            const uint32_t n = uint32_t(order.size());
            putBytes(f, &n, size_t(4) * size_t(1));
            // side file: per written draw its kind, source index, render state, texture key and mean
            // vertex colour (0x80 = 1.0), to identify draws found by bisecting a frame in the viewer
            FILE *dl = live || g_framePath.empty() ? nullptr : std::fopen((g_framePath + ".draws.txt").c_str(), "w");
            uint32_t drawNo = 0;
            for (const auto &o : order)
            {
                if (dl)
                {
                    const auto &rec = o[1] == 3 ? g_frameShadows[o[2]] : g_frameRefs[o[2]];
                    const DrawMaterial &m = o[1] == 0 ? g_frameMaterials[o[2]] : o[1] == 1 ? g_framePosed[o[2]].second : rec.second.second;
                    const auto &bs = o[1] == 0 ? g_elements[g_frameDraws[o[2]].first].meshes : o[1] == 1 ? g_framePosed[o[2]].first : rec.first;
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
                                 o[1] == 0 ? "static" : o[1] == 1 ? "skinned" : o[1] == 2 ? "instanced" : "shadow", o[2],
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
                    auto &rec = o[1] == 3 ? g_frameShadows[o[2]] : g_frameRefs[o[2]];
                    model = rec.second.first.data();
                    matp = &rec.second.second;
                    batches = &rec.first;
                }
                const DrawMaterial &mat = *matp;
                const int32_t image = mat.textureKey == kRefractionKey ? -2 : mat.textureKey == kHaloAlphaKey ? -3 : mat.textureKey == kHaloRestoreKey ? -4 : mat.textureKey && imageOf.count(mat.textureKey) ? imageOf[mat.textureKey] : -1;
                textured += image >= 0;
                const uint32_t nb = uint32_t(batches->size());
                putBytes(f, model, size_t(4) * size_t(16));
                putBytes(f, &image, size_t(4) * size_t(1));
                putBytes(f, &mat.flags, size_t(4) * size_t(1));
                putBytes(f, &mat.ref, size_t(4) * size_t(1));
                putBytes(f, &mat.fogA, size_t(4) * size_t(1));
                putBytes(f, &mat.fogB, size_t(4) * size_t(1));
                putBytes(f, &mat.fogMin, size_t(4) * size_t(1));
                putBytes(f, &mat.fogMax, size_t(4) * size_t(1));
                putBytes(f, &mat.fogColour, size_t(4) * size_t(1));
                putBytes(f, &mat.zTest, size_t(4) * size_t(1));
                putBytes(f, &mat.fbmsk, size_t(4) * size_t(1));
                putBytes(f, &nb, size_t(4) * size_t(1));
                const uint64_t variant = worldOut ? vertexVariant(mat) : 0;
                for (size_t bi = 0; bi < batches->size(); ++bi)
                {
                    const auto &b = (*batches)[bi];
                    if (worldOut)
                    {
                        // posed (skinned) batches change every frame: key 0, drawn for this frame only
                        // shadow receivers too (their UVs and alpha follow the projector)
                        const uint64_t key = o[1] == 0   ? mix64(mix64(0x5354u, (uint64_t(g_elements[g_frameDraws[o[2]].first].serial) << 16) | bi), variant)
                                             : o[1] == 1 || o[1] == 3 || mat.frameOnly ? 0u
                                                         : mix64(contentKey(b), variant);
                        putBytes(f, &key, 8);
                        tris += b.indices.size() / 3;
                        if (key && pop_native_world::meshSent(key, g_frameGeneration))
                        {
                            const uint32_t known = 0xFFFFFFFFu;
                            putBytes(f, &known, 4);
                            ++g_worldStats.batchesKnown;
                            continue;
                        }
                        g_worldStats.batchesSent += key ? 1 : 0;
                    }
                    const uint32_t nv = uint32_t(b.vertices.size()), ni = uint32_t(b.indices.size());
                    putBytes(f, &nv, size_t(4) * size_t(1));
                    putBytes(f, &ni, size_t(4) * size_t(1));
                    writeVertices(f, b.vertices, mat);
                    putBytes(f, b.indices.data(), size_t(4) * size_t(ni));
                    if (!worldOut)
                        tris += ni / 3;
                }
            }
            if (dl)
                std::fclose(dl);
            // the world frame has no post effects of its own (the GS runs them over the whole picture)
            const uint32_t noCalls = 0;
            putBytes(f, "GLOW", size_t(1) * size_t(4));
            putBytes(f, worldOut ? &noCalls : &g_glowCalls, size_t(4) * size_t(1));
            putBytes(f, g_glow, size_t(4) * size_t(5));
            putBytes(f, "HAZE", size_t(1) * size_t(4));
            putBytes(f, worldOut ? &noCalls : &g_hazeCalls, size_t(4) * size_t(1));
            putBytes(f, g_haze, size_t(4) * size_t(2));
            if (worldOut)
            {
                g_worldStats.bytes += f.size();
                *worldOut = std::move(f);
                return;
            }
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
                    const uint32_t list = material ? guestAny(g_rdramPtr, material + 0x10) : 0; // the first texture node
                    const uint32_t texIndex = list ? guest16(g_rdramPtr, list) : 0xFFFF;
                    std::fprintf(stderr, "[pop:native]   halo %u: pos (%.2f %.2f %.2f) half size %.2f x %.2f colour %08x material %08x tex index %04x key %08x\n",
                                 h, v[0], v[1], v[2], v[4], v[5], colour, material, texIndex,
                                 texIndex != 0xFFFF ? popTextureKey(texIndex) : 0u);
                }
            }
            if (g_shadowStats.projectors)
                std::fprintf(stderr, "[pop:native] shadows: %llu projectors (%llu mirror, left out), %llu casters (%llu triangles), %llu receivers (%llu triangles, %llu culled), %llu list entries without records\n",
                             (unsigned long long)g_shadowStats.projectors, (unsigned long long)g_shadowStats.mirror, (unsigned long long)g_shadowStats.casters,
                             (unsigned long long)g_shadowStats.casterTris, (unsigned long long)g_shadowStats.receivers, (unsigned long long)g_shadowStats.receiverTris,
                             (unsigned long long)g_shadowStats.culled, (unsigned long long)g_shadowStats.noGeometry);
            std::fprintf(stderr, "[pop:native] frame written to %s: %u draws (%zu skinned, %zu instanced, %zu textured, %u images), %zu triangles\n",
                         g_framePath.c_str(), n, g_framePosed.size(), g_frameRefs.size(), textured, ni0, tris);
        }

        void listRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_currentList = getRegU32(ctx, 4) - g_drawManager;
            if (g_world && g_worldOpen && g_currentList == 0x88u && g_worldPure)
                ++g_lateStats.pureAtLate;
            g_origListRender(rdram, ctx, runtime);
        }

        // GS draws the frame's purity (POP_NATIVE_LATE) has to know about besides the list paths: FUN_004B80C0(list,
        // mode) draws a shadow projector's casters or receivers on the GS (every path of FUN_004C2400 but a native
        // FUN_004C3540); FUN_004BDC80 (the shadow manager, between lists +0x78 and +0x4C when both +0x68 and +0x78
        // hold entries; not read yet: taken as drawing)
        PS2Runtime::RecompiledFunction g_origShadowList = nullptr, g_origShadowZ = nullptr;
        void shadowList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (guestAny(rdram, getRegU32(ctx, 4) + 4))
                worldOnGs(rdram, "shadow");
            g_origShadowList(rdram, ctx, runtime);
        }
        void shadowZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            worldOnGs(rdram, "FUN_004BDC80");
            g_origShadowZ(rdram, ctx, runtime);
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
            g_frameByCall.clear();
            g_frameShadows.clear();
            g_frameShadowImages.clear();
            g_framePaletteImages.clear();
            g_framePaletteKeys.clear();
            g_frameSeq = 0;
            g_glowCalls = 0;
            g_hazeCalls = 0;
            g_haloAddsInWindow = 0;
        }

        void logClipChange(uint8_t *rdram, const char *when)
        {
            static const bool on = std::getenv("POP_NATIVE_WORLD_DUMP") != nullptr;
            static int logged = 0;
            const uint32_t gs = guestAny(rdram, 0x00675148u);
            if (!on || !gs || logged >= 400)
                return;
            float now[16], diff = 0.0f;
            for (int k = 0; k < 16; ++k)
            {
                now[k] = guestF(rdram, gs + 0x240u + 4u * k);
                diff = std::max(diff, std::fabs(now[k] - g_frameClip[k]));
            }
            if (diff < 1e-4f && std::strcmp(when, "kick") == 0)
                return;
            ++logged;
            std::fprintf(stderr, "[pop:world] frame %u: %s (views %u), clip changed by %.4g; row 3 captured %.3f %.3f %.3f %.3f, now %.3f %.3f %.3f %.3f\n",
                         g_worldSeq, when, g_worldViews, diff, g_frameClip[12], g_frameClip[13], g_frameClip[14], g_frameClip[15], now[12], now[13], now[14], now[15]);
        }

        // POP_NATIVE_WORLD: the frame's first Display3DElements starts its capture and puts the marker
        // in the DMA list; a later one in the same frame (another view) leaves its draws to the GS
        void worldDisplay3D(uint8_t *rdram)
        {
            if (g_worldMarked)
            {
                g_worldOpen = false;
                ++g_worldViews;
                logClipChange(rdram, "Display3DElements again");
                return;
            }
            g_worldViews = 1;
            resetFrameCapture();
            g_worldBase.clear();
            g_worldMissing.clear();
            const uint32_t gs = guestAny(rdram, 0x00675148u);
            if (!gs)
                return;
            for (int k = 0; k < 16; ++k)
                g_frameClip[k] = guestF(rdram, gs + 0x240u + 4u * k);
            // instanced elements with a lightmap entry (CLMElementList at +0x5C: 8-byte entries, kind
            // 10 in the low nibble of the second word): their lightmaps are drawn by the GS, so they are too
            g_worldLmRefs.clear();
            const uint32_t lm = g_drawManager + 0x5Cu, entries = guestAny(rdram, lm), count = guestAny(rdram, lm + 4);
            for (uint32_t i = 0; i < count && i < 4096; ++i)
                if ((guestAny(rdram, entries + 8 * i + 4) & 0xF) == 10)
                    g_worldLmRefs.insert(guestAny(rdram, entries + 8 * i));
            // the marker: CNT tag (2 qw) with VIF FLUSHA (all paths idle, so nothing drawn before is
            // reordered after it) + DIRECT 2; GIF tag (NLOOP 1, EOP, PACKED, NREG 1, A+D); A+D data
            const uint32_t cursor = guestAny(rdram, gs + 0xECu);
            const uint32_t qw[12] = {0x10000002u, 0u, 0x13000000u, 0x50000002u, 0x00008001u, 0x10000000u, 0xEu, 0u, ++g_worldSeq, kWorldMagic, 0x7Fu, 0u};
            const uint32_t next = cursor + 0x30u;
            std::memcpy(getMemPtr(rdram, cursor), qw, sizeof qw);
            std::memcpy(getMemPtr(rdram, gs + 0xECu), &next, 4);
            g_frameCapturing = true;
            g_worldMarked = true;
            g_worldOpen = true;
            g_worldPure = true;
            g_frameClosedBy.clear();
        }

        // glitch capture (pop_native_world dumpRecent): the frame's camera, fog zones 0-4 and purity, one line
        void noteGlitchFrame(uint8_t *rdram)
        {
            static uint64_t lastNative = 0, lastGs = 0;
            std::string t;
            char b[160];
            std::snprintf(b, sizeof b, "views %u, %s, late native %llu GS %llu; clip", g_worldViews,
                          g_worldPure ? "pure to the end" : ("purity ended by " + g_frameClosedBy).c_str(),
                          (unsigned long long)(g_lateStats.native - lastNative), (unsigned long long)(g_lateStats.gs - lastGs));
            t += b;
            lastNative = g_lateStats.native;
            lastGs = g_lateStats.gs;
            for (float c : g_frameClip)
            {
                std::snprintf(b, sizeof b, " %.5g", double(c));
                t += b;
            }
            t += "; fog";
            for (uint32_t z = 0; z < 5; ++z)
            {
                const uint32_t a = 0x70000270u + 16u * z;
                std::snprintf(b, sizeof b, " %u:(%.0f %.0f %.1f %.4g %06x)", z, double(guestF(rdram, a)), double(guestF(rdram, a + 4)),
                              double(guestF(rdram, a + 8)), double(guestF(rdram, a + 12)), guestAny(rdram, 0x70000470u + 4u * z) & 0xFFFFFFu);
                t += b;
            }
            pop_native_world::noteFrame(g_worldSeq, std::move(t));
        }

        // the frame's DMA kick (0x4A2C80) from PS2_Flip: the frame's native draws go to pop_native_world
        PS2Runtime::RecompiledFunction g_origKick = nullptr;
        void worldKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if (g_worldMarked && ra >= 0x004A20C0u && ra < 0x004A2200u)
            {
                logClipChange(rdram, "kick");
                noteGlitchFrame(rdram);
                std::vector<uint8_t> f;
                writeFrame(true, &f);
                pop_native_world::submitFrame(g_worldSeq, g_frameGeneration, std::move(f));
                resetFrameCapture();
                g_frameCapturing = false;
                g_worldMarked = g_worldOpen = false;
                if (++g_worldStats.frames % 300 == 0)
                    std::fprintf(stderr, "[pop:world] frames %llu: calls native %llu, kept for textures %llu, not decoded %llu, lightmaps %llu; %.0f KB per frame; "
                                         "batches sent %llu, kept by the renderer %llu; skinned draws native %llu, driver switches replayed %llu, "
                                         "skinned calls on the GS (state/not decoded/texture) %llu/%llu/%llu, lit static draws on the GS %llu, "
                                         "native lit/env %llu/%llu\n",
                                 (unsigned long long)g_worldStats.frames, (unsigned long long)g_worldStats.skipped,
                                 (unsigned long long)g_worldStats.noTexture, (unsigned long long)g_worldStats.notRecorded,
                                 (unsigned long long)g_worldStats.lightmaps, double(g_worldStats.bytes) / 1024.0 / double(g_worldStats.frames),
                                 (unsigned long long)g_worldStats.batchesSent, (unsigned long long)g_worldStats.batchesKnown,
                                 (unsigned long long)g_worldStats.posed, (unsigned long long)g_worldStats.drivers,
                                 (unsigned long long)g_worldStats.skinGs[0], (unsigned long long)g_worldStats.skinGs[1],
                                 (unsigned long long)g_worldStats.skinGs[2], (unsigned long long)g_worldStats.litStatic,
                                 (unsigned long long)g_worldStats.litNative, (unsigned long long)g_worldStats.envNative);
                ++g_lateStats.frames;
                if (g_worldLate && g_worldStats.frames % 300 == 0)
                {
                    std::string top;
                    std::vector<std::pair<uint64_t, std::string>> by;
                    for (const auto &[why, n] : g_lateStats.closedBy)
                        by.emplace_back(n, why);
                    std::sort(by.rbegin(), by.rend());
                    for (size_t i = 0; i < by.size() && i < 5; ++i)
                        top += (i ? ", " : "") + by[i].second + " " + std::to_string(by[i].first);
                    std::fprintf(stderr, "[pop:world] late lists: frames %llu, pure at +0x88 %llu, ended within %llu; calls native %llu, GS %llu; "
                                         "particles native %llu; ended by: %s\n",
                                 (unsigned long long)g_lateStats.frames, (unsigned long long)g_lateStats.pureAtLate,
                                 (unsigned long long)g_lateStats.closedInLate, (unsigned long long)g_lateStats.native,
                                 (unsigned long long)g_lateStats.gs, (unsigned long long)g_worldStats.particles, top.c_str());
#if POP_SPRITE_VU1
                    std::fprintf(stderr, "[pop:world] sprites (P9 %s): draws %llu, native %llu, quads %llu, not replayed %llu; VU runs %llu, "
                                         "handed to the interpreter %llu\n",
                                 !g_spriteVu.loaded ? "not loaded yet" : g_spriteVu.usePort ? "port" : "on the interpreter",
                                 (unsigned long long)g_spriteStats.draws,
                                 (unsigned long long)g_spriteStats.native, (unsigned long long)g_spriteStats.quads,
                                 (unsigned long long)g_spriteStats.failed, (unsigned long long)g_spriteVu.runs,
                                 (unsigned long long)g_spriteVu.interpreted);
                    if (g_p5)
                        std::fprintf(stderr, "[pop:world] particles (P5 %s): draws %llu, native %llu, triangles %llu, not replayed %llu; VU runs %llu, "
                                             "handed to the interpreter %llu\n",
                                     !g_particleVu.loaded ? "not loaded yet" : g_particleVu.usePort ? "port" : "on the interpreter",
                                     (unsigned long long)g_particleStats.draws, (unsigned long long)g_particleStats.native,
                                     (unsigned long long)g_particleStats.quads, (unsigned long long)g_particleStats.failed,
                                     (unsigned long long)g_particleVu.runs, (unsigned long long)g_particleVu.interpreted);
                    if (g_waterVu.loaded)
                        std::fprintf(stderr, "[pop:world] water (P6 %s): draws %llu, native %llu, not replayed %llu; VU runs %llu\n",
                                     g_waterVu.usePort ? "port" : "on the interpreter", (unsigned long long)g_waterStats.draws,
                                     (unsigned long long)g_waterStats.native, (unsigned long long)g_waterStats.failed, (unsigned long long)g_waterVu.runs);
                    if (g_haloStats.calls)
                        std::fprintf(stderr, "[pop:world] halos: %llu, native %llu, with ghosts (GS) %llu\n", (unsigned long long)g_haloStats.calls,
                                     (unsigned long long)g_haloStats.native, (unsigned long long)g_haloStats.ghosts);
#endif
                }
                if (g_nativeShadows && g_worldStats.frames % 300 == 0)
                    std::fprintf(stderr, "[pop:world] shadows: projectors %llu, native %llu (mirror %llu), receivers %llu, list entries without records %llu "
                                         "(idx %llu, mesh/ref %llu, other kinds %llu, call without records %llu), entries that draw nothing %llu, receiver bases on the GS %llu\n",
                                 (unsigned long long)g_shadowStats.projectors, (unsigned long long)g_shadowStats.nativeProjectors,
                                 (unsigned long long)g_shadowStats.mirror, (unsigned long long)g_shadowStats.receivers,
                                 (unsigned long long)g_shadowStats.noGeometry, (unsigned long long)g_shadowStats.missIdx,
                                 (unsigned long long)g_shadowStats.missMesh, (unsigned long long)g_shadowStats.missOther,
                                 (unsigned long long)g_shadowStats.missEmpty, (unsigned long long)g_shadowStats.drawsNothing,
                                 (unsigned long long)g_shadowStats.baseOnGs);
            }
            g_origKick(rdram, ctx, runtime);
        }

        // CLMElementList::Render (0x4B7B80): its Display calls (pass 3) belong to list +0x5C
        PS2Runtime::RecompiledFunction g_origLmRender = nullptr;
        void lmRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_currentList = getRegU32(ctx, 4) - g_drawManager;
            g_origLmRender(rdram, ctx, runtime);
        }

        void display3D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdramPtr = rdram;
            g_runtimePtr = runtime;
            g_drawManager = getRegU32(ctx, 4);
            if (g_world)
            {
                g_rdramForDump = [rdram](uint32_t a) { return guestAny(rdram, a); };
                worldDisplay3D(rdram);
                g_origDisplay3D(rdram, ctx, runtime);
                return;
            }
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
        // lightmap (CLMElementList::Render), gfx (FUN_00458FC0), sprite (CJadeSpriteElement::Display), water
        // (CJSmWaterElement::Display), list18 (FUN_00469C00). Without POP_NATIVE_LIVE only.
        enum SkipGroup : unsigned
        {
            kSkipStatic = 1,
            kSkipSkinned = 2,
            kSkipLightmap = 4,
            kSkipGfx = 8,
            kSkipSprite = 16,
            kSkipWater = 32,
            kSkipList18 = 64
        };
        unsigned g_skip = 0;
        uint32_t g_skipRa = 0; // POP_NATIVE_SKIP_RA=<hex> (debug): mesh/ref calls skipped only from this return address
        bool g_skipReplay = false; // POP_NATIVE_SKIP_REPLAY=1: skip every chain of the group, replaying its driver switches
        PS2Runtime::RecompiledFunction g_skipOrig[8] = {};
        uint64_t g_skipCount[8] = {}, g_kept[8] = {};
        constexpr uint32_t kSkipAddress[8] = {kMeshDisplay, kRefDisplay, 0x004BDB80u, 0x004B7B80u, kGfxDisplay,
                                              0x004C6200u, 0x004C77C0u, 0x00469C00u};

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

        void chainReplayDrivers(uint8_t *rdram, uint32_t node, bool ref)
        {
            for (unsigned link = 0; node && link < 4096; ++link)
            {
                if (const uint32_t palette = guest32(rdram, (ref ? guest32(rdram, node + 0x20u) : node) + 0x48u))
                    worldReplayDriver(rdram, palette);
                node = guest32(rdram, node + (ref ? 0x1Cu : 0x4Cu));
            }
        }

        bool skipCall(int n, uint8_t *rdram, R5900Context *ctx)
        {
            switch (n)
            {
            case 0: // CJMeshElement: the kind judged by the chain's head
            case 1: // CJMeshElementRef
            {
                if (g_skipRa && getRegU32(ctx, 31) != g_skipRa)
                    return false;
                const uint32_t node = getRegU32(ctx, 4);
                const uint32_t element = n == 1 ? guest32(rdram, node + 0x20u) : node;
                const unsigned group = guest32(rdram, element + 0x48u) ? kSkipSkinned : kSkipStatic;
                if (!(g_skip & group))
                    return false;
                if (g_skipReplay)
                {
                    chainReplayDrivers(rdram, node, n == 1);
                    return true;
                }
                return !chainSwitchesDriver(rdram, node, n == 1);
            }
            case 2:
                return (g_skip & kSkipStatic) != 0;
            case 3:
                return (g_skip & kSkipLightmap) != 0;
            case 4:
                return (g_skip & kSkipGfx) != 0;
            case 5:
                return (g_skip & kSkipSprite) != 0;
            case 6:
                return (g_skip & kSkipWater) != 0;
            default:
                return (g_skip & kSkipList18) != 0;
            }
        }

        // POP_NATIVE_SKIP_CALLERS=1 (debug): mesh/ref Display calls per (path, caller $ra, palette or not), with
        // one sample element and its matrix translation; finds draws made outside CElementList::Render
        struct CallerSample
        {
            uint64_t count = 0;
            uint32_t element = 0, matrix = 0;
            float at[3] = {};
        };
        bool g_logCallers = false;
        std::mutex g_callersMutex;
        std::map<std::tuple<int, uint32_t, bool>, CallerSample> g_callers;
        void noteCaller(int n, uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t node = getRegU32(ctx, 4), index = getRegU32(ctx, 5) & 0xFFFF;
            const uint32_t element = n == 1 ? guest32(rdram, node + 0x20u) : node;
            std::lock_guard<std::mutex> lock(g_callersMutex);
            CallerSample &c = g_callers[{n, getRegU32(ctx, 31), guest32(rdram, element + 0x48u) != 0}];
            if (!c.count++)
            {
                c.element = element;
                c.matrix = index;
                for (int k = 0; k < 3; ++k)
                    c.at[k] = guestF(rdram, guestAny(rdram, 0x70000260u) + index * 0x40u + 0x30u + 4u * k);
            }
        }

        template <int N>
        void skippable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (N <= 1 && g_logCallers)
                noteCaller(N, rdram, ctx);
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
                                            {"lightmap", kSkipLightmap}, {"gfx", kSkipGfx}, {"sprite", kSkipSprite},
                                            {"water", kSkipWater}, {"list18", kSkipList18}, {"all", 15u}})
                if (spec.find(name) != std::string::npos)
                    g_skip |= bit;
            g_skipReplay = std::getenv("POP_NATIVE_SKIP_REPLAY") && *std::getenv("POP_NATIVE_SKIP_REPLAY") == '1';
            g_logCallers = std::getenv("POP_NATIVE_SKIP_CALLERS") && *std::getenv("POP_NATIVE_SKIP_CALLERS") == '1';
            if (const char *ra = std::getenv("POP_NATIVE_SKIP_RA"))
                g_skipRa = uint32_t(std::strtoul(ra, nullptr, 16));
            const PS2Runtime::RecompiledFunction hooks[8] = {skippable<0>, skippable<1>, skippable<2>, skippable<3>,
                                                             skippable<4>, skippable<5>, skippable<6>, skippable<7>};
            for (int n = 0; n < 8; ++n)
                if ((g_skipOrig[n] = runtime.lookupFunction(kSkipAddress[n])))
                    runtime.replaceFunction(kSkipAddress[n], hooks[n]);
            std::fprintf(stderr, "[pop:native] POP_NATIVE_SKIP=%s: groups %x%s (measurement only)\n", spec.c_str(), g_skip,
                         g_skipReplay ? ", driver switches replayed" : "");
            std::thread([] {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                    std::fprintf(stderr, "[pop:native] skipped/kept: mesh %llu/%llu ref %llu/%llu idx %llu/%llu lm %llu/%llu gfx %llu/%llu "
                                         "sprite %llu/%llu water %llu/%llu list18 %llu/%llu\n",
                                 (unsigned long long)g_skipCount[0], (unsigned long long)g_kept[0], (unsigned long long)g_skipCount[1],
                                 (unsigned long long)g_kept[1], (unsigned long long)g_skipCount[2], (unsigned long long)g_kept[2],
                                 (unsigned long long)g_skipCount[3], (unsigned long long)g_kept[3], (unsigned long long)g_skipCount[4],
                                 (unsigned long long)g_kept[4], (unsigned long long)g_skipCount[5], (unsigned long long)g_kept[5],
                                 (unsigned long long)g_skipCount[6], (unsigned long long)g_kept[6], (unsigned long long)g_skipCount[7],
                                 (unsigned long long)g_kept[7]);
                    if (g_logCallers)
                    {
                        std::lock_guard<std::mutex> lock(g_callersMutex);
                        for (const auto &[key, c] : g_callers)
                            std::fprintf(stderr, "[pop:native] caller %s ra %08x %s: %llu calls, e.g. element %08x matrix %u at (%.2f %.2f %.2f)\n",
                                         std::get<0>(key) ? "ref " : "mesh", std::get<1>(key), std::get<2>(key) ? "palette" : "static ",
                                         (unsigned long long)c.count, c.element, c.matrix, double(c.at[0]), double(c.at[1]), double(c.at[2]));
                    }
                }
            }).detach();
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("POP_NATIVE_LIVE");
        if (const char *world = std::getenv("POP_NATIVE_WORLD"); world && *world == '1')
        {
            // renderer step 1: the static world drawn natively into the GS buffers (see g_world)
            if (!pop_native_world::active(&runtime.memory().gs()))
            {
                std::fprintf(stderr, "[pop:world] POP_NATIVE_WORLD=1 but this runtime cannot inject (no GS host hook or no Vulkan renderer)\n");
                return;
            }
            const struct
            {
                uint32_t address;
                PS2Runtime::RecompiledFunction hook, *original;
            } hooks[] = {{kReadElement, readElement, &g_origRead},
                         {kMeshDisplay, display, &g_origDisplay},
                         {kDisplay3D, display3D, &g_origDisplay3D},
                         {kRefDisplay, refDisplay, &g_origRefDisplay},
                         {0x004B9440u, listRender, &g_origListRender},
                         {0x004B7B80u, lmRender, &g_origLmRender},
                         {0x004A2C80u, worldKick, &g_origKick},
                         {g_other[0].address, otherDisplay<0>, &g_other[0].original},
                         {g_other[1].address, otherDisplay<1>, &g_other[1].original},
                         {g_other[2].address, otherDisplay<2>, &g_other[2].original},
                         {g_other[3].address, otherDisplay<3>, &g_other[3].original},
                         {0x004584C0u, particles, &g_origParticles},
                         {0x004B80C0u, shadowList, &g_origShadowList},
                         {0x004BDC80u, shadowZ, &g_origShadowZ},
                         {kShadowRtt, shadowRtt, &g_origShadowRtt}};
            for (const auto &h : hooks)
                if (!(*h.original = runtime.lookupFunction(h.address)))
                {
                    std::fprintf(stderr, "[pop:world] function %08x not found\n", h.address);
                    return;
                }
            for (const auto &h : hooks)
                runtime.replaceFunction(h.address, h.hook);
            g_live = g_world = true;
            g_start = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[pop:world] native static world on\n");
            return;
        }
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
#if POP_SPRITE_VU1
            g_origHalo = runtime.lookupFunction(0x004AA240u);
            if (g_origHalo)
                runtime.replaceFunction(0x004AA240u, haloDisplay);
#endif
            g_origParticles = runtime.lookupFunction(0x004584C0u);
            if (g_origParticles)
                runtime.replaceFunction(0x004584C0u, particles);
            g_origGfx = runtime.lookupFunction(kGfxDisplay);
            if (g_origGfx)
                runtime.replaceFunction(kGfxDisplay, gfxDisplay);
            g_origShadowRtt = runtime.lookupFunction(kShadowRtt);
            if (g_origShadowRtt)
                runtime.replaceFunction(kShadowRtt, shadowRtt);
        }
        g_lastReport = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[pop:native] live conversion check on\n");
    }
}
