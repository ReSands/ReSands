// Offline stand-in for the runtime's PS2X_IOP_DUMP_MODULES: loads IRX modules embedded in an EE ELF through
// ps2xIOP's own loader (same bytes, same relocation) at the bases the game run logged, and writes
// <out>/<name>_<base>.bin + .json, the input of tools/iop/iop_recomp.py.
//
//   iop_dump_modules <SLUS_207.43> <out dir> <name>:<EE source>:<IOP base> ...
//
// Bases come from a run's "[IOP] loaded IRX ... base=" lines. Since pop-sotc patch 10 the loader takes a module's
// memory from the IOP heap (first fit from HeapBase), so [HeapBase, base) is reserved first to land it there.
// Build against the pop-sotc tree with the patches applied (~/tools/PS2Recomp-sotc), not an unpatched clone. The hash is FNV-1a 64 over the image readElf returns, as the
// runtime hashes the image it loads; a wrong base or hash only means the native code is not activated.
//
// Build (after building ps2xIOP: cmake -S <runtime-src>/ps2xIOP -B <build>):
//   c++ -std=c++20 -O2 -I<runtime-src>/ps2xIOP/include -I<runtime-src>/ps2xIOP/src tools/iop/iop_dump_modules.cpp
//       <build>/libps2_iop.a -o game/native-build/iop_dump_modules
#include "emulator/core/iop_memory.h"
#include "emulator/services/iop_module_loader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ps2x::iop::detail;

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <elf> <out dir> <name>:<ee source>:<iop base> ...\n", argv[0]);
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<uint8_t> elf((std::istreambuf_iterator<char>(in)), {});
    if (elf.size() < 0x34 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0)
    {
        std::fprintf(stderr, "%s: not an ELF\n", argv[1]);
        return 1;
    }
    // EE address -> file offset through the ELF's PT_LOAD segments.
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, elf.data() + at, 4); return v; };
    auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, elf.data() + at, 2); return v; };
    const uint32_t phoff = u32(28), phnum = u16(44), phentsize = u16(42);
    IopModuleLoader::MemoryReader reader = [&](uint32_t address, void *destination, size_t size)
    {
        for (uint32_t i = 0; i < phnum; ++i)
        {
            const size_t ph = phoff + size_t(i) * phentsize;
            const uint32_t type = u32(ph), offset = u32(ph + 4), vaddr = u32(ph + 8), filesz = u32(ph + 16);
            if (type == 1 && address >= vaddr && address + size <= uint64_t(vaddr) + filesz && offset + (address - vaddr) + size <= elf.size())
            {
                std::memcpy(destination, elf.data() + offset + (address - vaddr), size);
                return true;
            }
        }
        return false;
    };

    int failures = 0;
    for (int a = 3; a < argc; ++a)
    {
        const std::string spec = argv[a];
        const size_t c1 = spec.find(':'), c2 = spec.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos)
        {
            std::fprintf(stderr, "bad spec %s\n", spec.c_str());
            return 1;
        }
        const std::string name = spec.substr(0, c1);
        const uint32_t source = std::stoul(spec.substr(c1 + 1, c2 - c1 - 1), nullptr, 0);
        const uint32_t base = std::stoul(spec.substr(c2 + 1), nullptr, 0);

        std::vector<uint8_t> image;
        IopMemory memory;
        if (!IopModuleLoader::readElf(reader, source, image))
        {
            std::fprintf(stderr, "%s: no IRX at EE %#x\n", name.c_str(), source);
            ++failures;
            continue;
        }
        if (base > IopMemory::HeapBase && memory.allocate(base - IopMemory::HeapBase, 16u, IopMemory::HeapBase) == 0u)
        {
            std::fprintf(stderr, "%s: cannot reserve below %#x\n", name.c_str(), base);
            ++failures;
            continue;
        }
        const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, base);
        if (!loaded || loaded.base != base || !loaded.relocationsComplete)
        {
            std::fprintf(stderr, "%s: load failed or moved (error %d, base %#x, relocations %s)\n", name.c_str(), int(loaded.error), loaded.base,
                         loaded.relocationsComplete ? "ok" : "incomplete");
            ++failures;
            continue;
        }
        uint64_t hash = 1469598103934665603ull;
        for (const uint8_t byte : image)
            hash = (hash ^ byte) * 1099511628211ull;
        std::vector<uint8_t> bytes(loaded.size);
        if (!memory.readRam(loaded.base, bytes.data(), bytes.size()))
        {
            ++failures;
            continue;
        }
        char stem[1024];
        std::snprintf(stem, sizeof(stem), "%s/%s_%06x", argv[2], name.c_str(), loaded.base);
        std::ofstream(std::string(stem) + ".bin", std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        std::ofstream json(std::string(stem) + ".json");
        char line[512];
        std::snprintf(line, sizeof(line), "{\"name\": \"%s\", \"base\": %u, \"size\": %u, \"entry\": %u, \"gp\": %u, \"hash\": \"%016llx\"}\n",
                      name.c_str(), loaded.base, loaded.size, loaded.entry, loaded.gp, static_cast<unsigned long long>(hash));
        json << line;
        std::printf("%s: base %#x size %#x entry %#x gp %#x image %zu bytes hash %016llx\n", name.c_str(), loaded.base,
                    loaded.size, loaded.entry, loaded.gp, image.size(), static_cast<unsigned long long>(hash));
    }
    return failures ? 2 : 0;
}
