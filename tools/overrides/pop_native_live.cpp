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

#include <array>
#include <chrono>
#include <functional>
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
        constexpr size_t kRam = 32u << 20;

        PS2Runtime::RecompiledFunction g_origRead = nullptr, g_origDisplay = nullptr, g_origDisplay3D = nullptr;

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
            uint32_t mode = 0;   // pop_native::AlphaMode
            float cutoff = 0.5f;
        };
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
        // Display): word 0 = texture index, word 1 = the record's +0x1C flags (0x10: alpha test),
        // +0x10 = its +0x28 (AREF in the top byte), word 11 = its +0x14 (0x80: blending); 16 Oct.
        DrawMaterial currentMaterial(uint8_t *rdram)
        {
            DrawMaterial dm;
            const uint32_t sub = guestAny(rdram, 0x70000034u);
            if (!sub)
                return dm;
            dm.textureKey = popTextureKey(guestAny(rdram, sub));
            const uint32_t f1c = guestAny(rdram, sub + 4), f28 = guestAny(rdram, sub + 0x10), f14 = guestAny(rdram, sub + 0x2C);
            if (f14 & 0x80)
                dm.mode = uint32_t(pop_native::AlphaMode::Blend);
            else if (f1c & 0x10)
            {
                dm.mode = uint32_t(pop_native::AlphaMode::Mask);
                dm.cutoff = float((f28 >> 24) / 255.0);
            }
            return dm;
        }

        // A skinned element drawn with matrix index `index`, posed as CJMeshElement::Display uploads
        // its bones: palette at element +0x48 (byte 0 = bone count n, bytes 4.. = bone ids); bone k's
        // matrix = *(0x700001D0) + (firstBone + id[k]) * 0x40, firstBone = u16 table at 0x700001DC
        // indexed by index - u16 at 0x7000025E. n >= 2: vertex slot s uses bone s - 1 (VU 0x388); one
        // bone: every vertex uses it (VU 0). Matrices are world space, row vectors.
        bool poseSkinned(uint8_t *rdram, uint32_t element, uint32_t index, const std::vector<pop_assets::SkinnedBatch> &bind,
                         std::vector<pop_assets::Batch> &out)
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
                    float acc[3] = {0, 0, 0}, total = 0;
                    for (int k = 0; k < 3; ++k)
                    {
                        const int32_t slot = b.slots[3 * i + k];
                        const float w = n < 2 ? (k == 0 ? 1.0f : 0.0f) : b.weights[3 * i + k];
                        if (w == 0 || (n >= 2 && (slot < 1 || uint32_t(slot) > n)))
                            continue;
                        const auto &m = bones[n < 2 ? 0 : uint32_t(slot - 1)];
                        for (int c = 0; c < 3; ++c)
                            acc[c] += w * (v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + m[12 + c]);
                        total += w;
                    }
                    pop_native::Vertex &out1 = o.vertices[i];
                    for (int c = 0; c < 3; ++c)
                        out1.pos[c] = total > 0 ? acc[c] / total : v[c];
                    out1.uv[0] = b.uvs[2 * i];
                    out1.uv[1] = b.uvs[2 * i + 1];
                    std::memcpy(out1.colour, &b.colours[4 * i], sizeof out1.colour);
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
                    if (poseSkinned(rdram, element, index, sk->second, posed))
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
        // File: "PNF1", clip[16], draw count; per draw: model[16], batch count; per batch: vertex
        // count, index count, vertices (pop_native::Vertex), indices (u32).
        std::function<uint32_t(uint32_t)> g_rdramForDump;
        void writeFrame()
        {
            FILE *f = std::fopen(g_framePath.c_str(), "wb");
            if (!f)
                return;
            // "PNF2": clip[16]; images (count; per image: w, h, RGBA); draws (count; per draw: model[16],
            // image index (-1: none), alpha mode, cutoff, batch count, batches)
            std::fwrite("PNF2", 1, 4, f);
            std::fwrite(g_frameClip, 4, 16, f);
            std::unordered_map<uint32_t, int32_t> imageOf;
            std::vector<pop_native::Image> images;
            std::vector<DrawMaterial> allMaterials = g_frameMaterials;
            for (const auto &p : g_framePosed)
                allMaterials.push_back(p.second);
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
            const uint32_t ni0 = uint32_t(images.size());
            std::fwrite(&ni0, 4, 1, f);
            for (const auto &img : images)
            {
                std::fwrite(&img.width, 4, 1, f);
                std::fwrite(&img.height, 4, 1, f);
                std::fwrite(img.rgba.data(), 1, img.rgba.size(), f);
            }
            const uint32_t n = uint32_t(g_frameDraws.size() + g_framePosed.size());
            std::fwrite(&n, 4, 1, f);
            size_t tris = 0, textured = 0;
            for (size_t di = 0; di < g_frameDraws.size(); ++di)
            {
                const auto &[element, model] = g_frameDraws[di];
                const auto &meshes = g_elements[element].meshes;
                const uint32_t nb = uint32_t(meshes.size());
                const DrawMaterial &mat = g_frameMaterials[di];
                const int32_t image = mat.textureKey && imageOf.count(mat.textureKey) ? imageOf[mat.textureKey] : -1;
                textured += image >= 0;
                std::fwrite(model.data(), 4, 16, f);
                std::fwrite(&image, 4, 1, f);
                std::fwrite(&mat.mode, 4, 1, f);
                std::fwrite(&mat.cutoff, 4, 1, f);
                std::fwrite(&nb, 4, 1, f);
                for (const auto &b : meshes)
                {
                    const uint32_t nv = uint32_t(b.vertices.size()), ni = uint32_t(b.indices.size());
                    std::fwrite(&nv, 4, 1, f);
                    std::fwrite(&ni, 4, 1, f);
                    std::fwrite(b.vertices.data(), sizeof(pop_native::Vertex), nv, f);
                    std::fwrite(b.indices.data(), 4, ni, f);
                    tris += ni / 3;
                }
            }
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            for (const auto &[batches, mat] : g_framePosed)
            {
                const int32_t image = mat.textureKey && imageOf.count(mat.textureKey) ? imageOf[mat.textureKey] : -1;
                textured += image >= 0;
                const uint32_t nb = uint32_t(batches.size());
                std::fwrite(kIdentity, 4, 16, f);
                std::fwrite(&image, 4, 1, f);
                std::fwrite(&mat.mode, 4, 1, f);
                std::fwrite(&mat.cutoff, 4, 1, f);
                std::fwrite(&nb, 4, 1, f);
                for (const auto &b : batches)
                {
                    const uint32_t nv = uint32_t(b.vertices.size()), ni = uint32_t(b.indices.size());
                    std::fwrite(&nv, 4, 1, f);
                    std::fwrite(&ni, 4, 1, f);
                    std::fwrite(b.vertices.data(), sizeof(pop_native::Vertex), nv, f);
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
            std::fprintf(stderr, "[pop:native] frame written to %s: %u draws (%zu skinned, %zu textured, %u images), %zu triangles\n",
                         g_framePath.c_str(), n, g_framePosed.size(), textured, ni0, tris);
        }

        void display3D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
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
        }
        g_lastReport = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[pop:native] live conversion check on\n");
    }
}
