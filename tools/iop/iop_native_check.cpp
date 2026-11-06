// Differential check of recompiled IOP modules (tools/iop/iop_recomp.py output) against the interpreter, offline:
// loads PoP's libsd, sdrdrv and UbiSound_Driver from the ELF at the bases the game run uses, runs their start
// routines and threads for a stretch of EE time, once interpreted and once with PS2X_IOP_NATIVE=1, and compares
// all of IOP RAM, the IOP clock and every thread's state. Exit 0 = identical.
//
//   iop_native_check <SLUS_207.43> [EE seconds, default 2]
//   iop_native_check <SLUS_207.43> --clock     PS2X_IOP_CLOCK=host: 1 s of wall time fed with EE cycles at the wrong
//       rate must still move the IOP clock by 1 s (within 2 %), and a 0.5 s stall must be skipped, not replayed.
//
// Build (ps2xIOP from the pop-sotc tree with patch 51; <gen> = the generated *_iop.cpp, game material, not in git):
//   c++ -std=c++20 -O2 -I<sotc>/ps2xIOP/include -I<sotc>/ps2xIOP/src tools/iop/iop_native_check.cpp <gen>/*.cpp
//       <build>/libps2_iop.a -o game/native-build/iop_native_check
#include "emulator/iop_emulator.h"
#include "ps2x/iop/iop_host.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ps2x::iop;
using ps2x::iop::detail::IopEmulator;

namespace
{
    // EE memory as the game has it: the ELF's PT_LOAD segments at their addresses (the IRX images live there).
    class ElfHost final : public IopHost
    {
    public:
        explicit ElfHost(const std::vector<uint8_t> &elf) : guest(8u << 20, 0u)
        {
            auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, elf.data() + at, 4); return v; };
            auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, elf.data() + at, 2); return v; };
            for (uint32_t i = 0; i < u16(44); ++i)
            {
                const size_t ph = u32(28) + size_t(i) * u16(42);
                const uint32_t offset = u32(ph + 4), vaddr = u32(ph + 8), filesz = u32(ph + 16);
                if (u32(ph) == 1 && vaddr + size_t(filesz) <= guest.size() && offset + size_t(filesz) <= elf.size())
                    std::memcpy(guest.data() + vaddr, elf.data() + offset, filesz);
            }
        }

        bool readGuest(uint32_t address, void *destination, size_t size) const override
        {
            if ((!destination && size) || address > guest.size() || size > guest.size() - address)
                return false;
            std::memcpy(destination, guest.data() + address, size);
            return true;
        }
        bool writeGuest(uint32_t address, const void *source, size_t size) override
        {
            if ((!source && size) || address > guest.size() || size > guest.size() - address)
                return false;
            std::memcpy(guest.data() + address, source, size);
            return true;
        }
        bool zeroGuest(uint32_t address, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address)
                return false;
            std::memset(guest.data() + address, 0, size);
            return true;
        }
        bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override
        {
            normalized = address & 0x1FFFFFFFu;
            return normalized < guest.size();
        }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}

        std::vector<uint8_t> guest;
    };

    struct Load
    {
        const char *name;
        uint32_t eeSource, base, span;
    };
    // From the game's own load log (16-17 Oct runs; see docs/native_sound.md step 3); span = the loader's allocation.
    constexpr Load kLoads[] = {{"libsd", 0x553740u, 0x77700u, 0x5a00u},
                               {"sdrdrv", 0x55a700u, 0x7f100u, 0x1b00u},
                               {"ubisound", 0x55c6c0u, 0x88f00u, 0xae00u}};

    struct Result
    {
        std::vector<uint8_t> ram;
        uint64_t cycles = 0, instructions = 0;
        std::vector<std::string> threads;
        bool loaded = true;
    };

    void loadSoundModules(IopEmulator &iop, Result &result, const char *label)
    {
        // First-fit heap: fill everything below each module and hold its slot, then free a slot just before its
        // module loads, so each lands where the game has it and the start routines' allocations go above them all.
        uint32_t slots[std::size(kLoads)] = {};
        uint32_t cursor = iop.allocateMemory(16u);
        (void)iop.freeMemory(cursor);
        for (size_t i = 0; i < std::size(kLoads); ++i)
        {
            if (cursor < kLoads[i].base && iop.allocateMemory(kLoads[i].base - cursor) != cursor)
                result.loaded = false;
            slots[i] = iop.allocateMemory(kLoads[i].span);
            result.loaded &= slots[i] == kLoads[i].base;
            cursor = kLoads[i].base + kLoads[i].span;
        }
        for (size_t i = 0; i < std::size(kLoads); ++i)
        {
            (void)iop.freeMemory(slots[i]);
            const ModuleLoadResult loaded = iop.loadModuleBuffer(kLoads[i].eeSource, nullptr, 0u);
            std::printf("  %s %s: id %d start %d\n", label, kLoads[i].name, loaded.moduleId, loaded.startResult);
            result.loaded &= loaded.moduleId > 0;
        }
    }

    Result run(const std::vector<uint8_t> &elf, bool native, double seconds)
    {
        setenv("PS2X_IOP_NATIVE", native ? "1" : "0", 1);
        ElfHost host(elf);
        IopEmulator iop(host);
        Result result;
        loadSoundModules(iop, result, native ? "native" : "interp");
        const uint64_t eeCycles = static_cast<uint64_t>(seconds * 294912000.0);
        auto runFor = [&](uint64_t cycles)
        {
            for (uint64_t done = 0; done < cycles; done += 294912u)
                iop.runEeCycles(294912u);
        };
        // DARE's side of the conversation (UbiSound RPC 0x12345678, SND_fn_pvRPC_IOPServerFunc): 0 init with the
        // version string, 3 post-init config (reverb on, biggest reverb, 4 IOP stream voices), 2 voices wished,
        // 5 free memory, 6 voice status, 7 an empty command stack. Arguments as a plausible game would send them;
        // the point is the same inputs both ways, not the game's exact values.
        constexpr uint32_t kSend = 0x7F0000u, kReceive = 0x7F8000u;
        auto rpc = [&](uint32_t function, const void *data, uint32_t size)
        {
            if (size)
                (void)host.writeGuest(kSend, data, size);
            RpcRequest request;
            request.sid = 0x12345678u;
            request.function = function;
            request.send = {kSend, size};
            request.receive = {kReceive, 64u};
            const RpcResult reply = iop.handleRpc(request);
            std::printf("  %s rpc %u: handled %d result %#x\n", native ? "native" : "interp", function, reply.handled, reply.resultAddress);
            runFor(eeCycles / 8u);
        };
        static const char kVersion[16] = "UbiSound_Driver";
        const uint32_t config[4] = {1u, 0x10000u, 4u, 0u};
        rpc(0u, kVersion, sizeof(kVersion));
        rpc(3u, config, sizeof(config));
        rpc(2u, nullptr, 0u);
        rpc(5u, nullptr, 0u);
        rpc(6u, nullptr, 0u);
        rpc(7u, nullptr, 0u);
        runFor(eeCycles);
        result.ram.resize(2u << 20);
        (void)iop.readMemory(0u, result.ram.data(), result.ram.size());
        result.cycles = iop.cycles();
        result.instructions = iop.instructions();
        result.threads = iop.describeThreads();
        return result;
    }

    // PS2X_IOP_CLOCK=host: the IOP's clock follows wall time, not the EE cycles it is fed.
    int clockCheck(const std::vector<uint8_t> &elf)
    {
        setenv("PS2X_IOP_CLOCK", "host", 1);
        ElfHost host(elf);
        IopEmulator iop(host);
        Result result;
        loadSoundModules(iop, result, "host");
        RpcRequest init;
        init.sid = 0x12345678u;
        static const char kVersion[16] = "UbiSound_Driver";
        (void)host.writeGuest(0x7F0000u, kVersion, sizeof(kVersion));
        init.send = {0x7F0000u, sizeof(kVersion)};
        (void)iop.handleRpc(init); // starts the 180 Hz timer and the update thread

        using clock = std::chrono::steady_clock;
        // EE cycles at a third of the real rate (a slowdown): 2 ms of wall time per 0.68 ms of EE cycles.
        auto feed = [&](double wallSeconds)
        {
            const auto end = clock::now() + std::chrono::duration<double>(wallSeconds);
            while (clock::now() < end)
            {
                iop.runEeCycles(200'000u);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        feed(0.05); // anchor
        const uint64_t c0 = iop.cycles();
        const auto t0 = clock::now();
        feed(1.0);
        const double wall = std::chrono::duration<double>(clock::now() - t0).count();
        const double iopSeconds = double(iop.cycles() - c0) / 36'864'000.0;
        std::printf("host clock: %.3f s wall -> %.3f s IOP (%.1f %%)\n", wall, iopSeconds, 100.0 * iopSeconds / wall);

        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // a PC stall: nothing feeds the IOP
        const uint64_t c1 = iop.cycles();
        feed(0.01);
        const double replayed = double(iop.cycles() - c1) / 36'864'000.0;
        std::printf("after a 0.5 s stall: %.3f s replayed (cap 0.1 s + the 10 ms fed)\n", replayed);
        const bool ok = result.loaded && iopSeconds > wall * 0.98 && iopSeconds < wall * 1.02 && replayed < 0.13;
        std::printf("%s\n", ok ? "clock ok" : "CLOCK WRONG");
        return ok ? 0 : 4;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <SLUS_207.43> [EE seconds]\n", argv[0]);
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<uint8_t> elf((std::istreambuf_iterator<char>(in)), {});
    if (argc > 2 && std::string(argv[2]) == "--clock")
        return clockCheck(elf);
    const double seconds = argc > 2 ? std::atof(argv[2]) : 2.0;

    const Result interp = run(elf, false, seconds);
    const Result native = run(elf, true, seconds);
    int differences = 0;
    if (!interp.loaded || !native.loaded)
    {
        std::printf("modules did not load at their bases\n");
        return 2;
    }
    std::printf("interp: %llu instructions, %llu cycles; native: %llu instructions, %llu cycles\n",
                (unsigned long long)interp.instructions, (unsigned long long)interp.cycles,
                (unsigned long long)native.instructions, (unsigned long long)native.cycles);
    if (interp.cycles != native.cycles || interp.instructions != native.instructions)
        ++differences;
    for (size_t i = 0; i < interp.ram.size(); ++i)
        if (interp.ram[i] != native.ram[i] && differences++ < 20)
            std::printf("RAM %06zx: interp %02x native %02x\n", i, interp.ram[i], native.ram[i]);
    if (interp.threads != native.threads)
    {
        ++differences;
        for (size_t i = 0; i < std::max(interp.threads.size(), native.threads.size()); ++i)
            std::printf("thread %zu: %s | %s\n", i, i < interp.threads.size() ? interp.threads[i].c_str() : "-",
                        i < native.threads.size() ? native.threads[i].c_str() : "-");
    }
    std::printf("%s (%zu threads)\n", differences ? "DIFFERENT" : "identical", interp.threads.size());
    return differences ? 3 : 0;
}
