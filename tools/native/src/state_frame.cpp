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
// 0x84 and FOGCOL 0x24 as SetBlending sends it, Z test 0x38, alpha-only mask 0x44), and calls the
// same hooks the game would. Elements were not converted at load (no ReadElement), so the live code
// decodes them from their pass packets. Textures: index -> key from the engine's texture list
// (TEX_List: count 0x6CBC50, entries 0x6CBC58, 16 bytes: key, ..., +10 index), the packs holding them
// from tools/packindex.py's index.
// Not replayed yet: instanced lightmaps, shadows, the glow and haze
// (their hooks run from the after-effect code), skinned characters (converted only at load).
// Save-state layout: tools/popstate.py.

#include "../../overrides/pop_native_live.cpp"
#include "bigfile.h"

#include <cstdio>
#include <fstream>
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
            case 8: // CJadeSpriteElement
                setArgs(*ctx, uint32_t(word) * 16, matrix, kind == 8);
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
            default: // GFX (FUN_00458FC0, entry pointer)
                setArgs(*ctx, e);
                call(0x00458FC0u, ctx);
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

    // Display3DElements (0x4BC000): the lists in the game's order; shadows are not replayed
    void display3DElements(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t dm = getRegU32(ctx, 4);
        R5900Context sub{};
        for (uint32_t off : {200u, 0x2Cu, 0x3Cu, 0x68u, 0x78u, 0x4Cu, 0x5Cu, 0x88u, 0x98u, 0xA8u})
        {
            if (off == 0x5C)
            {
                renderLightmaps(dm + off, &sub);
                continue;
            }
            setArgs(sub, dm + off);
            call(0x004B9440u, &sub); // listRender hook -> renderList
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
    return noop;
}
bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    g_hooks[address] = func;
    return true;
}
// the live window's GS comparison picture (never used offline; the Mac runtime's GS has it)
bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &, uint32_t &, uint32_t &, uint32_t *, uint32_t *, bool *) const { return false; }
uint32_t popTextureKey(uint32_t index)
{
    const auto it = g_texKeys.find(index);
    return it == g_texKeys.end() ? 0u : it->second;
}
namespace pop_hdtex
{
    bool readEntry(const std::string &path, uint32_t key, std::vector<uint8_t> &out) { return pop_bigfile::readEntry(path, key, out); }
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
        std::fprintf(stderr, "usage: pop_state_frame <state> <out.pnf> [--bigfile PRINCE.BF] [--packindex packindex.txt]\n");
        return 2;
    }
    std::string index = "../../analysis/packindex.txt";
    for (int i = 3; i + 1 < argc; i += 2)
    {
        if (!std::strcmp(argv[i], "--bigfile"))
            g_bigfile = argv[i + 1];
        else if (!std::strcmp(argv[i], "--packindex"))
            index = argv[i + 1];
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

    setenv("POP_NATIVE_LIVE", "1", 1);
    setenv("POP_NATIVE_FRAME", (std::string(argv[2]) + ":0").c_str(), 1);
    setenv("POP_PS2_BIGFILE", g_bigfile.c_str(), 1);
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

    // two Display3DElements calls: the first starts the capture and replays the lists, the second writes
    R5900Context ctx{};
    setArgs(ctx, 0x700000C0u);
    call(0x004BC000u, &ctx);
    setArgs(ctx, 0x700000C0u);
    call(0x004BC000u, &ctx);
    std::fprintf(stderr, "[state] lightmap draws %llu, from packets %llu, instanced lightmaps skipped %llu\n",
                 (unsigned long long)pop_native_live::g_stats.drawsLightmap, (unsigned long long)pop_native_live::g_stats.drawsFromPacket,
                 (unsigned long long)g_lightmapRefs);
    return 0;
}
