// High-resolution textures for Prince of Persia: The Sands of Time (SLUS_207.43) from the PC
// version's bigfile (prince.bf, owned by the user). Loads a world pack by its bigfile key,
// unpacks it (LZO1X chunks), and decodes its texture records (DXT1, DXT5, BGRA32) to RGBA8.
// Formats: CLAUDE.md "Texture formats" and tools/jadetex.py.
//
// Environment: POP_PC_BIGFILE (default ../../gog/install/prince.bf, relative to game/ps2/disc).
// Debug: POP_HDTEX_TEST=<pack key>:<texture key> decodes one texture at start-up and writes
// ../../recomp/screens/hdtex_<key>.png.

#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

namespace pop_hdtex
{
    struct Texture
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba; // top mip level, RGBA8, alpha 0..255
    };

    using Pack = std::unordered_map<uint32_t, Texture>; // texture key -> texture

    namespace
    {
        constexpr uint32_t kRecordMarker = 0xEEFFC099u;
        constexpr uint32_t kTextureMagic = 0xCAD01234u;

        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
        uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }

        // LZO1X decompression (port of tools/lzo1x.py). Returns false on malformed input.
        bool lzo1xDecompress(const uint8_t *src, size_t srcSize, std::vector<uint8_t> &out)
        {
            size_t ip = 0;
            auto byteAt = [&](size_t i) -> int { return i < srcSize ? src[i] : -1; };
            auto length = [&](size_t base, size_t value, size_t &result) -> bool {
                if (value == 0)
                {
                    while (true)
                    {
                        const int b = byteAt(ip);
                        if (b < 0)
                            return false;
                        if (b != 0)
                            break;
                        value += 255;
                        ++ip;
                    }
                    value += base + src[ip++];
                }
                result = value;
                return true;
            };
            auto copyLiterals = [&](size_t count) -> bool {
                if (ip + count > srcSize)
                    return false;
                out.insert(out.end(), src + ip, src + ip + count);
                ip += count;
                return true;
            };
            auto copyMatch = [&](size_t distance, size_t count) -> bool {
                if (distance > out.size())
                    return false;
                const size_t start = out.size() - distance;
                for (size_t i = 0; i < count; ++i)
                    out.push_back(out[start + i]);
                return true;
            };

            int state = 0;
            if (srcSize == 0)
                return false;
            size_t t = src[ip];
            if (t > 17)
            {
                ++ip;
                t -= 17;
                if (!copyLiterals(t))
                    return false;
                state = t >= 4 ? 4 : int(t);
            }
            while (ip < srcSize)
            {
                t = src[ip++];
                if (t >= 64)
                {
                    const size_t count = (t >> 5) + 1;
                    if (ip >= srcSize)
                        return false;
                    const size_t distance = ((t >> 2) & 7) + (size_t(src[ip]) << 3) + 1;
                    ++ip;
                    if (!copyMatch(distance, count))
                        return false;
                }
                else if (t >= 32)
                {
                    size_t count;
                    if (!length(31, t & 31, count) || ip + 1 >= srcSize)
                        return false;
                    const size_t distance = (src[ip] >> 2) + (size_t(src[ip + 1]) << 6) + 1;
                    ip += 2;
                    if (!copyMatch(distance, count + 2))
                        return false;
                }
                else if (t >= 16)
                {
                    size_t count;
                    if (!length(7, t & 7, count) || ip + 1 >= srcSize)
                        return false;
                    const size_t distance = ((t & 8) << 11) + (src[ip] >> 2) + (size_t(src[ip + 1]) << 6);
                    ip += 2;
                    if (distance == 0)
                        return true; // end marker
                    if (!copyMatch(distance + 0x4000, count + 2))
                        return false;
                }
                else
                {
                    if (state == 0)
                    {
                        size_t count;
                        if (!length(15, t, count) || !copyLiterals(count + 3))
                            return false;
                        state = 4;
                        continue;
                    }
                    if (ip >= srcSize)
                        return false;
                    const size_t far = state == 4 ? 0x801 : 1;
                    const size_t distance = (t >> 2) + (size_t(src[ip]) << 2) + far;
                    ++ip;
                    if (!copyMatch(distance, state == 4 ? 3 : 2))
                        return false;
                }
                const size_t trailing = src[ip - 2] & 3;
                if (!copyLiterals(trailing))
                    return false;
                state = int(trailing);
            }
            return false;
        }

        void rgb565(uint16_t v, uint8_t *out)
        {
            out[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
            out[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
            out[2] = uint8_t((v & 31) * 255 / 31);
        }

        // DXT1 (alpha == false) or DXT5 (alpha == true) to RGBA8.
        bool decodeDxt(const uint8_t *data, size_t size, uint32_t w, uint32_t h, bool alpha, std::vector<uint8_t> &out)
        {
            const size_t blockBytes = alpha ? 16 : 8;
            const size_t blocks = size_t((w + 3) / 4) * ((h + 3) / 4);
            if (blocks * blockBytes > size)
                return false;
            out.assign(size_t(w) * h * 4, 0);
            const uint8_t *p = data;
            for (uint32_t by = 0; by < h; by += 4)
            {
                for (uint32_t bx = 0; bx < w; bx += 4)
                {
                    uint8_t alphas[16];
                    if (alpha)
                    {
                        const uint8_t a0 = p[0], a1 = p[1];
                        uint8_t table[8] = {a0, a1};
                        if (a0 > a1)
                            for (int i = 0; i < 6; ++i)
                                table[2 + i] = uint8_t(((6 - i) * a0 + (i + 1) * a1) / 7);
                        else
                        {
                            for (int i = 0; i < 4; ++i)
                                table[2 + i] = uint8_t(((4 - i) * a0 + (i + 1) * a1) / 5);
                            table[6] = 0;
                            table[7] = 255;
                        }
                        uint64_t bits = 0;
                        for (int i = 0; i < 6; ++i)
                            bits |= uint64_t(p[2 + i]) << (8 * i);
                        for (int j = 0; j < 16; ++j)
                            alphas[j] = table[(bits >> (3 * j)) & 7];
                        p += 8;
                    }
                    const uint16_t c0 = le16(p), c1 = le16(p + 2);
                    const uint32_t bits = le32(p + 4);
                    p += 8;
                    uint8_t cols[4][4];
                    rgb565(c0, cols[0]);
                    rgb565(c1, cols[1]);
                    cols[0][3] = cols[1][3] = 255;
                    if (c0 > c1 || alpha)
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            cols[2][c] = uint8_t((2 * cols[0][c] + cols[1][c]) / 3);
                            cols[3][c] = uint8_t((cols[0][c] + 2 * cols[1][c]) / 3);
                        }
                        cols[2][3] = cols[3][3] = 255;
                    }
                    else
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            cols[2][c] = uint8_t((cols[0][c] + cols[1][c]) / 2);
                            cols[3][c] = 0;
                        }
                        cols[2][3] = 255;
                        cols[3][3] = 0;
                    }
                    for (int j = 0; j < 16; ++j)
                    {
                        const uint32_t x = bx + j % 4, y = by + j / 4;
                        if (x >= w || y >= h)
                            continue;
                        uint8_t *dst = &out[(size_t(y) * w + x) * 4];
                        std::memcpy(dst, cols[(bits >> (2 * j)) & 3], 4);
                        if (alpha)
                            dst[3] = alphas[j];
                    }
                }
            }
            return true;
        }

        // One PC texture record (format 0xB). Header: +0x0C/+0x0E width/height, +0x24 header
        // variant (3: data at +0x3C, else +0x38), +0x28 pixel format (0 BGRA32, 5/11 DXT1, 7 DXT5).
        std::optional<Texture> decodeRecord(const uint8_t *rec, size_t size)
        {
            if (size < 0x40 || le32(rec + 4) != 0xFFFFFFFFu || le32(rec + 0x18) != kTextureMagic)
                return std::nullopt;
            Texture t;
            t.width = le16(rec + 0x0C);
            t.height = le16(rec + 0x0E);
            if (!t.width || !t.height || t.width > 4096 || t.height > 4096)
                return std::nullopt;
            const uint32_t variant = le32(rec + 0x24), format = le32(rec + 0x28);
            const size_t start = variant == 3 ? 0x3C : 0x38;
            const uint8_t *px = rec + start;
            const size_t avail = size - start;
            bool ok = false;
            if (format == 7)
                ok = decodeDxt(px, avail, t.width, t.height, true, t.rgba);
            else if (format == 5 || format == 11)
                ok = decodeDxt(px, avail, t.width, t.height, false, t.rgba);
            else if (format == 0 && avail >= size_t(t.width) * t.height * 4)
            {
                t.rgba.resize(size_t(t.width) * t.height * 4);
                for (size_t i = 0; i < size_t(t.width) * t.height; ++i)
                {
                    t.rgba[i * 4 + 0] = px[i * 4 + 2];
                    t.rgba[i * 4 + 1] = px[i * 4 + 1];
                    t.rgba[i * 4 + 2] = px[i * 4 + 0];
                    t.rgba[i * 4 + 3] = 255;
                }
                ok = true;
            }
            if (!ok)
                return std::nullopt;
            return t;
        }

        struct Bigfile
        {
            std::string path;
            std::unordered_map<uint32_t, uint32_t> offsets; // key -> file offset of the entry
            bool opened = false;
        };

        std::mutex g_mutex;
        Bigfile g_bigfile;

        bool openBigfile()
        {
            if (g_bigfile.opened)
                return !g_bigfile.offsets.empty();
            g_bigfile.opened = true;
            const char *env = std::getenv("POP_PC_BIGFILE");
            g_bigfile.path = env && *env ? env : "../../gog/install/prince.bf";
            std::ifstream f(g_bigfile.path, std::ios::binary);
            uint8_t head[0x44];
            if (!f.read(reinterpret_cast<char *>(head), sizeof(head)) || std::memcmp(head, "BIG", 4) != 0)
            {
                std::cerr << "[pop:hdtex] no PC bigfile at " << g_bigfile.path << ", high-res textures off" << std::endl;
                return false;
            }
            const uint32_t count = le32(head + 8);
            std::vector<uint8_t> table(size_t(count) * 8);
            if (!f.read(reinterpret_cast<char *>(table.data()), std::streamsize(table.size())))
                return false;
            for (uint32_t i = 0; i < count; ++i)
                g_bigfile.offsets[le32(&table[i * 8 + 4])] = le32(&table[i * 8]);
            std::cerr << "[pop:hdtex] PC bigfile " << g_bigfile.path << ": " << count << " entries" << std::endl;
            return true;
        }
    }

    // Loads and decodes all textures of the world pack with bigfile key `packKey`. Thread-safe;
    // meant to run on a worker thread.
    std::shared_ptr<Pack> loadPack(uint32_t packKey)
    {
        std::string path;
        uint32_t offset = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!openBigfile())
                return nullptr;
            const auto it = g_bigfile.offsets.find(packKey);
            if (it == g_bigfile.offsets.end())
                return nullptr;
            path = g_bigfile.path;
            offset = it->second;
        }
        std::ifstream f(path, std::ios::binary);
        uint8_t sizeBytes[4];
        if (!f.seekg(offset) || !f.read(reinterpret_cast<char *>(sizeBytes), 4))
            return nullptr;
        std::vector<uint8_t> blob(le32(sizeBytes));
        if (!f.read(reinterpret_cast<char *>(blob.data()), std::streamsize(blob.size())))
            return nullptr;

        std::vector<uint8_t> data;
        for (size_t p = 0; p + 8 <= blob.size();)
        {
            const uint32_t usize = le32(&blob[p]), csize = le32(&blob[p + 4]);
            p += 8;
            if (usize == 0 || p + csize > blob.size())
                break;
            if (csize == usize)
                data.insert(data.end(), blob.begin() + long(p), blob.begin() + long(p + csize));
            else if (!lzo1xDecompress(&blob[p], csize, data))
            {
                std::cerr << "[pop:hdtex] LZO error in pack " << std::hex << packKey << std::dec << std::endl;
                return nullptr;
            }
            p += csize;
        }

        auto pack = std::make_shared<Pack>();
        for (size_t p = 0; p + 12 <= data.size();)
        {
            const uint32_t size = le32(&data[p]), marker = le32(&data[p + 4]), key = le32(&data[p + 8]);
            if (marker != kRecordMarker || p + 12 + size > data.size())
                break;
            if (auto t = decodeRecord(&data[p + 12], size))
                (*pack)[key] = std::move(*t);
            p += 12 + size;
        }
        return pack;
    }

    namespace
    {
        constexpr size_t kMaxPacks = 24; // loaded PC packs kept (least recently requested dropped)
        std::mutex g_packsMutex;
        std::unordered_map<uint32_t, std::shared_ptr<Pack>> g_packs; // pack key -> textures (null while loading)
        std::vector<uint32_t> g_packOrder;                          // least recently requested first
    }

    // The game looked up bigfile key `key` (BIG_ul_SearchKeyToPos). For a world pack (0xFF......),
    // load the PC pack with the same key on a worker thread, once.
    void onBigfileLookup(uint32_t key)
    {
        if ((key >> 24) != 0xFFu)
            return;
        {
            std::lock_guard<std::mutex> lock(g_packsMutex);
            auto pos = std::find(g_packOrder.begin(), g_packOrder.end(), key);
            if (pos != g_packOrder.end())
            {
                g_packOrder.erase(pos);
                g_packOrder.push_back(key);
                return;
            }
            g_packOrder.push_back(key);
            g_packs[key] = nullptr;
            while (g_packOrder.size() > kMaxPacks)
            {
                g_packs.erase(g_packOrder.front());
                g_packOrder.erase(g_packOrder.begin());
            }
        }
        std::thread([key] {
            const auto start = std::chrono::steady_clock::now();
            auto pack = loadPack(key);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (pack && !pack->empty())
                std::cerr << "[pop:hdtex] PC pack " << std::hex << key << std::dec << ": " << pack->size() << " textures in "
                          << ms << " ms" << std::endl;
            std::lock_guard<std::mutex> lock(g_packsMutex);
            if (auto it = g_packs.find(key); it != g_packs.end())
                it->second = std::move(pack);
        }).detach();
    }

    // The PC texture for a bigfile texture key, from any loaded pack, or nullptr.
    std::shared_ptr<const Texture> find(uint32_t textureKey)
    {
        std::lock_guard<std::mutex> lock(g_packsMutex);
        for (auto it = g_packOrder.rbegin(); it != g_packOrder.rend(); ++it)
        {
            const auto &pack = g_packs[*it];
            if (!pack)
                continue;
            if (auto t = pack->find(textureKey); t != pack->end())
                return std::shared_ptr<const Texture>(pack, &t->second); // keeps the pack alive
        }
        return nullptr;
    }

    // Debug: POP_HDTEX_TEST=<pack key>:<texture key>
    void runSelfTest()
    {
        const char *env = std::getenv("POP_HDTEX_TEST");
        if (!env || !*env)
            return;
        unsigned packKey = 0, texKey = 0;
        if (std::sscanf(env, "%x:%x", &packKey, &texKey) != 2)
            return;
        const auto start = std::chrono::steady_clock::now();
        const auto pack = loadPack(packKey);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!pack)
        {
            std::cerr << "[pop:hdtex] test: pack " << std::hex << packKey << " not loaded" << std::dec << std::endl;
            return;
        }
        std::cerr << "[pop:hdtex] test: pack " << std::hex << packKey << std::dec << " -> " << pack->size()
                  << " textures in " << ms << " ms" << std::endl;
        const auto it = pack->find(texKey);
        if (it == pack->end())
            return;
        Image img{it->second.rgba.data(), int(it->second.width), int(it->second.height), 1,
                  PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        char path[128];
        std::snprintf(path, sizeof(path), "../../recomp/screens/hdtex_%08x.png", texKey);
        ExportImage(img, path);
        std::cerr << "[pop:hdtex] test: wrote " << path << " (" << it->second.width << "x" << it->second.height << ")"
                  << std::endl;
    }
}
