// Native renderer, live side (Stage B steps 2-3), POP_NATIVE_LIVE=1. Converts every static mesh element
// as the game loads it, with the C++ asset conversion (tools/native/src/assets.*, the same code the
// offline viewer uses), and checks each element the game then draws against its conversion: every
// position the draw's DMA packet uploads (UNPACK V3-32) must be one of the converted vertices.
// Prints "[pop:native]" statistics every 5 s. Nothing is drawn natively yet.
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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pop_native_live
{
    namespace
    {
        constexpr uint32_t kReadElement = 0x004C9500u;
        constexpr uint32_t kMeshDisplay = 0x004C8A80u;
        constexpr size_t kRam = 32u << 20;

        PS2Runtime::RecompiledFunction g_origRead = nullptr, g_origDisplay = nullptr;

        // Per converted element: its vertex positions (exact float bits), for the draw check.
        struct Converted
        {
            std::unordered_set<uint64_t> positions; // hash of (x, y, z) bits
            size_t batches = 0, vertices = 0, triangles = 0;
        };
        std::unordered_map<uint32_t, Converted> g_elements; // element object address -> conversion
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
            const uint32_t pass = getRegU32(ctx, 6) & 0xFF;
            uint32_t element = getRegU32(ctx, 4);
            for (unsigned link = 0; element && link < 4096; ++link)
            {
                ++g_stats.draws;
                const auto it = g_elements.find(element);
                if (it == g_elements.end())
                {
                    ++g_stats.drawsUnknown;
                    const auto sk = g_skipped.find(element);
                    ++g_unknownBy[sk == g_skipped.end() ? '?' : sk->second];
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
                }
                element = guest32(rdram, element + 0x4C);
            }
            report();
            g_origDisplay(rdram, ctx, runtime);
        }
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
        g_lastReport = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[pop:native] live conversion check on\n");
    }
}
