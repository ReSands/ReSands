// Judge driver for the mesh decoder port: runs the pop_mesh functions on every job of a jobs file
// and prints the canonical text that tools/llm_port/meshdecode/reference.py prints for the Python
// reference. Not written by the model.
// Usage: driver <unpacked pack> <jobs file>   (jobs: one element offset per line, decimal)
#include "meshdecode.h"
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

using namespace pop_mesh;

static void hexbytes(const std::vector<uint8_t> &v)
{
    for (uint8_t b : v)
        std::printf("%02x", b);
}
static void f32(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    std::printf(" %08x", u);
}
static void f64(double f)
{
    uint64_t u;
    std::memcpy(&u, &f, 8);
    std::printf(" %016" PRIx64, u);
}
static void batches(const std::vector<Batch> &bs)
{
    for (const Batch &b : bs)
    {
        std::printf(" B");
        for (const auto &[fmt, u] : b)
        {
            std::printf(" %x:%x:%u:", fmt, u.addr, u.count);
            hexbytes(u.raw);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> pack((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint8_t *d = pack.data();
    const size_t len = pack.size();
    std::ifstream jobs(argv[2]);
    std::string line;
    while (std::getline(jobs, line))
    {
        if (line.empty())
            continue;
        const uint32_t o = static_cast<uint32_t>(std::stoul(line));
        std::printf("J %u\n", o);
        ElementParts parts;
        if (!elementParts(d, len, o, parts))
        {
            std::printf("parts X\n");
            continue;
        }
        std::printf("parts %u", parts.boneCount);
        for (const Block &b : parts.lods)
            std::printf(" L%u:%u:%u", b.index, b.offset, b.size);
        for (const Block &b : parts.passes)
            std::printf(" P%u:%u:%u", b.index, b.offset, b.size);
        std::printf("\n");
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> blobs;
        for (const Block &b : parts.lods)
            blobs[b.index] = {b.offset, b.size};
        for (const Block &l : parts.lods)
        {
            if (l.index != 3)
                continue;
            std::vector<Variant3Batch> v3;
            std::printf("v3 %u", l.offset);
            if (!variant3Batches(d, len, l.offset, l.size, v3))
            {
                std::printf(" X\n");
                continue;
            }
            for (const Variant3Batch &b : v3)
            {
                std::printf(" B");
                for (float x : b.positions)
                    f32(x);
                std::printf(" |");
                for (int16_t x : b.uvs)
                    std::printf(" %d", x);
                std::printf(" |");
                for (int16_t x : b.flags)
                    std::printf(" %d", x);
            }
            std::printf("\n");
        }
        for (const Block &p : parts.passes)
        {
            for (int withBlobs = 0; withBlobs < 2; ++withBlobs)
            {
                std::vector<Batch> bs;
                std::printf("pkt %u %d", p.offset, withBlobs);
                if (!decodePacket(d, len, p.offset, p.size, withBlobs ? &blobs : nullptr, bs))
                {
                    std::printf(" X\n");
                    continue;
                }
                batches(bs);
                std::printf("\n");
                if (!withBlobs)
                    continue;
                for (double scale : {4096.0, 512.0})
                    for (const Batch &b : bs)
                    {
                        Mesh m;
                        std::printf("mesh %g", scale);
                        if (!batchMesh(b, scale, m))
                        {
                            std::printf(" X\n");
                            continue;
                        }
                        for (float x : m.positions)
                            f32(x);
                        std::printf(" |");
                        for (double x : m.uvs)
                            f64(x);
                        std::printf(" |");
                        for (double x : m.colours)
                            f64(x);
                        std::printf(" |");
                        for (uint32_t x : m.tris)
                            std::printf(" %u", x);
                        std::printf("\n");
                    }
            }
            std::vector<SkinnedBatch> sk;
            std::printf("skin %u", p.offset);
            if (!skinnedBatches(d, len, p.offset, p.size, sk))
            {
                std::printf(" X\n");
                continue;
            }
            for (const SkinnedBatch &b : sk)
            {
                std::printf(" B");
                for (float x : b.positions)
                    f32(x);
                std::printf(" |");
                for (int32_t x : b.uvs)
                    std::printf(" %d", x);
                std::printf(" |");
                for (int32_t x : b.flags)
                    std::printf(" %d", x);
                std::printf(" |");
                for (const auto &t : b.bones)
                {
                    std::printf(" (");
                    for (int32_t x : t)
                        std::printf("%d,", x);
                    std::printf(")");
                }
                std::printf(" |");
                for (float x : b.weights)
                    f32(x);
                std::printf(" |");
                if (b.hasNormals)
                    for (int32_t x : b.normals)
                        std::printf(" %d", x);
                else
                    std::printf(" None");
                std::printf(" |");
                if (b.hasColours)
                    for (double x : b.colours)
                        f64(x);
                else
                    std::printf(" None");
            }
            std::printf("\n");
        }
    }
    return 0;
}
