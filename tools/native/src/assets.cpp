// See assets.h. Ported from tools/jade2gltf.py (main's static path, batch_mesh, instanced meshes)
// and tools/jadetex.py (ps2_texture, ps2_lightmap); comments there give the format evidence.
#include "assets.h"

#include "gs_tables.h"

#include <algorithm>
#include <cstring>

namespace pop_assets
{
    namespace
    {
        constexpr uint32_t kRecordMarker = 0xEEFFC099u;
        constexpr uint32_t kTextureMagic = 0xCAD01234u;

        bool u32(const Pack &p, size_t o, uint32_t &v)
        {
            if (o + 4 > p.len)
                return false;
            std::memcpy(&v, p.d + o, 4);
            return true;
        }
        bool u64(const Pack &p, size_t o, uint64_t &v)
        {
            if (o + 8 > p.len)
                return false;
            std::memcpy(&v, p.d + o, 8);
            return true;
        }
        bool u16(const Pack &p, size_t o, uint16_t &v)
        {
            if (o + 2 > p.len)
                return false;
            std::memcpy(&v, p.d + o, 2);
            return true;
        }

        // jadetex.image_data: the data after the GIF IMAGE tag (FLG 2) whose NLOOP * 16 is nbytes,
        // searched from start in 4-byte steps.
        bool imageData(const Pack &p, size_t start, size_t end, size_t nbytes, size_t &out)
        {
            for (size_t q = start; q + 16 < end; q += 4)
            {
                uint64_t tag;
                if (!u64(p, q, tag))
                    return false;
                if (((tag >> 58) & 3) == 2 && (tag & 0x7FFF) * 16 == nbytes)
                {
                    out = q + 16;
                    return out + nbytes <= p.len;
                }
            }
            return false;
        }

        // GS PSMCT32 byte address and PSMT4 nibble address (ps2xRuntime's layout, via gs_tables.h).
        uint32_t addr32(uint32_t block, uint32_t width, uint32_t x, uint32_t y)
        {
            const uint32_t page = (block >> 5) + (y >> 5) * width + (x >> 6);
            const uint32_t id = (block & 0x1F) + pop_gs::kBlock32[(y >> 3) & 3][(x >> 3) & 7];
            return ((page << 13) + ((id >> 5) << 13) + (id & 0x1F) * 256 + pop_gs::kColumn32[y & 7][x & 7] * 4) % (4u << 20);
        }
        uint32_t addr4(uint32_t block, uint32_t width, uint32_t x, uint32_t y)
        {
            const uint32_t ppr = (width >> 1) ? (width >> 1) : 1;
            const uint32_t page = (block >> 5) + (y >> 7) * ppr + (x >> 7);
            const uint32_t id = (block & 0x1F) + pop_gs::kBlock4[(y >> 4) & 7][(x >> 5) & 3];
            return ((page << 14) + ((id >> 5) << 14) + (id & 0x1F) * 512 + pop_gs::kColumn4[y & 15][x & 31]) % (8u << 20);
        }

        bool texture8(const Pack &p, uint32_t off, pop_native::Image &out)
        {
            uint16_t w, h;
            uint32_t palKey;
            if (!u16(p, off + 12, w) || !u16(p, off + 14, h) || !u32(p, off + 0x3C, palKey) || !w || !h)
                return false;
            const auto pal = p.records.find(palKey);
            if (pal == p.records.end())
                return false;
            size_t pp, dp;
            if (!imageData(p, pal->second.first, pal->second.first + pal->second.second, 1024, pp) ||
                !imageData(p, off, off + 0x200, size_t(w) * h, dp))
                return false;
            const uint8_t *buf = p.d + dp, *palette = p.d + pp;
            out.width = w;
            out.height = h;
            out.rgba.assign(size_t(w) * h * 4, 0);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    // jadetex.unswizzle8
                    const size_t block = size_t(y & ~0xFu) * w + (x & ~0xFu) * 2;
                    const uint32_t swap = (((y + 2) >> 2) & 1) * 4;
                    const uint32_t py = (((y & ~3u) >> 1) + (y & 1)) & 7;
                    const size_t col = size_t(py) * w * 2 + ((x + swap) & 7) * 4;
                    const uint32_t byte = ((y >> 1) & 1) + ((x >> 2) & 2);
                    // the formula assumes 16x16 swizzle blocks: textures smaller than that (8x8 seen)
                    // index past their data; refused, as the Python reference does (IndexError)
                    if (block + col + byte >= size_t(w) * h)
                        return false;
                    uint32_t c = buf[block + col + byte];
                    c = (c & 0xE7) | ((c & 0x08) << 1) | ((c & 0x10) >> 1); // CSM1 palette order
                    uint8_t *o = &out.rgba[(size_t(y) * w + x) * 4];
                    o[0] = palette[c * 4];
                    o[1] = palette[c * 4 + 1];
                    o[2] = palette[c * 4 + 2];
                    o[3] = static_cast<uint8_t>(std::min(255, palette[c * 4 + 3] * 2));
                }
            return true;
        }

        bool lightmap(const Pack &p, uint32_t off, uint32_t size, pop_native::Image &out)
        {
            uint16_t w, h;
            if (!u16(p, off + 12, w) || !u16(p, off + 14, h) || !w || !h)
                return false;
            std::unordered_map<uint32_t, uint64_t> regs;
            const uint8_t *img = nullptr;
            size_t imgBytes = 0;
            const size_t end = off + std::min<uint32_t>(size, 0x400);
            for (size_t q = off; q + 16 <= end;)
            {
                uint64_t lo, hi;
                if (!u64(p, q, lo) || !u64(p, q + 8, hi))
                    return false;
                const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3, nreg = ((lo >> 60) & 0xF) ? ((lo >> 60) & 0xF) : 16;
                if (flg == 0 && nreg == 1 && (hi & 0xF) == 0xE && nloop > 0 && nloop < 16)
                {
                    for (uint32_t k = 0; k < nloop; ++k)
                    {
                        uint64_t val, a;
                        if (!u64(p, q + 16 + 16 * k, val) || !u64(p, q + 24 + 16 * k, a))
                            return false;
                        regs[uint32_t(a & 0xFF)] = val;
                    }
                    q += 16 + 16 * size_t(nloop);
                    continue;
                }
                if (flg == 2 && nloop > 0 && nloop * 16u <= size)
                {
                    if (q + 16 + nloop * 16u > p.len)
                        return false;
                    img = p.d + q + 16;
                    imgBytes = nloop * 16u;
                    break;
                }
                q += 16;
            }
            if (!img)
                return false;
            const auto reg = [&](uint32_t r) { const auto it = regs.find(r); return it == regs.end() ? uint64_t{0} : it->second; };
            uint32_t dbw = uint32_t(reg(0x50) >> 48) & 0x3F;
            if (!dbw)
                dbw = std::max(1, w / 128);
            uint32_t cw = uint32_t(reg(0x52)) & 0xFFF;
            if (!cw)
                cw = 128;
            std::vector<uint8_t> vram(4u << 20);
            for (size_t i = 0; i < imgBytes / 4; ++i)
            {
                const uint32_t y = uint32_t(i / cw), x = uint32_t(i % cw);
                std::memcpy(&vram[addr32(0, dbw, x, y)], img + 4 * i, 4);
            }
            const uint32_t tbw = std::max(1, w / 64);
            out.width = w;
            out.height = h;
            out.rgba.assign(size_t(w) * h * 4, 255);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t n = addr4(0, tbw, x, y);
                    const uint8_t b = vram[(n >> 1) % vram.size()];
                    const uint8_t v = static_cast<uint8_t>(17 * ((n & 1) ? (b >> 4) : (b & 0xF)));
                    uint8_t *o = &out.rgba[(size_t(y) * w + x) * 4];
                    o[0] = o[1] = o[2] = v;
                }
            return true;
        }

        // Triangle strip with restart flags (a set flag starts a new strip at that vertex): the
        // game's winding alternates per vertex (jade2gltf).
        void strips(const std::vector<int32_t> &flags, size_t n, std::vector<uint32_t> &indices)
        {
            for (size_t i = 2; i < n; ++i)
            {
                if (i < flags.size() && flags[i])
                    continue;
                const uint32_t a = uint32_t(i - 2), b = uint32_t(i - 1), c = uint32_t(i);
                if (i % 2 == 0)
                    indices.insert(indices.end(), {a, b, c});
                else
                    indices.insert(indices.end(), {b, a, c});
            }
        }
    }

    bool indexPack(const uint8_t *d, size_t len, Pack &out)
    {
        out.d = d;
        out.len = len;
        out.records.clear();
        size_t p = 0;
        while (p + 12 <= len)
        {
            uint32_t size, marker, key;
            std::memcpy(&size, d + p, 4);
            std::memcpy(&marker, d + p + 4, 4);
            std::memcpy(&key, d + p + 8, 4);
            if (marker != kRecordMarker || p + 12 + size > len)
                return false;
            out.records[key] = {uint32_t(p + 12), size}; // later records with the same key win, as in Python
            p += 12 + size_t(size);
        }
        return true;
    }

    bool isTexture(const Pack &pack, uint32_t key)
    {
        const auto it = pack.records.find(key);
        uint32_t a, magic;
        return it != pack.records.end() && u32(pack, it->second.first + 4, a) && a == 0xFFFFFFFFu &&
               u32(pack, it->second.first + 0x18, magic) && magic == kTextureMagic;
    }

    uint32_t textureFormat(const Pack &pack, uint32_t key)
    {
        uint16_t f = 0;
        return isTexture(pack, key) && u16(pack, pack.records.at(key).first + 8, f) ? f : 0u;
    }

    bool decodeTexture(const Pack &pack, uint32_t key, pop_native::Image &out)
    {
        if (!isTexture(pack, key))
            return false;
        const auto [off, size] = pack.records.at(key);
        return textureFormat(pack, key) == 6 ? lightmap(pack, off, size, out) : texture8(pack, off, out);
    }

    bool subMaterial(const Pack &pack, uint32_t multiMaterial, uint32_t index, SubMaterial &out)
    {
        const auto mm = pack.records.find(multiMaterial);
        uint32_t kind, count, sub;
        if (mm == pack.records.end() || !u32(pack, mm->second.first, kind) || kind != 4 ||
            !u32(pack, mm->second.first + 8, count) || index >= count || !u32(pack, mm->second.first + 0xC + 4 * index, sub))
            return false;
        const auto sm = pack.records.find(sub);
        uint32_t f14, f1c, f28;
        if (sm == pack.records.end() || !u32(pack, sm->second.first, kind) || kind != 5 ||
            !u32(pack, sm->second.first + 0x2C, out.texture) || !u32(pack, sm->second.first + 0x14, f14) ||
            !u32(pack, sm->second.first + 0x1C, f1c) || !u32(pack, sm->second.first + 0x28, f28))
            return false;
        // +0x14 bit 0x80: alpha blending; else +0x1C bit 0x10: alpha test, AREF = (+0x28 top byte) / 2
        // on the PS2 scale = the top byte on the doubled 0..255 alpha
        if (f14 & 0x80)
            out.mode = pop_native::AlphaMode::Blend;
        else if (f1c & 0x10)
        {
            out.mode = pop_native::AlphaMode::Mask;
            out.cutoff = float((f28 >> 24) / 255.0);
        }
        else
            out.mode = pop_native::AlphaMode::Opaque;
        return true;
    }

    bool convertElement(const Pack &pack, uint32_t offset, double uvscale, std::vector<Batch> &out)
    {
        pop_mesh::ElementParts parts;
        if (!pop_mesh::elementParts(pack.d, pack.len, offset, parts) || parts.passes.empty() || parts.boneCount)
            return false;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> blocks;
        for (const auto &l : parts.lods)
            blocks[l.index] = {l.offset, l.size};
        // lightmap pass (pass index 3): same positions, second UV set at 1/(8 * uvscale)
        std::vector<std::vector<double>> lmUv; // per batch with positions; empty = none
        for (const auto &pass : parts.passes)
        {
            if (pass.index != 3)
                continue;
            std::vector<pop_mesh::Batch> lb;
            if (!pop_mesh::decodePacket(pack.d, pack.len, pass.offset, pass.size, &blocks, lb))
                continue;
            for (const auto &b : lb)
            {
                if (!b.count(8))
                    continue;
                std::vector<double> uv2;
                const auto it9 = b.find(9);
                if (it9 != b.end())
                    for (uint32_t i = 0; i < it9->second.count && 6 * i + 4 <= it9->second.raw.size(); ++i)
                    {
                        int16_t s, t;
                        std::memcpy(&s, &it9->second.raw[6 * i], 2);
                        std::memcpy(&t, &it9->second.raw[6 * i + 2], 2);
                        uv2.push_back(s / (8 * uvscale));
                        uv2.push_back(t / (8 * uvscale));
                    }
                lmUv.push_back(std::move(uv2));
            }
        }
        // elements whose packets reference data blocks store texture UVs at 1/512 (8x coarser)
        const double baseScale = blocks.empty() ? uvscale : uvscale / 8;
        std::vector<pop_mesh::Batch> batches;
        if (!pop_mesh::decodePacket(pack.d, pack.len, parts.passes[0].offset, parts.passes[0].size, &blocks, batches))
            return false;
        int batchNo = -1;
        for (const auto &b : batches)
        {
            const auto itC = b.find(0xC);
            if (b.count(8) || (itC != b.end() && itC->second.addr == 3))
                ++batchNo;
            pop_mesh::Mesh m;
            if (!pop_mesh::batchMesh(b, baseScale, m) || m.tris.empty())
                continue;
            const size_t n = m.positions.size() / 3;
            const std::vector<double> *uv2 = (batchNo >= 0 && size_t(batchNo) < lmUv.size() && lmUv[batchNo].size() == 2 * n)
                                                 ? &lmUv[batchNo] : nullptr;
            Batch out1;
            out1.hasUv2 = uv2 != nullptr;
            out1.vertices.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                pop_native::Vertex &v = out1.vertices[i];
                std::memcpy(v.pos, &m.positions[3 * i], sizeof v.pos);
                v.uv[0] = float(m.uvs[2 * i]);
                v.uv[1] = float(m.uvs[2 * i + 1]);
                for (int k = 0; k < 4; ++k)
                    v.colour[k] = 4 * i + k < m.colours.size() ? float(m.colours[4 * i + k]) : 1.0f;
                v.uv2[0] = uv2 ? float((*uv2)[2 * i]) : 0.0f;
                v.uv2[1] = uv2 ? float((*uv2)[2 * i + 1]) : 0.0f;
            }
            out1.indices = m.tris;
            out.push_back(std::move(out1));
        }
        return true;
    }

    bool convertSkinned(const Pack &pack, uint32_t offset, std::vector<SkinnedBatch> &out)
    {
        pop_mesh::ElementParts parts;
        if (!pop_mesh::elementParts(pack.d, pack.len, offset, parts) || parts.passes.empty() || !parts.boneCount)
            return false;
        std::vector<pop_mesh::SkinnedBatch> batches;
        if (!pop_mesh::skinnedBatches(pack.d, pack.len, parts.passes[0].offset, parts.passes[0].size, batches))
            return false;
        for (const auto &b : batches)
        {
            const size_t n = b.positions.size() / 3;
            SkinnedBatch o;
            o.positions = b.positions;
            o.uvs.resize(2 * n);
            o.colours.assign(4 * n, 1.0f);
            o.slots.assign(3 * n, 0);
            o.weights.assign(3 * n, 0.0f);
            for (size_t i = 0; i < n; ++i)
            {
                // skinned UVs are at 1/512 (raw 0..512, as reference-style static meshes)
                o.uvs[2 * i] = 2 * i < b.uvs.size() ? float(b.uvs[2 * i] / 512.0) : 0.0f;
                o.uvs[2 * i + 1] = 2 * i + 1 < b.uvs.size() ? float(b.uvs[2 * i + 1] / 512.0) : 0.0f;
                if (b.hasColours)
                    for (int k = 0; k < 4 && 4 * i + k < b.colours.size(); ++k)
                        o.colours[4 * i + k] = float(b.colours[4 * i + k]);
                for (int k = 0; k < 3; ++k)
                {
                    o.slots[3 * i + k] = i < b.bones.size() && size_t(k) < b.bones[i].size() ? b.bones[i][k] : 0;
                    o.weights[3 * i + k] = 3 * i + k < b.weights.size() ? b.weights[3 * i + k] : 0.0f;
                }
            }
            strips(b.flags, n, o.indices);
            if (!o.indices.empty())
                out.push_back(std::move(o));
        }
        return !out.empty();
    }

    bool isInstanced(const Pack &pack, uint32_t offset)
    {
        pop_mesh::ElementParts parts;
        if (!pop_mesh::elementParts(pack.d, pack.len, offset, parts) || !parts.passes.empty())
            return false;
        return std::any_of(parts.lods.begin(), parts.lods.end(), [](const pop_mesh::Block &l) { return l.index == 3; });
    }

    bool convertInstanced(const Pack &pack, uint32_t offset, const uint8_t *colours, size_t colourBytes, double uvscale,
                          std::vector<Batch> &out)
    {
        pop_mesh::ElementParts parts;
        if (!pop_mesh::elementParts(pack.d, pack.len, offset, parts) || !parts.passes.empty())
            return false;
        const auto v3 = std::find_if(parts.lods.begin(), parts.lods.end(), [](const pop_mesh::Block &l) { return l.index == 3; });
        if (v3 == parts.lods.end())
            return false;
        std::vector<pop_mesh::Variant3Batch> batches;
        if (!pop_mesh::variant3Batches(pack.d, pack.len, v3->offset, v3->size, batches))
            return false;
        size_t vbase = 0;
        for (const auto &b : batches)
        {
            const size_t n = b.positions.size() / 3;
            Batch o;
            o.vertices.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                pop_native::Vertex &v = o.vertices[i];
                std::memcpy(v.pos, &b.positions[3 * i], sizeof v.pos);
                v.uv[0] = float(b.uvs[2 * i] / uvscale);
                v.uv[1] = float(b.uvs[2 * i + 1] / uvscale);
                const size_t c = 4 * (vbase + i);
                for (int k = 0; k < 4; ++k)
                    v.colour[k] = colours && c + 4 <= colourBytes ? std::min(colours[c + k] / 128.0f, 1.0f) : 1.0f;
            }
            vbase += n;
            std::vector<int32_t> flags(b.flags.begin(), b.flags.end());
            strips(flags, n, o.indices);
            if (!o.indices.empty())
                out.push_back(std::move(o));
        }
        return true;
    }
}
