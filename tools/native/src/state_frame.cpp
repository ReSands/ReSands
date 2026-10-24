// pop_state_frame: a native frame (PNF, as POP_NATIVE_FRAME writes it) from a pop-sotc save state,
// offline, with the live code itself (tools/overrides/pop_native_live.cpp, compiled in unchanged).
//
//   pop_state_frame <state> <out.pnf> [--bigfile DATA/PRINCE.BF] [--packindex packindex.txt]
//   pop_native_view <out.pnf> <any> <out.png> --frame
//
// A save holds the game's memory at a frame boundary: the draw manager (scratchpad 0x700000C0) still
// holds the last frame's element lists, the matrix pool, camera, light sets and fog table. This tool
// stands in for the runtime: it records the hooks pop_native_live::install registers and replays
// Display3DElements (0x4BC000) -> CElementList::Render (0x4B9440) over those lists, setting the
// render state Render sets before each Display (current sub-material 0x34, material 0x30, fog zone
// 0x84 and FOGCOL 0x24 as SetBlending sends it, Z test 0x38, alpha-only mask 0x44, light set 0x88),
// and calls the
// same hooks the game would. Elements were not converted at load (no ReadElement), so the live code
// decodes them from their pass packets. Textures: index -> key from the engine's texture list
// (TEX_List: count 0x6CBC50, entries 0x6CBC58, 16 bytes: key, ..., +10 index), the packs holding them
// from tools/packindex.py's index.
// Not replayed yet: instanced lightmaps, halos; shadows only on the render-to-texture path (POP_NATIVE_SHADOWS=1).
// Save-state layout: tools/popstate.py.

#include "../../overrides/pop_native_live.cpp"
#include "bigfile.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <tuple>
#include <set>

namespace
{
    std::vector<uint8_t> g_ram(32u << 20), g_spr(16u << 10);
    std::unordered_map<uint32_t, PS2Runtime::RecompiledFunction> g_hooks;
    std::unordered_map<uint32_t, uint32_t> g_texKeys; // texture index -> key
    std::string g_bigfile = "DATA/PRINCE.BF";

    uint32_t rd32(uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, getConstMemPtr(g_ram.data(), a), 4);
        return v;
    }
    void wr32(uint32_t a, uint32_t v) { std::memcpy(getMemPtr(g_ram.data(), a), &v, 4); }
    void wr8(uint32_t a, uint8_t v) { *getMemPtr(g_ram.data(), a) = v; }

    void call(uint32_t address, R5900Context *ctx)
    {
        const auto it = g_hooks.find(address);
        if (it != g_hooks.end())
            it->second(g_ram.data(), ctx, nullptr);
    }
    void setArgs(R5900Context &ctx, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0)
    {
        ctx.r[4] = _mm_set_epi32(0, 0, 0, int(a0));
        ctx.r[5] = _mm_set_epi32(0, 0, 0, int(a1));
        ctx.r[6] = _mm_set_epi32(0, 0, 0, int(a2));
    }

    void noop(uint8_t *, R5900Context *, PS2Runtime *) {}
    void gfxEntry(uint8_t *, R5900Context *ctx, PS2Runtime *);

    // CElementList::Render (0x4B9440) over one list: entries of 16 bytes at *list, count at list + 4.
    void renderList(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t list = getRegU32(ctx, 4), entries = rd32(list), count = rd32(list + 4);
        const uint32_t base = pop_native_live::g_drawManager;
        // Z test off only for list +0x78; the alpha-only pass (render state 0x44) is list +0x98
        wr8(0x70000038u, list == base + 0x78 ? 0 : 1);
        wr32(0x70000044u, list == base + 0x98 ? 0xFF000000u : 0xFFFFFFFFu);
        for (uint32_t i = 0; i < count && i < 4096; ++i)
        {
            const uint32_t e = entries + 16 * i;
            uint64_t word;
            std::memcpy(&word, getConstMemPtr(g_ram.data(), e), 8);
            const uint32_t type = *getConstMemPtr(g_ram.data(), e + 6), kind = (type & 0x3F) >> 2;
            static const bool kinds = std::getenv("POP_STATE_KINDS") != nullptr; // debug: every list entry
            if (kinds)
            {
                uint64_t w0;
                std::memcpy(&w0, getConstMemPtr(g_ram.data(), e), 8);
                std::fprintf(stderr, "[state] list +0x%x entry %u: kind %u word %016llx matrix %u\n", list - base, i, kind, (unsigned long long)w0,
                             uint32_t((w0 & 0x3FFFFFFFFFull) >> 28));
            }
            uint16_t h4, h6;
            std::memcpy(&h4, getConstMemPtr(g_ram.data(), e + 4), 2);
            std::memcpy(&h6, getConstMemPtr(g_ram.data(), e + 6), 2);
            const uint32_t zone = (h4 & 0x3FF) >> 6, material = rd32(e + 8);
            const uint32_t subIndex = *getConstMemPtr(g_ram.data(), e + 0xC), pass = *getConstMemPtr(g_ram.data(), e + 0xD) & 0xF;
            const uint32_t matrix = uint32_t((word & 0x3FFFFFFFFFull) >> 28);
            // the sub-material: material +0x10, linked by +0x20, subIndex steps
            uint32_t sub = material ? rd32(material + 0x10) : 0;
            for (uint32_t k = 0; sub && k != subIndex && k < 64; ++k)
                sub = rd32(sub + 0x20);
            if (kind >= 4 && kind <= 0xB && kind != 6)
            {
                wr32(0x70000034u, sub);
                wr32(0x70000030u, material);
                wr32(0x70000084u, zone);
                // DoChangeFogColor: the zone colour (table 0x70000470); SetBlending sends it as FOGCOL
                // except for blend kinds 4, 5, 6 and 0xF (0)
                const uint32_t colour = rd32(0x70000470u + zone * 4), blend = sub ? (rd32(sub + 4) >> 16) & 0xF : 0;
                wr32(0x70000024u, (blend == 4 || blend == 5 || blend == 6 || blend == 0xF) ? 0 : colour);
                // the character light set (SetSkinDriver: entry bits 42-49 -> 0x70000088)
                if (kind == 9 || kind == 10)
                    wr8(0x70000088u, uint8_t((word & 0x3FFFFFFFFFFFFull) >> 42));
            }
            switch (kind)
            {
            case 4: // CJIdxMesh: object table *(0x6DF870) + u16 * 4
                setArgs(*ctx, rd32(0x006DF870u) + uint32_t(word & 0xFFFF) * 4, matrix);
                call(0x004BDB80u, ctx);
                break;
            case 5: // CJSmWaterElement
                setArgs(*ctx, uint32_t(word) * 16);
                call(0x004C77C0u, ctx);
                break;
            case 6:
                setArgs(*ctx, e);
                call(0x00469C00u, ctx);
                break;
            case 7:
            case 8: // CJadeSpriteElement: element table *(0x6E1598) + u16 * 0x20 (CElementList::Render)
                setArgs(*ctx, rd32(0x006E1598u) + uint32_t(word & 0xFFFF) * 0x20, matrix, kind == 8);
                call(0x004C6200u, ctx);
                break;
            case 9: // CJMeshElement::Display(element, matrix index, pass)
                setArgs(*ctx, uint32_t(word) * 16, matrix, pass);
                call(0x004C8A80u, ctx);
                break;
            case 10: // CJMeshElementRef::Display
                setArgs(*ctx, uint32_t(word) * 16, matrix, pass);
                call(0x004C7B40u, ctx);
                break;
            case 11:
                break;
            default: // GFX (FUN_00458FC0, entry pointer; the world mode does not hook it: replayed directly)
                setArgs(*ctx, e);
                if (g_hooks.count(0x00458FC0u))
                    call(0x00458FC0u, ctx);
                else
                    gfxEntry(nullptr, ctx, nullptr);
                break;
            }
        }
    }

    // CLMElementList::Render (0x4B7B80) over the lightmap list: 8-byte entries (element; kind in the
    // low nibble of the next word, 9 mesh, 10 instanced; matrix index (u16 +4 & 0x3FFF) >> 4; lightmap
    // texture index (u16 +6 & 0xFFF) >> 2; fog zone bits 14-17 + 0x10), each drawn as pass 3 with the
    // render flags 0x30284 that FUN_004B2000 leaves at 0x70000000 (texture index at 0x70000008).
    uint64_t g_lightmapRefs = 0;
    void renderLightmaps(uint32_t list, R5900Context *ctx)
    {
        const uint32_t entries = rd32(list), count = rd32(list + 4);
        pop_native_live::g_currentList = list - pop_native_live::g_drawManager;
        wr8(0x70000038u, 1);
        wr32(0x70000044u, 0xFFFFFFFFu);
        for (uint32_t i = 0; i < count && i < 4096; ++i)
        {
            const uint32_t element = rd32(entries + 8 * i), w1 = rd32(entries + 8 * i + 4);
            const uint32_t kind = w1 & 0xF, matrix = (w1 & 0x3FFF) >> 4, texture = ((w1 >> 16) & 0xFFF) >> 2;
            wr32(0x70000000u, 0x30284u);
            wr32(0x70000008u, texture);
            wr32(0x70000084u, ((w1 >> 14) & 0xF) + 0x10);
            setArgs(*ctx, element, matrix, 3);
            if (kind == 9)
                call(0x004C8A80u, ctx);
            else if (kind == 10)
                ++g_lightmapRefs; // instanced lightmaps: not drawn yet
        }
    }

    // FUN_00458FC0 (CElementList::Render's default case, GFX): render state from the entry as it sets
    // it (fog zone, 0x70000090 = entry bits 28-37, sub-material, material), then FUN_004584C0 on the
    // element (low word << 4), whose hook draws the particles
    void gfxEntry(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t e = getRegU32(ctx, 4);
        uint64_t word;
        std::memcpy(&word, getConstMemPtr(g_ram.data(), e), 8);
        uint16_t h4;
        std::memcpy(&h4, getConstMemPtr(g_ram.data(), e + 4), 2);
        const uint32_t material = rd32(e + 8), subIndex = *getConstMemPtr(g_ram.data(), e + 0xC);
        uint32_t sub = material ? rd32(material + 0x10) : 0;
        for (uint32_t k = 0; sub && k != subIndex && k < 64; ++k)
            sub = rd32(sub + 0x20);
        const uint32_t zone = (h4 & 0x3FF) >> 6;
        wr32(0x70000084u, zone);
        wr32(0x70000090u, uint32_t((word << 26) >> 54));
        wr32(0x70000034u, sub);
        wr32(0x70000030u, material);
        const uint32_t colour = rd32(0x70000470u + zone * 4), blend = sub ? (rd32(sub + 4) >> 16) & 0xF : 0;
        wr32(0x70000024u, (blend == 4 || blend == 5 || blend == 6 || blend == 0xF) ? 0 : colour);
        R5900Context p{};
        setArgs(p, uint32_t(word) << 4);
        call(0x004584C0u, &p);
    }

    // CJadeShadowManager::DisplayAllShadows (0x4C02C0) after the lightmaps: manager at scratchpad 0x500
    // (projector pointers +0, count +0x100, active mask +0x110); per active projector FUN_004C2400 calls
    // FUN_004C3540 (render-to-texture: casters into the shadow buffer, then the receivers) when the
    // strength +0x45C x fade +0x120 x FUN_004C2C40 (a fade by fog zone, not replayed: 1) is > 0, +0x104 is
    // clear (else a mask texture may replace the path) and both lists (counts +4, +0x14) are filled with
    // +0x111 set. FUN_004C2400 multiplies +0x45C for the call and restores it; so does this replay.
    uint64_t g_shadowProjectors = 0;
    void displayShadows(R5900Context *ctx)
    {
        const uint32_t n = rd32(0x70000600u), mask = rd32(0x70000610u);
        for (uint32_t i = 0; i < n && i < 32; ++i)
        {
            const uint32_t p = rd32(0x70000500u + 4 * i);
            if (!(mask >> i & 1) || !p || *getConstMemPtr(g_ram.data(), p + 0x104) || !rd32(p + 4) || !rd32(p + 0x14) ||
                !*getConstMemPtr(g_ram.data(), p + 0x111))
                continue;
            const uint32_t base = rd32(p + 0x45C);
            float strength, fade;
            std::memcpy(&strength, &base, 4);
            const uint32_t fb = rd32(p + 0x120);
            std::memcpy(&fade, &fb, 4);
            strength *= fade;
            if (strength <= 0)
                continue;
            uint32_t bits;
            std::memcpy(&bits, &strength, 4);
            wr32(p + 0x45C, bits);
            setArgs(*ctx, p);
            call(0x004C3540u, ctx);
            wr32(p + 0x45C, base);
            ++g_shadowProjectors;
        }
    }

    // Display3DElements (0x4BC000): the lists in the game's order, the shadows after the lightmaps
    void display3DElements(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t dm = getRegU32(ctx, 4);
        R5900Context sub{};
        for (uint32_t off : {200u, 0x2Cu, 0x3Cu, 0x68u, 0x78u, 0x4Cu, 0x5Cu, 0x88u, 0x98u, 0xA8u})
        {
            if (off == 0x5C)
            {
                renderLightmaps(dm + off, &sub);
                displayShadows(&sub);
                continue;
            }
            setArgs(sub, dm + off);
            call(0x004B9440u, &sub); // listRender hook -> renderList
        }
        // the after-effect pass (FUN_004ABFC0 on CAfterEffect 0x6DF170, end of Display3DElements):
        // ColorDiffusion (glow) when bit 5 of +0x121 is set, its arguments from the object (+0x94,
        // +0x98, +0x9C, +0x90, +0x88, +0x8C, +0xA0 in a1..t3, +0xA4 on the stack); AE_ProcessSmartBlur
        // (haze) when bit 7 is set, reading the object itself
        constexpr uint32_t ae = 0x006DF170u;
        const uint8_t flags = *getConstMemPtr(g_ram.data(), ae + 0x121);
        if (flags & 0x20)
        {
            R5900Context g{};
            const uint32_t regs[8] = {ae, rd32(ae + 0x94), rd32(ae + 0x98), rd32(ae + 0x9C), rd32(ae + 0x90), rd32(ae + 0x88),
                                      rd32(ae + 0x8C), rd32(ae + 0xA0)};
            for (int r = 0; r < 8; ++r)
                g.r[4 + r] = _mm_set_epi32(0, 0, 0, int(regs[r]));
            g.r[29] = _mm_set_epi32(0, 0, 0, int(ae + 0xA4)); // the stack argument
            call(0x004AE100u, &g);
        }
        if (flags & 0x80)
        {
            R5900Context h{};
            setArgs(h, ae);
            call(0x004A93C0u, &h);
        }
    }

    bool loadState(const char *path)
    {
        std::ifstream f(path, std::ios::binary);
        uint8_t head[0x18];
        if (!f.read(reinterpret_cast<char *>(head), sizeof(head)) || std::memcmp(head + 0x14, "EMEM", 4) != 0)
            return false;
        return bool(f.read(reinterpret_cast<char *>(g_ram.data()), g_ram.size())) &&
               bool(f.read(reinterpret_cast<char *>(g_spr.data()), g_spr.size()));
    }
}

// --- what the live code needs from the runtime and the other overrides ---
PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    if (address == 0x004BC000u)
        return display3DElements;
    if (address == 0x004B9440u)
        return renderList;
    if (address == 0x00458FC0u)
        return gfxEntry;
    return noop;
}
bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    g_hooks[address] = func;
    return true;
}
// the live window's GS comparison picture (never used offline; the Mac runtime's GS has it)
bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &, uint32_t &, uint32_t &, uint32_t *, uint32_t *, bool *) const { return false; }
#if POP_LIVE_QUICKSAVE // the SotC runtime's save states (POP_NATIVE_FLASH_SAVE): nothing to save offline
void ps2x::savestate::requestQuickSave() {}
#endif
uint32_t popTextureKey(uint32_t index)
{
    const auto it = g_texKeys.find(index);
    return it == g_texKeys.end() ? 0u : it->second;
}
namespace pop_hdtex
{
    bool readEntry(const std::string &path, uint32_t key, std::vector<uint8_t> &out) { return pop_bigfile::readEntry(path, key, out); }
}
// POP_NATIVE_WORLD offline (--world): the frame's native draws as pop_native_world would get them
namespace
{
    std::vector<uint8_t> g_worldFrame;
    uint32_t g_worldFrameSeq = 0;
}
namespace pop_native_world
{
    bool active(const GSRegisters *) { return true; }
    void submitFrame(uint32_t seq, std::vector<uint8_t> &&pnf)
    {
        g_worldFrameSeq = seq;
        g_worldFrame = std::move(pnf);
    }
    bool meshSent(uint64_t key)
    {
        static std::unordered_set<uint64_t> sent;
        return !sent.insert(key).second;
    }
}
namespace pop_live_client
{
    void start(int) {}
    void submit(std::vector<uint8_t> &&) {}
    uint64_t connection() { return 0; }
    bool busy() { return false; }
    void waitIdle() {}
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: pop_state_frame <state> <out.pnf> [--bigfile PRINCE.BF] [--packindex packindex.txt] [--world]\n"
                             "  --world: <out.pnf> = only the draws POP_NATIVE_WORLD would draw natively\n");
        return 2;
    }
    std::string index = "../../analysis/packindex.txt";
    bool world = false;
    for (int i = 3; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--world"))
            world = true;
        else if (i + 1 < argc && !std::strcmp(argv[i], "--bigfile"))
            g_bigfile = argv[++i];
        else if (i + 1 < argc && !std::strcmp(argv[i], "--packindex"))
            index = argv[++i];
    }
    if (!loadState(argv[1]))
    {
        std::fprintf(stderr, "cannot read save state %s\n", argv[1]);
        return 1;
    }
    ps2SetScratchpadHostPtr(g_spr.data());

    // texture index -> key from the engine's texture list
    const uint32_t texCount = rd32(0x006CBC50u), texList = rd32(0x006CBC58u);
    for (uint32_t i = 0; i < texCount && i < 8192; ++i)
    {
        uint16_t ix;
        std::memcpy(&ix, getConstMemPtr(g_ram.data(), texList + 16 * i + 10), 2);
        if (const uint32_t key = rd32(texList + 16 * i))
            g_texKeys[ix] = key;
    }
    // the packs holding them: per key the first pack of the index, preferring packs already chosen
    std::unordered_map<uint32_t, std::vector<uint32_t>> packsOf;
    {
        std::ifstream f(index);
        uint32_t t, p;
        while (f >> std::hex >> t >> p)
            packsOf[t].push_back(p);
    }
    std::set<uint32_t> packs;
    size_t missing = 0;
    for (const auto &[ix, key] : g_texKeys)
    {
        const auto it = packsOf.find(key);
        if (it == packsOf.end())
        {
            ++missing;
            continue;
        }
        bool have = false;
        for (uint32_t p : it->second)
            have = have || packs.count(p);
        if (!have)
            packs.insert(it->second.front());
    }
    std::fprintf(stderr, "[state] %zu texture keys, %zu packs, %zu keys not in the index (%s)\n", g_texKeys.size(), packs.size(), missing,
                 packsOf.empty() ? "no index" : index.c_str());

    const auto setEnv = [](const char *name, const std::string &value) {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    };
    setEnv("POP_NATIVE_LIVE", "1");
    if (world)
        setEnv("POP_NATIVE_WORLD", "1");
    else
        setEnv("POP_NATIVE_FRAME", std::string(argv[2]) + ":0");
    setEnv("POP_PS2_BIGFILE", g_bigfile);
    alignas(PS2Runtime) static unsigned char storage[sizeof(PS2Runtime)];
    PS2Runtime &runtime = *reinterpret_cast<PS2Runtime *>(storage);
    pop_native_live::install(runtime);
    for (uint32_t p : packs)
        pop_native_live::onBigfileLookup(p);
    for (int waited = 0;; ++waited)
    {
        size_t ready = 0;
        {
            std::lock_guard<std::mutex> lock(pop_native_live::g_packsMutex);
            for (uint32_t p : packs)
                ready += pop_native_live::g_packs.count(p) && pop_native_live::g_packs[p] ? 1 : 0;
        }
        if (ready == packs.size() || waited > 600)
        {
            std::fprintf(stderr, "[state] %zu of %zu packs loaded\n", ready, packs.size());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (world)
    {
        // one Display3DElements (marker, lists), then PS2_Flip's DMA kick (return address in PS2_Flip)
        R5900Context ctx{};
        setArgs(ctx, 0x700000C0u);
        call(0x004BC000u, &ctx);
        // what the GS still draws of the recorded draws: per (kind, list, blend kind) native / left, with triangles
        {
            using namespace pop_native_live;
            std::map<std::tuple<int, uint32_t, uint32_t>, std::array<size_t, 4>> by; // native, left, native tris, left tris
            auto add = [&](int kind, const DrawMaterial &m, const std::vector<pop_assets::Batch> &bs) {
                size_t tris = 0;
                for (const auto &b : bs)
                    tris += b.indices.size() / 3;
                auto &e = by[{kind, m.list, (m.flags >> 16) & 0xF}];
                e[m.native ? 0 : 1] += 1;
                e[m.native ? 2 : 3] += tris;
            };
            for (size_t i = 0; i < g_frameMaterials.size(); ++i)
                add(0, g_frameMaterials[i], g_elements[g_frameDraws[i].first].meshes);
            for (const auto &p : g_framePosed)
                add(1, p.second, p.first);
            for (const auto &r : g_frameRefs)
                add(2, r.second.second, r.first);
            for (const auto &[k, v] : by)
                std::fprintf(stderr, "[state] %s list +0x%x blend %u: native %zu (%zu tris), GS %zu (%zu tris)\n",
                             std::get<0>(k) == 0 ? "static" : std::get<0>(k) == 1 ? "skinned" : "instanced", std::get<1>(k), std::get<2>(k), v[0],
                             v[2], v[1], v[3]);
        }
        ctx.r[31] = _mm_set_epi32(0, 0, 0, 0x004A2100);
        call(0x004A2C80u, &ctx);
        if (g_worldFrame.empty())
        {
            std::fprintf(stderr, "[state] no world frame\n");
            return 1;
        }
        std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char *>(g_worldFrame.data()), std::streamsize(g_worldFrame.size()));
        // the marker as the DMA list now holds it (graphics object +0xEC moved past it)
        const uint32_t cursor = rd32(0x70000620u + 0xECu) - 0x30u;
        const size_t firstBytes = g_worldFrame.size();
        // the same frame again as the next one would be written: batches sent before only by key
        // (<out>.2; pop_world_test draws it as the later frames)
        if (std::getenv("POP_STATE_WORLD_SECOND"))
        {
            setArgs(ctx, 0x700000C0u);
            call(0x004BC000u, &ctx);
            ctx.r[31] = _mm_set_epi32(0, 0, 0, 0x004A2100);
            call(0x004A2C80u, &ctx);
            std::ofstream(std::string(argv[2]) + ".2", std::ios::binary)
                .write(reinterpret_cast<const char *>(g_worldFrame.data()), std::streamsize(g_worldFrame.size()));
            std::fprintf(stderr, "[state] second world frame %u: %zu bytes\n", g_worldFrameSeq, g_worldFrame.size());
        }
        std::fprintf(stderr, "[state] world frame %u: %zu bytes; marker at %08x: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x\n",
                     1u, firstBytes, cursor, rd32(cursor), rd32(cursor + 4), rd32(cursor + 8), rd32(cursor + 12),
                     rd32(cursor + 16), rd32(cursor + 20), rd32(cursor + 24), rd32(cursor + 28), rd32(cursor + 32), rd32(cursor + 36),
                     rd32(cursor + 40), rd32(cursor + 44));
        const auto &ws = pop_native_live::g_worldStats;
        std::fprintf(stderr, "[state] world calls: native %llu, kept for textures %llu, not decoded %llu; skinned draws native %llu, "
                             "driver switches replayed %llu, skinned calls on the GS (state/not decoded/texture) %llu/%llu/%llu\n",
                     (unsigned long long)ws.skipped, (unsigned long long)ws.noTexture, (unsigned long long)ws.notRecorded,
                     (unsigned long long)ws.posed, (unsigned long long)ws.drivers, (unsigned long long)ws.skinGs[0],
                     (unsigned long long)ws.skinGs[1], (unsigned long long)ws.skinGs[2]);
        const auto &ls = pop_native_live::g_lateStats;
        std::string by;
        for (const auto &[why, n] : ls.closedBy)
            by += " " + why + " " + std::to_string(n) + ";";
        std::fprintf(stderr, "[state] late lists: pure at +0x88 %llu, ended within %llu, calls native %llu, GS %llu, particles native %llu; ended by:%s\n",
                     (unsigned long long)ls.pureAtLate, (unsigned long long)ls.closedInLate, (unsigned long long)ls.native,
                     (unsigned long long)ls.gs, (unsigned long long)ws.particles, by.empty() ? " -" : by.c_str());
        return 0;
    }
    // two Display3DElements calls: the first starts the capture and replays the lists, the second writes
    R5900Context ctx{};
    setArgs(ctx, 0x700000C0u);
    call(0x004BC000u, &ctx);
    setArgs(ctx, 0x700000C0u);
    call(0x004BC000u, &ctx);
    std::fprintf(stderr, "[state] lightmap draws %llu, from packets %llu, instanced lightmaps skipped %llu\n",
                 (unsigned long long)pop_native_live::g_stats.drawsLightmap, (unsigned long long)pop_native_live::g_stats.drawsFromPacket,
                 (unsigned long long)g_lightmapRefs);
    std::fprintf(stderr, "[state] lit static passes native: lit %llu, env %llu\n", (unsigned long long)pop_native_live::g_worldStats.litNative,
                 (unsigned long long)pop_native_live::g_worldStats.envNative);
    return 0;
}
