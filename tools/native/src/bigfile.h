// Jade bigfile (BIG) reading for the native tools and the game overrides: LZO1X decompression and
// entry lookup by key. Header-only; used by tools/overrides/pop_hdtextures.cpp (pop_hdtex::readEntry)
// and tools/native/src/state_frame.cpp.
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pop_bigfile
{
    inline uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

    // LZO1X decompression (port of tools/lzo1x.py). Returns false on malformed input.
    inline bool lzo1xDecompress(const uint8_t *src, size_t srcSize, std::vector<uint8_t> &out)
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

    // Reads bigfile entry `key` of the bigfile at `path` (Jade BIG: entry count at 0x08, (offset, key)
    // pairs from 0x44; an entry is a u32 size, then 0x20000-byte chunks [u32 unpacked size][u32 packed
    // size][LZO1X data, or raw when the sizes match]) and returns its unpacked bytes. Thread-safe; the
    // index of each bigfile is read once.
    inline bool readEntry(const std::string &path, uint32_t key, std::vector<uint8_t> &out)
    {
        static std::mutex indexMutex;
        static std::unordered_map<std::string, std::unordered_map<uint32_t, uint32_t>> indexes;
        uint32_t offset = 0;
        {
            std::lock_guard<std::mutex> lock(indexMutex);
            auto it = indexes.find(path);
            if (it == indexes.end())
            {
                auto &index = indexes[path];
                std::ifstream f(path, std::ios::binary);
                uint8_t head[0x44];
                if (f.read(reinterpret_cast<char *>(head), sizeof(head)) && std::memcmp(head, "BIG", 4) == 0)
                {
                    const uint32_t count = le32(head + 8);
                    std::vector<uint8_t> table(size_t(count) * 8);
                    if (f.read(reinterpret_cast<char *>(table.data()), std::streamsize(table.size())))
                        for (uint32_t i = 0; i < count; ++i)
                            index[le32(&table[i * 8 + 4])] = le32(&table[i * 8]);
                }
                it = indexes.find(path);
            }
            const auto e = it->second.find(key);
            if (e == it->second.end())
                return false;
            offset = e->second;
        }
        std::ifstream f(path, std::ios::binary);
        uint8_t sizeBytes[4];
        if (!f.seekg(offset) || !f.read(reinterpret_cast<char *>(sizeBytes), 4))
            return false;
        std::vector<uint8_t> blob(le32(sizeBytes));
        if (!f.read(reinterpret_cast<char *>(blob.data()), std::streamsize(blob.size())))
            return false;
        out.clear();
        for (size_t p = 0; p + 8 <= blob.size();)
        {
            const uint32_t usize = le32(&blob[p]), csize = le32(&blob[p + 4]);
            p += 8;
            if (usize == 0 || p + csize > blob.size())
                break;
            if (csize == usize)
                out.insert(out.end(), blob.begin() + long(p), blob.begin() + long(p + csize));
            else if (!lzo1xDecompress(&blob[p], csize, out))
                return false;
            p += csize;
        }
        if (out.empty())
            out = std::move(blob); // stored raw, no chunks (the loading screen's images 0xF402E7E7 / 8: start with zero texels)
        return true;
    }}
