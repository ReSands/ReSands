// Differential check of recompiled IOP modules (tools/iop/iop_recomp.py output) against the interpreter, offline:
// loads PoP's libsd, sdrdrv and UbiSound_Driver from the ELF at the bases the game run uses, runs their start
// routines and threads for a stretch of EE time, once interpreted and once with PS2X_IOP_NATIVE=1, and compares
// all of IOP RAM, the IOP clock and every thread's state. Exit 0 = identical.
//
//   iop_native_check <SLUS_207.43> [EE seconds, default 2]
//   iop_native_check <SLUS_207.43> --sound <out dir>   PS2X_NATIVE_AUDIO=1 (native libsd + mixer) against the
//       reference, the real libsd on PCSX2's SPU2 core (tools/iop/pcsx2_ref), driven the EE's way (sceSdRemote =
//       sdrdrv RPC 0x80000701); the runtime's own emulated SPU2 is printed beside for information. A
//       looping ADPCM voice on core 0 (all 5 filters) keyed on and off, a PCM block input, a voice with exponential
//       envelope and a volume sweep, and UbiSound's reverb set-up (hall, echo, echo with effects switched off at the key off); writes pcsx2.wav, native.wav
//       and emulated.wav; passes when native matches PCSX2 in shape (correlation >= 0.999) and level (2 %) per
//       section. PCSX2 is the reference short of the real PS2 (docs/native_sound.md 8b).
//   iop_native_check <SLUS_207.43> --clock     PS2X_IOP_CLOCK=host: 1 s of wall time fed with EE cycles at the wrong
//       rate must still move the IOP clock by 1 s (within 2 %), a 0.5 s stall must be skipped, not replayed, and
//       after 5000 fast inline RPCs (run ahead) the IOP must keep real time, not wait for the PC to catch up.
//
// Build (ps2xIOP from the pop-sotc tree with patches 51-54; <gen> = the generated *_iop.cpp, game material, not in
// git; PCSX2's SPU2: python3 tools/iop/pcsx2_ref/build.py --iop-include <sotc>/ps2xIOP/include):
//   c++ -std=c++20 -O2 -I<sotc>/ps2xIOP/include -I<sotc>/ps2xIOP/src -Itools/iop tools/iop/iop_native_check.cpp
//       <gen>/*.cpp <build>/libps2_iop.a game/native-build/pcsx2_ref/libpcsx2_spu2.a -o game/native-build/iop_native_check
#include "emulator/iop_emulator.h"
#include "ps2x/iop/iop_host.h"

#include <chrono>
#include <cmath>
#include <tuple>
#include <cstdio>
#include <filesystem>
#include "pcsx2_ref/pcsx2_spu2_backend.h"
#include <map>
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
        void submitAudio(std::span<const int16_t> samples) override { audio.insert(audio.end(), samples.begin(), samples.end()); }

        std::vector<int16_t> audio; // 48 kHz stereo, as the IOP's sound output gave it

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
        // DARE polls RPC 9 (bank upload done?) in a loop while loading: each inline call adds IOP time.
        const auto p0 = clock::now();
        const uint64_t c2 = iop.cycles();
        RpcRequest poll;
        poll.sid = 0x12345678u;
        poll.function = 5u;
        for (int i = 0; i < 5000; ++i)
        {
            (void)iop.handleRpc(poll);
            iop.runEeCycles(2'000u);
        }
        const double pollWall = std::chrono::duration<double>(clock::now() - p0).count();
        std::printf("5000 polls: %.3f s wall -> %.3f s IOP\n", pollWall, double(iop.cycles() - c2) / 36'864'000.0);
        // EE cycles now at the real rate (294.912 MHz): 2 ms of wall time per 589,824 EE cycles.
        const uint64_t c3 = iop.cycles();
        const auto a0 = clock::now();
        for (int i = 0; i < 250; ++i)
        {
            iop.runEeCycles(589'824u);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const double afterWall = std::chrono::duration<double>(clock::now() - a0).count();
        const double afterIop = double(iop.cycles() - c3) / 36'864'000.0;
        std::printf("then %.3f s wall at the real EE rate: %.3f s IOP (no stall: >= 90 %%)\n", afterWall, afterIop);
        const bool ok = result.loaded && iopSeconds > wall * 0.98 && iopSeconds < wall * 1.02 && replayed < 0.13 &&
                        afterIop >= afterWall * 0.9;
        std::printf("%s\n", ok ? "clock ok" : "CLOCK WRONG");
        return ok ? 0 : 4;
    }

    // sceSdRemote as the EE makes it: sdrdrv's command number is the RPC function, arguments at +4, +8, ...
    struct SoundRun
    {
        std::vector<int16_t> audio;                    // 48 kHz stereo
        std::vector<std::tuple<std::string, size_t, size_t>> sections; // name, first and end frame
    };

    // PCSX2: the real libsd on PCSX2's SPU2 core (the reference); Native: PS2X_NATIVE_AUDIO=1; Emulated: the real
    // libsd on the runtime's own SPU2 model (what playtests used so far; for information only).
    enum class Path { Pcsx2, Native, Emulated };

    SoundRun soundRun(const std::vector<uint8_t> &elf, Path path)
    {
        SoundRun run;
        const bool nativeAudio = path == Path::Native;
        setenv("PS2X_NATIVE_AUDIO", nativeAudio ? "1" : "0", 1);
        ElfHost host(elf);
        IopEmulator iop(host);
        const std::unique_ptr<ps2x::iop::Spu2Backend> pcsx2 = path == Path::Pcsx2 ? pop::makePcsx2Spu2Backend() : nullptr;
        iop.setSpu2Backend(pcsx2.get());
        Result result;
        const char *label = path == Path::Pcsx2 ? "pcsx2" : nativeAudio ? "native" : "emulated";
        loadSoundModules(iop, result, label);
        auto runFor = [&](double seconds)
        {
            for (uint64_t done = 0, cycles = uint64_t(seconds * 294912000.0); done < cycles; done += 294912u)
                iop.runEeCycles(294912u);
        };
        constexpr uint32_t kSend = 0x7F0000u, kReceive = 0x7F8000u;
        auto sd = [&](uint32_t command, std::initializer_list<uint32_t> args)
        {
            uint32_t words[8] = {};
            size_t i = 1;
            for (uint32_t a : args)
                words[i++] = a;
            (void)host.writeGuest(kSend, words, sizeof(words));
            RpcRequest request;
            request.sid = 0x80000701u;
            request.function = command;
            request.send = {kSend, sizeof(words)};
            request.receive = {kReceive, 16u};
            (void)iop.handleRpc(request);
            uint32_t reply = 0;
            (void)host.readGuest(kReceive, &reply, 4u);
            runFor(0.002);
            return reply;
        };
        enum : uint32_t { kInit = 0x8000, kSetParam = 0x8010, kSetSwitch = 0x8030, kSetAddr = 0x8050, kSetCoreAttr = 0x8070,
                          kVoiceTrans = 0x80d0, kBlockTrans = 0x80e0, kVoiceTransStatus = 0x80f0, kSetEffectAttr = 0x8130,
                          kClearEffectWorkArea = 0x8150 };
        // Output frame n is IOP cycle n * 768 on both paths.
        size_t sectionStart = 0;
        auto begin = [&] { sectionStart = size_t(iop.cycles() / 768u); };
        auto end = [&](const char *name) { run.sections.emplace_back(name, sectionStart, size_t(iop.cycles() / 768u)); };
        sd(kInit, {0u});
        runFor(0.05);

        // A looping sample: 40 ADPCM blocks of a 28-sample sawtooth, cycling through the 5 filters and 3 shifts;
        // the first block marks the loop start, the last ends with repeat.
        std::vector<uint8_t> adpcm(40u * 16u, 0u);
        for (uint32_t b = 0; b < 40u; ++b)
        {
            uint8_t *block = &adpcm[b * 16u];
            block[0] = uint8_t(((b % 5u) << 4) | (2u + b % 3u));
            block[1] = b == 0u ? 4u : b == 39u ? 3u : 0u;
            for (uint32_t i = 0; i < 28u; ++i)
            {
                const uint8_t nibble = uint8_t((i * 16u / 28u) ^ 8u) & 0xFu;
                block[2u + i / 2u] |= uint8_t(nibble << ((i & 1u) * 4u));
            }
        }
        const uint32_t iopSample = iop.allocateMemory(uint32_t(adpcm.size()), 64u);
        (void)iop.writeMemory(iopSample, adpcm.data(), adpcm.size());
        constexpr uint32_t kSpuSample = 0x5000u;
        const uint32_t sent = sd(kVoiceTrans, {0u, 0u, iopSample, kSpuSample, uint32_t(adpcm.size())});
        runFor(0.02);
        const uint32_t status = sd(kVoiceTransStatus, {0u, 1u});
        std::printf("  %s: VoiceTrans %#x status %#x\n", label, sent, status);

        // Core 0: master and voice volumes, voice 0 to the dry output, the sample at a fifth above its rate.
        for (uint32_t core = 0; core < 2u; ++core)
        {
            sd(kSetParam, {0x0980u | core, 0x3FFFu});
            sd(kSetParam, {0x0A80u | core, 0x3FFFu});
        }
        sd(kSetSwitch, {0x1800u, 1u}); // VMIXL core 0: voice 0
        sd(kSetSwitch, {0x1A00u, 1u}); // VMIXR
        sd(kSetParam, {0x0000u, 0x3FFFu}); // VOLL voice 0
        sd(kSetParam, {0x0100u, 0x2000u}); // VOLR
        sd(kSetParam, {0x0200u, 0x1800u}); // PITCH
        sd(kSetParam, {0x0300u, 0x0A8Fu}); // ADSR1: attack linear rate 10, decay 8, sustain level 15
        sd(kSetParam, {0x0400u, 0x4C4Bu}); // ADSR2: sustain decreasing, rate 0x31; release linear rate 11
        sd(kSetAddr, {0x2040u, kSpuSample}); // SSA voice 0
        begin();
        sd(kSetSwitch, {0x1500u, 1u}); // KON
        runFor(0.4);
        sd(kSetSwitch, {0x1600u, 1u}); // KOFF
        runFor(0.2);
        end("voice");

        // A streamed input on core 0 (as DARE's stream): a looping 2-half PCM buffer, 1 kHz square, left only.
        std::vector<int16_t> pcm(2u * 2u * 512u, 0);
        for (uint32_t block = 0; block < 4u; ++block)
            for (uint32_t i = 0; i < 256u; ++i)
                pcm[block * 512u + i] = int16_t(((block * 256u + i) / 24u) % 2u ? 6000 : -6000);
        const uint32_t iopStream = iop.allocateMemory(uint32_t(pcm.size() * 2u), 64u);
        (void)iop.writeMemory(iopStream, pcm.data(), pcm.size() * 2u);
        sd(kSetParam, {0x0F80u, 0x7FFFu}); // BVOLL core 0
        sd(kSetParam, {0x1080u, 0x7FFFu}); // BVOLR
        begin();
        sd(kBlockTrans, {0u, 0x10u, iopStream, uint32_t(pcm.size() * 2u)});
        runFor(0.2);
        end("stream"); // before the stop: the SPU2 path plays its queued input on (32 samples), native stops at once
        sd(kBlockTrans, {0u, 2u, 0u, 0u});
        runFor(0.05);

        // Envelope and volume modes the banks can use: voice 1, exponential attack / sustain / release, the left
        // volume a linear sweep up, the right a fixed level.
        sd(kSetSwitch, {0x1800u, 2u});
        sd(kSetSwitch, {0x1A00u, 2u});
        sd(kSetParam, {0x0002u, 0x8050u}); // VOLL voice 1: sweep, linear increase, rate 0x50
        sd(kSetParam, {0x0102u, 0x3000u}); // VOLR
        sd(kSetParam, {0x0202u, 0x1000u}); // PITCH
        sd(kSetParam, {0x0302u, 0x8C8Bu}); // ADSR1: attack exponential rate 12, decay 8, sustain level 11
        sd(kSetParam, {0x0402u, 0xCC6Bu}); // ADSR2: sustain exponential decreasing rate 0x31; release exponential 11
        sd(kSetAddr, {0x2042u, kSpuSample});
        begin();
        sd(kSetSwitch, {0x1500u, 2u});
        runFor(0.4);
        sd(kSetSwitch, {0x1600u, 2u});
        runFor(0.3);
        end("envelope");

        // Reverb as UbiSound sets it (SND_fn_vPostInitEngineConfSfx, SPUDMAManager_Update, SND_fn_vSetEffectSfx):
        // core 0, effect end 0x1fffff, effect on, the work area cleared, the mode with its depths. Voice 2 goes to
        // the effect only (no dry), so the output is the reverb's return: during the note and its tail.
        sd(kSetSwitch, {0x1800u, 0u});
        sd(kSetSwitch, {0x1A00u, 0u});
        sd(kSetSwitch, {0x1900u, 4u}); // VMIXEL core 0: voice 2
        sd(kSetSwitch, {0x1B00u, 4u}); // VMIXER
        sd(kSetAddr, {0x1D00u, 0x1FFFFFu}); // EEA core 0
        sd(kSetCoreAttr, {2u, 1u});         // effect enable core 0
        sd(kSetParam, {0x0004u, 0x3FFFu});
        sd(kSetParam, {0x0104u, 0x3FFFu});
        sd(kSetParam, {0x0204u, 0x0C00u});
        sd(kSetParam, {0x0304u, 0x0A8Fu});
        sd(kSetParam, {0x0404u, 0x1FCBu}); // sustain held, release linear 11
        sd(kSetAddr, {0x2044u, kSpuSample});
        // sceSdSetEffectAttr reads its attribute from the RPC buffer: {core, mode, depth L | depth R << 16, delay,
        // feedback} at +0, +4, +8, +C, +10 (the helper fills from +4).
        // effectsOff: switched off at the key off; the reverb keeps running and sounding, but writes nothing back.
        auto effect = [&](const char *name, uint32_t mode, uint32_t delay, uint32_t feedback, bool effectsOff = false)
        {
            sd(kClearEffectWorkArea, {0u, 1u, mode});
            runFor(0.05);
            sd(kSetEffectAttr | 0u, {mode, 0x3FFFu | (0x3FFFu << 16), delay, feedback});
            runFor(0.02);
            begin();
            sd(kSetSwitch, {0x1500u, 4u});
            runFor(0.15);
            sd(kSetSwitch, {0x1600u, 4u});
            if (effectsOff)
                sd(kSetCoreAttr, {2u, 0u});
            runFor(0.8);
            end(name);
            if (effectsOff)
                sd(kSetCoreAttr, {2u, 1u});
        };
        effect("reverb hall", 5u, 0u, 0u);
        effect("reverb echo", 7u, 0x40u, 0x40u);
        effect("effects off", 7u, 0x40u, 0x40u, true);
        run.audio = std::move(host.audio);
        return run;
    }

    void writeWav(const std::string &path, const std::vector<int16_t> &stereo)
    {
        std::ofstream out(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
        auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
        const uint32_t bytes = uint32_t(stereo.size() * 2u);
        out.write("RIFF", 4); u32(36u + bytes); out.write("WAVEfmt ", 8); u32(16u); u16(1u); u16(2u); u32(48000u);
        u32(48000u * 4u); u16(4u); u16(16u); out.write("data", 4); u32(bytes);
        out.write(reinterpret_cast<const char *>(stereo.data()), bytes);
    }

    // Native against PCSX2 per section: level per 100 ms, then the best correlation of both channels over +-8
    // frames and the RMS ratio; pass: same shape (>= 0.999) and level (within 2 %). The emulated SPU2 is printed
    // beside them for information (it has no reverb or volume sweep).
    int soundCheck(const std::vector<uint8_t> &elf, const std::string &outDir)
    {
        const SoundRun refRun = soundRun(elf, Path::Pcsx2), nativeRun = soundRun(elf, Path::Native),
                       emuRun = soundRun(elf, Path::Emulated);
        const std::vector<int16_t> &ref = refRun.audio, &native = nativeRun.audio, &emu = emuRun.audio;
        std::filesystem::create_directories(outDir);
        writeWav(outDir + "/pcsx2.wav", ref);
        writeWav(outDir + "/native.wav", native);
        writeWav(outDir + "/emulated.wav", emu);
        const size_t frames = std::min({ref.size(), native.size(), emu.size()}) / 2u;
        std::printf("frames: pcsx2 %zu native %zu emulated %zu\n", ref.size() / 2u, native.size() / 2u, emu.size() / 2u);
        auto rms = [&](const std::vector<int16_t> &a, size_t from, size_t to, int channel)
        {
            double sum = 0;
            for (size_t i = from; i < to; ++i)
                sum += double(a[i * 2u + channel]) * a[i * 2u + channel];
            return to > from ? std::sqrt(sum / double(to - from)) : 0.0;
        };
        std::printf("window(10ms)  pcsx2 L/R      native L/R     emulated L/R\n");
        for (size_t w = 0; w + 480u <= frames; w += 4800u)
            std::printf("%6.2f s   %6.0f %6.0f   %6.0f %6.0f   %6.0f %6.0f\n", double(w) / 48000.0, rms(ref, w, w + 480u, 0),
                        rms(ref, w, w + 480u, 1), rms(native, w, w + 480u, 0), rms(native, w, w + 480u, 1),
                        rms(emu, w, w + 480u, 0), rms(emu, w, w + 480u, 1));
        bool ok = !refRun.sections.empty() && refRun.sections == nativeRun.sections;
        auto compare = [&](const std::vector<int16_t> &a, const std::vector<int16_t> &b, size_t first, size_t last)
        {
            const size_t i0 = first + 8u, i1 = std::min(last, frames - 8u);
            double best = -1.0, ratio = 0.0;
            int bestLag = 0;
            for (int lag = -8; lag <= 8; ++lag)
            {
                double xy = 0, xx = 0, yy = 0;
                for (size_t i = i0; i < i1; ++i)
                    for (size_t c = 0; c < 2u; ++c)
                    {
                        const double x = a[i * 2u + c], y = b[(i + lag) * 2u + c];
                        xy += x * y, xx += x * x, yy += y * y;
                    }
                const double c = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0.0;
                if (c > best)
                    best = c, bestLag = lag, ratio = xx > 0 ? std::sqrt(yy / xx) : 0.0;
            }
            return std::tuple{best, bestLag, ratio};
        };
        for (const auto &[name, first, last] : refRun.sections)
        {
            const auto [c, lag, ratio] = compare(ref, native, first, last);
            const auto [ce, lage, ratioe] = compare(ref, emu, first, last);
            const bool sectionOk = c >= 0.999 && ratio > 0.98 && ratio < 1.02;
            std::printf("%-12s %.2f-%.2f s: native %.5f lag %d level %.4f %s | emulated %.5f lag %d level %.4f\n",
                        name.c_str(), first / 48000.0, last / 48000.0, c, lag, ratio, sectionOk ? "ok" : "DIFFERS", ce, lage, ratioe);
            ok &= sectionOk;
        }
        std::printf("%s\n", ok ? "sound ok (native matches PCSX2)" : "SOUND DIFFERS (native vs PCSX2)");
        return ok ? 0 : 5;
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
    if (argc > 3 && std::string(argv[2]) == "--sound")
        return soundCheck(elf, argv[3]);
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
    if (std::getenv("CHECK_THREADS"))
        for (const std::string &thread : interp.threads)
            std::printf("  thread %s\n", thread.c_str());
    std::printf("%s (%zu threads)\n", differences ? "DIFFERENT" : "identical", interp.threads.size());
    return differences ? 3 : 0;
}
