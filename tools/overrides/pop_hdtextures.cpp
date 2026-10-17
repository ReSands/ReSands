// High-resolution textures for Prince of Persia: The Sands of Time (SLUS_207.43) from the PC
// version's bigfile (prince.bf, owned by the user). Loads a world pack by its bigfile key,
// unpacks it (LZO1X chunks), and decodes its texture records (DXT1, DXT5, BGRA32) to RGBA8.
// Formats: CLAUDE.md "Texture formats" and tools/jadetex.py.
//
// Environment: POP_PC_BIGFILE (default ../../gog/install/prince.bf, relative to game/ps2/disc).
// Debug: POP_HDTEX_TEST=<pack key>:<texture key> decodes one texture at start-up and writes
// ../../recomp/screens/hdtex_<key>.png.

#include "raylib.h"

// The bigfile reader, by path: this file is copied to <runtime>/ps2xRuntime/src/runner, six levels
// below the repository root (as in pop_native_live.cpp).
#include "../../../../../../tools/native/src/bigfile.h"

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

        using pop_bigfile::lzo1xDecompress; // tools/native/src/bigfile.h

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

    // Reads bigfile entry `key` of the bigfile at `path` (Jade BIG: entry count at 0x08, (offset, key)
    // pairs from 0x44; an entry is a u32 size, then 0x20000-byte chunks [u32 unpacked size][u32 packed
    // size][LZO1X data, or raw when the sizes match]) and returns its unpacked bytes. Thread-safe; the
    // index of each bigfile is read once. Used for the PS2 PRINCE.BF by pop_native_live.cpp.
    bool readEntry(const std::string &path, uint32_t key, std::vector<uint8_t> &out)
    {
        return pop_bigfile::readEntry(path, key, out);
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
