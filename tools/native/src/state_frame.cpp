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
// Not replayed yet: halo lens-flare ghosts; shadows only on the render-to-texture path (POP_NATIVE_SHADOWS=1).
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

    // census (step 3c): Display passes per (element, matrix index) for kinds 9 / 10, as bits 1 << pass
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_passesOf;

    void noop(uint8_t *, R5900Context *, PS2Runtime *) {}
    void gfxEntry(uint8_t *, R5900Context *ctx, PS2Runtime *);

    // The save's last kicked VIF1 chain (as tools/statechain.py): +0xF4 of the graphics object is the buffer being built,
    // the other one was kicked. f(tag address, tag id, qwc, data address, address after the record) per record.
    template <class F> void walkLastChain(F f)
    {
        const uint32_t g = 0x70000620u;
        uint32_t addr = rd32(g + 0xE4 + 4 * (rd32(g + 0xF4) ^ 1));
        std::vector<uint32_t> stack;
        for (int rec = 0; rec < 200000; ++rec)
        {
            const uint32_t t0 = rd32(addr), qwc = t0 & 0xFFFF, id = (t0 >> 28) & 7, ta = rd32(addr + 4) & 0x7FFFFFF0u;
            const bool refType = id == 0 || id == 3 || id == 4;
            const uint32_t after = addr + 16 + (refType ? 0 : qwc * 16);
            f(addr, id, qwc, refType ? ta : addr + 16, after);
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
    }

    // Step 4c (docs/gs_off.md): the save's matrix pools run a frame ahead of its last chain, so characters are posed with
    // the bones that chain uploaded (VU1 data: VU 0 the single-bone / object matrix, VU 0x388 + 4k bone k). Per CALL
    // target (the draw's packet) the VU1 matrices in effect at the CALL; a draw takes the CALL whose bones lie nearest to
    // the pool's (instances of one packet are metres apart, a frame of motion centimetres).
    struct ChainPose
    {
        std::vector<std::array<float, 16>> bones; // [0] = VU 0, [1 + k] = VU 0x388 + 4k
    };
    std::unordered_map<uint32_t, std::vector<ChainPose>> g_chainPoses;
    size_t g_chainPosed = 0, g_chainKept = 0;
    void findChainPoses()
    {
        std::vector<std::array<float, 4>> vu(1024, std::array<float, 4>{});
        size_t calls = 0;
        walkLastChain([&](uint32_t tag, uint32_t id, uint32_t qwc, uint32_t data, uint32_t) {
            // VIF stream of the record: the tag's upper two words (TTE), then its data
            std::vector<uint32_t> w{rd32(tag + 8), rd32(tag + 12)};
            for (uint32_t k = 0; k < qwc * 4; ++k)
                w.push_back(rd32(data + 4 * k));
            for (size_t i = 0; i < w.size();)
            {
                const uint32_t c = w[i++], cmd = (c >> 24) & 0x7F, num = (c >> 16) & 0xFF, imm = c & 0xFFFF;
                if (cmd >= 0x60)
                {
                    const uint32_t n = num ? num : 256, vl = cmd & 3, vn = (cmd >> 2) & 3;
                    const uint32_t bits = vl == 3 ? 16 : (32u >> vl) * (vn + 1);
                    const size_t words = (size_t(n) * bits + 31) / 32;
                    if ((cmd & 0xF) == 0xC && !(imm & 0x8000)) // V4-32, not relative to TOPS: matrices, constants
                        for (uint32_t q = 0; q < n && i + 4 * q + 3 < w.size(); ++q)
                            std::memcpy(vu[(imm + q) & 0x3FF].data(), &w[i + 4 * q], 16);
                    i += words;
                }
                else if (cmd == 0x4A)
                    i += 2 * size_t(num ? num : 256);
                else if (cmd == 0x50 || cmd == 0x51)
                    i += 4 * size_t(imm ? imm : 65536);
                else if (cmd == 0x20)
                    i += 1;
                else if (cmd == 0x30 || cmd == 0x31)
                    i += 4;
            }
            if (id != 5)
                return;
            ++calls;
            ChainPose p;
            const auto mat = [&](uint32_t q) {
                std::array<float, 16> m;
                for (int r = 0; r < 4; ++r)
                    std::memcpy(&m[4 * r], vu[q + r].data(), 16);
                return m;
            };
            p.bones.push_back(mat(0));
            for (uint32_t q = 0x388; q + 3 < 1024; q += 4)
                p.bones.push_back(mat(q));
            g_chainPoses[rd32(tag + 4) & 0x01FFFFF0u].push_back(std::move(p));
        });
        std::fprintf(stderr, "[state] chain poses: %zu calls, %zu packets\n", calls, g_chainPoses.size());
    }
    void chainBones(uint32_t packet, std::vector<std::array<float, 16>> &bones)
    {
        const auto it = g_chainPoses.find(packet);
        const size_t n = bones.size();
        if (it == g_chainPoses.end() || n > 30) // 30 bones fill VU 0x388..0x3FF
        {
            ++g_chainKept;
            return;
        }
        const ChainPose *best = nullptr;
        float bestD = 0;
        for (const ChainPose &p : it->second)
        {
            float d = 0;
            for (size_t k = 0; k < n; ++k)
            {
                const auto &c = p.bones[n < 2 ? 0 : 1 + k];
                d += std::hypot(c[12] - bones[k][12], c[13] - bones[k][13], c[14] - bones[k][14]);
            }
            if (!best || d < bestD)
                best = &p, bestD = d;
        }
        // shortcut: 0.5 m mean bone distance tells another instance from this one's last frame; tune if fast moves miss
        if (!best || bestD / float(n) > 0.5f)
        {
            ++g_chainKept;
            return;
        }
        for (size_t k = 0; k < n; ++k)
            bones[k] = best->bones[n < 2 ? 0 : 1 + k];
        ++g_chainPosed;
    }

#if POP_SPRITE_VU1
    // GFX particle packets in the save's last VIF1 chain (as tools/statechain.py walks it): each run of SetParticleEnv
    // records (CNT, UNPACK V4-32 0x17 qw at 0) and CGraphicParticle batches (CNT, STCYCL 4/4, FLUSHA, ..., positions at
    // TOPS) is one FUN_004584C0 call (a call starts a new pair only after one at its cap: GIF count 200 billboard /
    // 150 oriented); the frame's GFX draws take them in order (POP_NATIVE_P5)
    std::vector<std::pair<uint32_t, uint32_t>> g_particleRanges;
    size_t g_particleNext = 0;
    void findParticlePackets()
    {
        uint32_t start = 0, end = 0, lastCount = 0;
        walkLastChain([&](uint32_t addr, uint32_t id, uint32_t, uint32_t, uint32_t after) {
            const bool cnt = id == 1;
            const bool env = cnt && rd32(addr + 12) == 0x6C170000u;
            const bool batch = cnt && rd32(addr + 8) == 0x01000404u && rd32(addr + 16) == 0x13000000u && (rd32(addr + 32) & 0xFB00FFFFu) == 0x68008000u;
            if (env && start && lastCount != 200 && lastCount != 150)
            {
                g_particleRanges.emplace_back(start, end); // the next call's env
                start = 0;
            }
            if (env)
                lastCount = rd32(addr + 16 + 16 * 16) & 0x7FFF; // the GIF tag at VU qw 16
            if (env || batch)
            {
                if (!start)
                    start = addr;
                end = after;
            }
            else if (start)
            {
                g_particleRanges.emplace_back(start, end);
                start = 0;
            }
        });
        if (start)
            g_particleRanges.emplace_back(start, end);
        std::fprintf(stderr, "[state] particle packets in the last chain: %zu draws\n", g_particleRanges.size());
    }
    uint32_t particlePackets(uint32_t, uint32_t &end)
    {
        if (g_particleNext >= g_particleRanges.size())
            return 0;
        end = g_particleRanges[g_particleNext].second;
        return g_particleRanges[g_particleNext++].first;
    }
#endif

    // CElementList::Render (0x4B9440) over one list: entries of 16 bytes at *list, count at list + 4.
    void renderList(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t list = getRegU32(ctx, 4), entries = rd32(list), count = rd32(list + 4);
        const uint32_t base = pop_native_live::g_drawManager;
        // Z test off for list +0x78 and the 2D pass (+0xB8, after Before2D's SetZTest 0); the alpha-only pass (render state 0x44) is list +0x98
        wr8(0x70000038u, list == base + 0x78 || list == base + 0xB8 ? 0 : 1);
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
                // the UV mode FUN_004BA5C0 sets (2 = this sub's UV matrix, FUN_004B5EC0) and the sub that call records
                wr32(0x70000080u, sub && rd32(sub + 8) ? 2u : 1u);
                pop_native_live::g_uvSub = sub;
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
            case 8: // CJadeSpriteElement: element table *(0x6E1598) + u16 * 0x20 (CElementList::Render, which also sets 0x90)
                wr32(0x70000090u, uint32_t((word << 26) >> 54));
                setArgs(*ctx, rd32(0x006E1598u) + uint32_t(word & 0xFFFF) * 0x20, matrix, kind == 8);
                call(0x004C6200u, ctx);
                break;
            case 9: // CJMeshElement::Display(element, matrix index, pass)
                g_passesOf[{uint32_t(word) * 16, matrix}] |= 1u << pass;
                setArgs(*ctx, uint32_t(word) * 16, matrix, pass);
                call(0x004C8A80u, ctx);
                break;
            case 10: // CJMeshElementRef::Display
                g_passesOf[{uint32_t(word) * 16, matrix}] |= 1u << pass;
                setArgs(*ctx, uint32_t(word) * 16, matrix, pass);
                call(0x004C7B40u, ctx);
                break;
            case 11: // FUN_004B4580(x, y, width, height, colour, mode) from the table 0x6DF6E0 (0x18 bytes per rectangle)
            {
                const uint32_t t = 0x006DF6E0u + uint32_t(word & 0xFFFF) * 0x18;
                std::memcpy(&ctx->f[12], getConstMemPtr(g_ram.data(), t), 16);
                setArgs(*ctx, rd32(t + 0x10), rd32(t + 0x14));
                call(0x004B4580u, ctx);
                break;
            }
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
            wr32(0x70000080u, 1u); // CLMElementList::Render sets its own UV mode (FUN_004B7500): no UV matrix
            wr32(0x70000084u, ((w1 >> 14) & 0xF) + 0x10);
            setArgs(*ctx, element, matrix, 3);
            if (kind == 9)
                call(0x004C8A80u, ctx);
            else if (kind == 10)
            {
                ++g_lightmapRefs;
                call(0x004C7B40u, ctx);
            }
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
        static const char *skip = std::getenv("POP_STATE_SKIP_LIST"); // debug: leave out one list (hex offset, e.g. 88)
        for (uint32_t off : {200u, 0x2Cu, 0x3Cu, 0x68u, 0x78u, 0x4Cu, 0x5Cu, 0x88u, 0x98u, 0xA8u})
        {
            if (skip && std::strtoul(skip, nullptr, 16) == off)
                continue;
            if (off == 0x5C)
            {
                renderLightmaps(dm + off, &sub);
                displayShadows(&sub);
                continue;
            }
            setArgs(sub, dm + off);
            call(0x004B9440u, &sub); // listRender hook -> renderList
        }
        // the halos (FUN_004ABFC0 -> FUN_004AA240 per entry): the pass zeroes the count after drawing, so a save holds the
        // last frame's entries with count 0 and, after them, stale ones from earlier frames with more halos (162621: the sun
        // at 0, 22 and 27). shortcut: entries up to the first without a material, skipping any within 0.5 of an earlier one;
        // a stale halo that moved further still gets drawn
        std::vector<std::array<float, 3>> drawn;
        for (uint32_t e = 0x006DF2C0u; e < 0x006DF2C0u + 32 * 0x20 && rd32(e + 0x1C); e += 0x20)
        {
            std::array<float, 3> p;
            for (int k = 0; k < 3; ++k)
                p[k] = pop_native_live::guestF(g_ram.data(), e + 4 * k);
            if (std::any_of(drawn.begin(), drawn.end(), [&](const std::array<float, 3> &q) {
                    return std::fabs(p[0] - q[0]) < 0.5f && std::fabs(p[1] - q[1]) < 0.5f && std::fabs(p[2] - q[2]) < 0.5f;
                }))
                continue;
            drawn.push_back(p);
            setArgs(sub, 0x006DF170u, e);
            call(0x004AA240u, &sub);
        }
        // the after-effect pass (FUN_004ABFC0 on CAfterEffect 0x6DF170, end of Display3DElements):
        // ColorDiffusion (glow) when bit 5 of +0x121 is set, its arguments from the object (+0x94,
        // +0x98, +0x9C, +0x90, +0x88, +0x8C, +0xA0 in a1..t3, +0xA4 on the stack); AE_ProcessSmartBlur
        // (haze) when bit 7 is set, reading the object itself
        constexpr uint32_t ae = 0x006DF170u;
        // ZoomSmooth 0 (0x4AD900: this, mode 0, f12 = +0x18) when +0x120 bit 7 is set, first in the pass;
        // POP_STATE_ZOOMSMOOTH=<strength> forces it (no save has the power of haste running yet)
        if (const char *z = std::getenv("POP_STATE_ZOOMSMOOTH"))
        {
            const float p = float(std::atof(z));
            std::memcpy(getMemPtr(g_ram.data(), ae + 0x18), &p, 4);
            *getMemPtr(g_ram.data(), ae + 0x120) |= 0x80;
        }
        if (*getConstMemPtr(g_ram.data(), ae + 0x120) & 0x80)
        {
            R5900Context z{};
            setArgs(z, ae, 0);
            std::memcpy(&z.f[12], getConstMemPtr(g_ram.data(), ae + 0x18), 4);
            call(0x004AD900u, &z);
        }
        const uint8_t flags = *getConstMemPtr(g_ram.data(), ae + 0x121);
        if (flags & 0x10) // PinchEffect (FUN_004AEA00), before the glow; its hook reads the object's fields
        {
            setArgs(sub, ae);
            call(0x004AEA00u, &sub);
        }
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
        // the 2D pass: Before2D's (FUN_004B4A80) letterbox bands, FUN_004B4580(0, 0, 1, band) and (0, 1 - band, 1, band) in
        // black, then PS2_After2D's list +0xB8 (FUN_004BBFC0, with draw manager +0x28 set: rectangles draw, not queue)
        wr8(dm + 0x28, 1);
        const float band = pop_native_live::guestF(g_ram.data(), rd32(0x00674B00u) + 0xEA0u);
        for (int k = 0; band > 0.0f && k < 2; ++k)
        {
            R5900Context r{};
            const float f[4] = {0.0f, k ? 1.0f - band : 0.0f, 1.0f, band};
            std::memcpy(&r.f[12], f, sizeof f);
            setArgs(r, 0, 0);
            call(0x004B4580u, &r);
        }
        setArgs(sub, dm + 0xB8);
        call(0x004B9440u, &sub);
        wr8(dm + 0x28, 0);
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
#if POP_SPRITE_VU1 // pop_state_frame_vu1: the interpreter's XGKICK targets (the sprite path's sink keeps everything)
void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool) {}
void GS::processGIFPacket(const uint8_t *, uint32_t) {}
#endif
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
    void submitFrame(uint32_t seq, uint32_t, std::vector<uint8_t> &&pnf)
    {
        g_worldFrameSeq = seq;
        g_worldFrame = std::move(pnf);
    }
    uint32_t sentGeneration() { return 1; }
    void noteFrame(uint32_t, std::string &&) {}
    bool meshSent(uint64_t key, uint32_t)
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
    pop_mesh::g_keepAlpha = std::getenv("POP_ALPHA_CENSUS") != nullptr;

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
    if (const char *e = std::getenv("POP_CHAIN_POSES"); !e || *e != '0') // step 4c: on by default offline
    {
        findChainPoses();
        pop_native_live::g_chainBones = chainBones;
    }
#if POP_SPRITE_VU1
    if (const char *e = std::getenv("POP_NATIVE_P5"); !e || *e != '0') // on by default, as live
    {
        findParticlePackets();
        pop_native_live::g_particlePackets = particlePackets;
    }
#endif
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
        // census (POP_ALPHA_CENSUS=1, docs/renderer_audit.md items 9 and the displayRecord question): per (kind,
        // list, blend kind) draws native / GS, and of those with source texel alpha > 0x80 or vertex alpha > 0x80
        // (vertex alpha only where no local alpha replaces it); shadow textures are synthetic: not counted
        if (pop_mesh::g_keepAlpha)
        {
            using namespace pop_native_live;
            std::map<std::tuple<int, uint32_t, uint32_t, uint32_t>, std::array<size_t, 6>> by; // native, GS; texture over n/G; vertex over n/G
            std::unordered_map<uint32_t, uint32_t> overOf; // texture key -> texels over 0x80
            size_t notFound = 0;
            auto add = [&](int kind, const DrawMaterial &m, const std::vector<pop_assets::Batch> &bs) {
                const int g = m.native ? 0 : 1;
                auto &e = by[{kind, m.list, (m.flags >> 16) & 0xF, (m.flags >> 4) & 1}];
                ++e[g];
                if (m.textureKey && kind != 3)
                {
                    auto it = overOf.find(m.textureKey);
                    if (it == overOf.end())
                    {
                        pop_native::Image img;
                        if (m.textureKey - kPaletteKey < 0x8000u && g_framePaletteImages.count(m.textureKey - kPaletteKey))
                            img = g_framePaletteImages[m.textureKey - kPaletteKey];
                        else if (!findTexture(m.textureKey, img))
                            ++notFound;
                        it = overOf.emplace(m.textureKey, img.alphaOver).first;
                    }
                    e[2 + g] += it->second != 0;
                }
                bool vover = false;
                for (const auto &b : bs)
                    for (const auto &v : b.vertices)
                        vover = vover || v.colour[3] > 1.0f;
                e[4 + g] += m.constAlpha < 0 && vover;
            };
            for (size_t i = 0; i < g_frameMaterials.size(); ++i)
                add(0, g_frameMaterials[i], g_elements[g_frameDraws[i].first].meshes);
            for (const auto &p : g_framePosed)
                add(1, p.second, p.first);
            for (const auto &r : g_frameRefs)
                add(2, r.second.second, r.first);
            for (const auto &r : g_frameShadows)
                add(3, r.second.second, r.first);
            static const char *const kinds[] = {"static", "skinned", "instanced", "shadow"};
            for (const auto &[k, v] : by)
                std::fprintf(stderr, "[census] %s list +0x%x blend %u test %u: draws native %zu GS %zu; texture alpha > 0x80 native %zu GS %zu; "
                                     "vertex alpha > 0x80 native %zu GS %zu\n",
                             kinds[std::get<0>(k)], std::get<1>(k), std::get<2>(k), std::get<3>(k), v[0], v[1], v[2], v[3], v[4], v[5]);
            size_t over = 0;
            for (const auto &[key, n] : overOf)
                over += n != 0;
            std::fprintf(stderr, "[census] textures %zu, not found %zu, with texels > 0x80 %zu\n", overOf.size(), notFound, over);
            size_t both = 0;
            for (const auto &[em, passes] : g_passesOf)
                if ((passes & 3) == 3)
                {
                    ++both;
                    std::fprintf(stderr, "[census] pass 0 and 1: element %08x matrix %u passes %x\n", em.first, em.second, passes);
                }
            std::map<uint32_t, size_t> masks;
            for (const auto &[em, passes] : g_passesOf)
                ++masks[passes];
            std::string ms;
            for (const auto &[m, n] : masks)
                ms += " " + std::to_string(m) + ":" + std::to_string(n);
            std::fprintf(stderr, "[census] (element, matrix) displayed %zu, with pass 0 and pass 1 %zu; pass masks%s\n", g_passesOf.size(), both,
                         ms.c_str());
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
#if POP_SPRITE_VU1
        const auto &ss = pop_native_live::g_spriteStats, &ps = pop_native_live::g_particleStats;
        std::fprintf(stderr, "[state] sprites: draws %llu, native %llu, quads %llu, not replayed %llu\n", (unsigned long long)ss.draws,
                     (unsigned long long)ss.native, (unsigned long long)ss.quads, (unsigned long long)ss.failed);
        std::fprintf(stderr, "[state] particles (P5): draws %llu, triangles %llu, not replayed %llu; VU runs %llu, to the interpreter %llu\n",
                     (unsigned long long)ps.draws, (unsigned long long)ps.quads, (unsigned long long)ps.failed,
                     (unsigned long long)pop_native_live::g_particleVu.runs, (unsigned long long)pop_native_live::g_particleVu.interpreted);
        const auto &wst = pop_native_live::g_waterStats;
        std::fprintf(stderr, "[state] water (P6): draws %llu, triangles %llu, not replayed %llu; VU runs %llu, to the interpreter %llu\n",
                     (unsigned long long)wst.draws, (unsigned long long)wst.quads, (unsigned long long)wst.failed,
                     (unsigned long long)pop_native_live::g_waterVu.runs, (unsigned long long)pop_native_live::g_waterVu.interpreted);
#endif
        const auto &ls = pop_native_live::g_lateStats;
        std::string by;
        for (const auto &[why, n] : ls.closedBy)
            by += " " + why + " " + std::to_string(n) + ";";
        const auto &td = pop_native_live::g_twoDStats;
        std::fprintf(stderr, "[state] 2D: rectangles %llu, letters %llu, strings %llu (not native %llu)\n", (unsigned long long)td.rects,
                     (unsigned long long)td.letters, (unsigned long long)td.strings, (unsigned long long)td.stringsGs);
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
    std::fprintf(stderr, "[state] lightmap draws %llu, from packets %llu, instanced lightmaps %llu\n",
                 (unsigned long long)pop_native_live::g_stats.drawsLightmap, (unsigned long long)pop_native_live::g_stats.drawsFromPacket,
                 (unsigned long long)g_lightmapRefs);
    std::fprintf(stderr, "[state] skinned draws posed from the chain: %zu, kept on the pool: %zu\n", g_chainPosed, g_chainKept);
    std::fprintf(stderr, "[state] lit static passes native: lit %llu, env %llu\n", (unsigned long long)pop_native_live::g_worldStats.litNative,
                 (unsigned long long)pop_native_live::g_worldStats.envNative);
    return 0;
}
