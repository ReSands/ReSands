// Prints "<key> <format> <w>x<h> <FNV-1a 64 of RGBA>" for every texture record of an unpacked pack, or
// "<key> <format> fail": compared with the same listing from tools/jadetex.py (see tools/native/assets_check.py).
#include "assets.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <unpacked pack.bin>\n", argv[0]);
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d(std::istreambuf_iterator<char>(f), {});
    pop_assets::Pack pack;
    if (!pop_assets::indexPack(d.data(), d.size(), pack))
        return 1;
    std::vector<uint32_t> keys;
    for (const auto &r : pack.records)
        if (pop_assets::isTexture(pack, r.first))
            keys.push_back(r.first);
    std::sort(keys.begin(), keys.end());
    for (uint32_t k : keys)
    {
        pop_native::Image img;
        const uint32_t fmt = pop_assets::textureFormat(pack, k);
        if (!pop_assets::decodeTexture(pack, k, img))
        {
            std::printf("%08x %u fail\n", k, fmt);
            continue;
        }
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint8_t b : img.rgba)
            h = (h ^ b) * 0x100000001b3ull;
        std::printf("%08x %u %ux%u %016" PRIx64 "\n", k, fmt, img.width, img.height, h);
    }
    return 0;
}
