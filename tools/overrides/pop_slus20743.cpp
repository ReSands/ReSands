// Game overrides for Prince of Persia: The Sands of Time, US v1.01 (SLUS_207.43).
// rebuild.sh copies this into ps2xRuntime/src/runner/ so it links into the
// executable: overrides compiled into the ps2_runtime static library are
// dropped by the linker and never apply (upstream issue #264).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include <ps2_recompiled_functions.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

namespace
{
    constexpr uint32_t kEntry = 0x00100008u;
    constexpr uint32_t kElfCrc32 = 0x7E7889A9u; // whole-file CRC-32 of SLUS_207.43

    // sceCdLayerSearchFile(sceCdlFILE *fp, const char *name, int layer). The game's
    // own copy waits on the IOP CD driver (and is flagged as self-modifying), so it
    // spins forever. The runtime's sceCdSearchFile takes the same fp/name in a0/a1;
    // the layer in a2 does not matter for this single-layer disc.
    constexpr uint32_t kSceCdLayerSearchFile = 0x00101FA8u;

    // sceCdDiskReady(int mode), not matched by ps2_analyzer's signature scan. Identified
    // from its body: binds the cdvdfsv "disk ready" RPC server 0x8000059A, returns 6
    // (not ready) on failure. Without an IOP CD server the bind loop never ends.
    constexpr uint32_t kSceCdDiskReady = 0x00102A48u;

    // ---- File-system tracing (temporary, for the "Virtual path not found" exception) ----
    // ubi FileManager (object at a0): mount(path, &device) 0x153940,
    // alias(from, to) 0x153140, resolve(path, out) 0x152080 returning 0 when found
    // and 5 when no mount matches, open(path, mode) 0x152b40 (throws
    // ubi::FileNotFoundException when resolve fails).
    std::string guestString(uint8_t *rdram, uint32_t addr)
    {
        std::string s;
        for (uint32_t i = 0; addr != 0u && i < 256u; ++i)
        {
            const char c = static_cast<char>(*getConstMemPtr(rdram, addr + i));
            if (c == '\0')
                break;
            s += c;
        }
        return s;
    }

    uint32_t guestU32(uint8_t *rdram, uint32_t addr)
    {
        return addr ? *reinterpret_cast<const uint32_t *>(getConstMemPtr(rdram, addr)) : 0u;
    }
    std::string hex(uint32_t v)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%x", v);
        return buf;
    }

    // Calls the recompiled original; reports v0 only if it returned normally (a guest
    // thread switch inside it leaves pc elsewhere and the call resumes later).
    template <PS2Runtime::RecompiledFunction Original>
    void traced(const char *what, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &args)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        std::cerr << "[pop:fs] " << what << "(" << args << ")" << std::endl;
        Original(rdram, ctx, runtime);
        if (ctx->pc == ra)
            std::cerr << "[pop:fs] " << what << " -> " << static_cast<int32_t>(getRegU32(ctx, 2)) << std::endl;
    }

    void traceMount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t devPtr = getRegU32(ctx, 6);
        const uint32_t device = devPtr ? *reinterpret_cast<const uint32_t *>(getConstMemPtr(rdram, devPtr)) : 0u;
        traced<FUN_00153940_0x153940>("mount", rdram, ctx, runtime,
            "\"" + guestString(rdram, getRegU32(ctx, 5)) + "\", device=" + hex(device));
    }
    void traceAlias(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        traced<FUN_00153140_0x153140>("alias", rdram, ctx, runtime,
            "\"" + guestString(rdram, getRegU32(ctx, 5)) + "\" -> \"" + guestString(rdram, getRegU32(ctx, 6)) + "\"");
    }
    void traceResolve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        traced<FUN_00152080_0x152080>("resolve", rdram, ctx, runtime,
            "\"" + guestString(rdram, getRegU32(ctx, 5)) + "\"");
    }
    void traceOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        traced<FUN_00152b40_0x152b40>("open", rdram, ctx, runtime,
            "\"" + guestString(rdram, getRegU32(ctx, 5)) + "\", mode=" + std::to_string(getRegU32(ctx, 6)));
    }

    // CD device constructor (0x14f380), called by FUN_00488a00 on memory from the type-4
    // allocator (singleton pointer at 0x6803b0, allocate = vtable+0xc).
    void traceCdDeviceCtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t alloc = guestU32(rdram, 0x006803b0u);
        const uint32_t vtbl = guestU32(rdram, alloc);
        traced<FUN_0014f380_0x14f380>("cdDeviceCtor", rdram, ctx, runtime,
            "this=" + hex(getRegU32(ctx, 4)) + ", allocator=" + hex(alloc) + ", vtbl=" + hex(vtbl) +
            ", alloc fn=" + hex(guestU32(rdram, vtbl + 0xc)));
    }

    // C library heap: malloc 0x100ac0 (wrapped by the type-4 allocator at 0x1399c0) and
    // the heap-growing helper 0x101880 (sbrk-like). Logged a limited number of times.
    int g_heapTraceBudget = 40;
    // ---- Heap debugger: ring buffer of the last malloc/free calls, dumped (once) with the
    // allocator state when malloc first returns a pointer below the heap start. ----
    constexpr uint32_t kHeapStart = 0x006E7000u;
    struct HeapEvent { char kind; uint32_t arg, result, caller, chunkSize; };
    HeapEvent g_heapRing[48];
    uint32_t g_heapRingCount = 0;
    bool g_heapDumped = false;

    void heapRecord(char kind, uint32_t arg, uint32_t result, uint32_t caller, uint32_t chunkSize)
    {
        g_heapRing[g_heapRingCount++ % 48] = {kind, arg, result, caller, chunkSize};
    }

    void heapDump(uint8_t *rdram)
    {
        g_heapDumped = true;
        std::cerr << "[pop:heap] FIRST BAD MALLOC - last " << std::min<uint32_t>(g_heapRingCount, 48) << " heap calls:" << std::endl;
        const uint32_t n = std::min<uint32_t>(g_heapRingCount, 48);
        for (uint32_t i = 0; i < n; ++i)
        {
            const HeapEvent &e = g_heapRing[(g_heapRingCount - n + i) % 48];
            std::cerr << "[pop:heap]   " << (e.kind == 'm' ? "malloc(" : "free(") << hex(e.arg) << ")"
                      << (e.kind == 'm' ? " -> " + hex(e.result) : " chunkSize=" + hex(e.chunkSize))
                      << " caller " << hex(e.caller) << std::endl;
        }
        // Allocator state (dlmalloc-style, base 0x675280): max_fast/flags, fastbins 0x675284..,
        // top 0x6752b0, last_remainder 0x6752b4, unsorted bin fd/bk 0x6752b8/0x6752c4.
        std::cerr << "[pop:heap]   max_fast=" << hex(guestU32(rdram, 0x675280)) << " top=" << hex(guestU32(rdram, 0x6752b0))
                  << " last_rem=" << hex(guestU32(rdram, 0x6752b4)) << " unsorted.fd=" << hex(guestU32(rdram, 0x6752c0))
                  << " unsorted.bk=" << hex(guestU32(rdram, 0x6752c4)) << std::endl;
        std::cerr << "[pop:heap]   fastbins:";
        for (uint32_t i = 0; i < 10; ++i)
            std::cerr << " " << hex(guestU32(rdram, 0x675284 + i * 4));
        std::cerr << std::endl;
    }

    // Walks chunk headers from the heap start through the first 64 KB (or to top) and
    // reports, once, the first heap call at which the chain is broken: a size that is
    // zero, misaligned, or runs past top. Chunk layout: [prev_size][size|flags][data...].
    bool g_walkBroken = false;
    const char *g_lastCallName = "";   // last instrumented kernel/HLE call (see g_watched)
    uint32_t g_lastCallCaller = 0;
    uint32_t g_lastGoodEvent = 0;
    void heapWalkCheck(uint8_t *rdram, const char *where, uint32_t caller)
    {
        if (g_walkBroken)
            return;
        const uint32_t top = guestU32(rdram, 0x6752b0);
        if (top < kHeapStart)
            return; // heap not set up yet
        // First chunk: sbrk returns 0x6E7000 and the allocator aligns its first chunk to +8.
        uint32_t chunk = kHeapStart + 8u, prev = 0;
        for (int steps = 0; chunk < top && chunk < kHeapStart + 0x10000u && steps < 4096; ++steps)
        {
            const uint32_t size = guestU32(rdram, chunk + 4) & ~3u;
            if (size < 0x10u || (size & 7u) != 0u || chunk + size > top)
            {
                g_walkBroken = true;
                std::cerr << "[pop:heap] HEAP WALK BROKEN at " << where << " (heap call #" << g_heapRingCount
                          << ", caller " << hex(caller) << "): chunk " << hex(chunk) << " size field "
                          << hex(guestU32(rdram, chunk + 4)) << ", previous chunk " << hex(prev)
                          << "; last good check at heap call #" << g_lastGoodEvent
                          << "; last kernel/HLE call before this: " << g_lastCallName << " from " << hex(g_lastCallCaller) << std::endl;
                std::cerr << "[pop:heap]   bytes around: ";
                for (uint32_t a = (prev ? prev : chunk) ; a < chunk + 0x20u; a += 4)
                    std::cerr << hex(a) << "=" << hex(guestU32(rdram, a)) << " ";
                std::cerr << std::endl;
                const bool dumped = g_heapDumped;
                heapDump(rdram);
                g_heapDumped = dumped; // keep the bad-malloc dump available
                return;
            }
            prev = chunk;
            chunk += size;
        }
        g_lastGoodEvent = g_heapRingCount;
    }

    void traceMalloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t size = getRegU32(ctx, 4);
        const uint32_t ra = getRegU32(ctx, 31);
        heapWalkCheck(rdram, "malloc entry", ra);
        FUN_00100ac0_0x100ac0(rdram, ctx, runtime);
        if (ctx->pc != ra)
            return;
        const uint32_t result = getRegU32(ctx, 2);
        heapRecord('m', size, result, ra, 0);
        if (!g_heapDumped && result != 0u && result < kHeapStart)
            heapDump(rdram);
    }

    void traceFree(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ptr = getRegU32(ctx, 4);
        const uint32_t chunkSize = ptr >= 4u ? guestU32(rdram, ptr - 4u) : 0u;
        heapWalkCheck(rdram, "free entry", getRegU32(ctx, 31));
        heapRecord('f', ptr, 0, getRegU32(ctx, 31), chunkSize);
        FUN_001008c0_0x1008c0(rdram, ctx, runtime);
    }
    void traceSbrk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_heapTraceBudget-- <= 0) { FUN_00101880_0x101880(rdram, ctx, runtime); return; }
        traced<FUN_00101880_0x101880>("sbrk", rdram, ctx, runtime, hex(getRegU32(ctx, 4)));
    }

    // Writes into the start of the heap (where the first chunk headers live) by the C
    // library's memset/memcpy/memmove, which the runtime replaces with HLE versions.
    // The originals are looked up at apply time and called through.
    PS2Runtime::RecompiledFunction g_origMemset = nullptr, g_origMemcpy = nullptr, g_origMemmove = nullptr;
    int g_heapWriteReports = 30;
    void reportHeapStartWrite(const char *what, R5900Context *ctx)
    {
        const uint32_t dst = getRegU32(ctx, 4), n = getRegU32(ctx, 6);
        if (g_heapWriteReports > 0 && dst < kHeapStart + 0x100u && dst + n > kHeapStart)
        {
            --g_heapWriteReports;
            std::cerr << "[pop:heap] " << what << "(dst=" << hex(dst) << ", " << hex(getRegU32(ctx, 5)) << ", n=" << hex(n)
                      << ") touches heap start; caller " << hex(getRegU32(ctx, 31)) << ", heap call #" << g_heapRingCount << std::endl;
        }
    }
    void guardMemset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        reportHeapStartWrite("memset", ctx);
        g_origMemset(rdram, ctx, runtime);
    }
    void guardMemcpy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        reportHeapStartWrite("memcpy", ctx);
        g_origMemcpy(rdram, ctx, runtime);
    }
    void guardMemmove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        reportHeapStartWrite("memmove", ctx);
        g_origMemmove(rdram, ctx, runtime);
    }

    // Kernel/HLE calls instrumented with the heap walk, so the walk runs between almost
    // every step and the event ring shows the last calls before the heap broke.
    struct WatchedCall { uint32_t addr; const char *name; PS2Runtime::RecompiledFunction orig; };
    WatchedCall g_watched[] = {
        {0x001024E8u, "sceCdSync", nullptr},      {0x00102D40u, "sceCdRead", nullptr},
        {0x00108920u, "SetAlarm", nullptr},       {0x001089A0u, "CreateThread", nullptr},
        {0x001089C0u, "StartThread", nullptr},    {0x00108A30u, "ChangeThreadPriority", nullptr},
        {0x00108A90u, "GetThreadId", nullptr},    {0x00108AA0u, "ReferThreadStatus", nullptr},
        {0x00108AC0u, "SleepThread", nullptr},    {0x00108AD0u, "WakeupThread", nullptr},
        {0x00108BA0u, "CreateSema", nullptr},     {0x00108BC0u, "SignalSema", nullptr},
        {0x00108BE0u, "WaitSema", nullptr},       {0x00108BF0u, "PollSema", nullptr},
        {0x00108E20u, "FlushCache", nullptr},     {0x00108F60u, "sceSifSetDma", nullptr},
        {0x0010BD40u, "sceSifCallRpc", nullptr},
    };
    constexpr size_t kWatchedCount = sizeof(g_watched) / sizeof(g_watched[0]);
    template <size_t I>
    void watchedCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t caller = getRegU32(ctx, 31);
        heapWalkCheck(rdram, g_watched[I].name, caller);
        g_lastCallName = g_watched[I].name;
        g_lastCallCaller = caller;
        g_watched[I].orig(rdram, ctx, runtime);
    }
    template <size_t... I>
    void installWatched(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        ((g_watched[I].orig = runtime.lookupFunction(g_watched[I].addr),
          g_watched[I].orig ? (void)runtime.replaceFunction(g_watched[I].addr, watchedCall<I>) : (void)0), ...);
    }

    // ---- Keep the runtime's own guest allocations out of the game's heap ----
    // HLE functions (sceGsExecLoadImage's GS packet, thread stacks, SIF, MPEG, fonts...)
    // allocate scratch memory with runtime->guestMalloc(), whose region the runtime sets
    // from the game's SetupHeap(0x6E6700, -1) call. This game runs its own dlmalloc over
    // exactly that memory (arena from 0x6E7000), so the runtime's scratch writes corrupt
    // the game's heap: the 48 KB "Loading" image upload at boot zeroed the first chunk
    // headers, which led to the CD device at a garbage address, a failed /cdroms/ mount
    // and the uncaught FileNotFoundException.
    //
    // crt0 calls SetupHeap as a raw syscall, then _InitSys (0x111C68). Wrapping _InitSys
    // moves the runtime heap to 0x80000-0x100000 right after SetupHeap: on hardware that
    // is kernel memory, here it is unused (code starts at 0x100000, the game's heap at
    // 0x6E7000). EndOfHeap (0x108B80), which the runtime answers from its heap limit,
    // keeps returning the default 0x1F00000.
    constexpr uint32_t kRuntimeHeapBase = 0x00080000u;
    constexpr uint32_t kRuntimeHeapLimit = 0x00100000u;
    constexpr uint32_t kGameHeapEnd = 0x01F00000u;

    void initSysWithRuntimeHeapMoved(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->configureGuestHeap(kRuntimeHeapBase, kRuntimeHeapLimit);
        std::cerr << "[pop] runtime guest heap moved to " << hex(runtime->guestHeapBase()) << ".."
                  << hex(runtime->guestHeapLimit()) << std::endl;
        ps2__InitSys_0x111c68(rdram, ctx, runtime);
    }

    void endOfHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        SET_GPR_U32(ctx, 2, kGameHeapEnd); // like runtime stubs: leave pc, the dispatcher returns
    }

    void applyPopOverrides(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x00111c68u, initSysWithRuntimeHeapMoved);
        runtime.replaceFunction(0x00108b80u, endOfHeap);

        ps2_game_overrides::bindAddressHandler(runtime, kSceCdLayerSearchFile, "sceCdSearchFile");
        ps2_game_overrides::bindAddressHandler(runtime, kSceCdDiskReady, "sceCdDiskReady");

        runtime.replaceFunction(0x00153940u, traceMount);
        runtime.replaceFunction(0x00153140u, traceAlias);
        runtime.replaceFunction(0x00152080u, traceResolve);
        runtime.replaceFunction(0x00152b40u, traceOpen);
        runtime.replaceFunction(0x0014f380u, traceCdDeviceCtor);
        runtime.replaceFunction(0x00100ac0u, traceMalloc);
        runtime.replaceFunction(0x00101880u, traceSbrk);
        runtime.replaceFunction(0x001008c0u, traceFree);

        g_origMemset = runtime.lookupFunction(0x00116c00u);
        g_origMemcpy = runtime.lookupFunction(0x00116a00u);
        g_origMemmove = runtime.lookupFunction(0x00116ac0u);
        if (g_origMemset && g_origMemcpy && g_origMemmove)
        {
            runtime.replaceFunction(0x00116c00u, guardMemset);
            runtime.replaceFunction(0x00116a00u, guardMemcpy);
            runtime.replaceFunction(0x00116ac0u, guardMemmove);
        }
        else
        {
            std::cerr << "[pop:heap] could not look up memset/memcpy/memmove handlers" << std::endl;
        }
        installWatched(runtime, std::make_index_sequence<kWatchedCount>{});
    }
}

PS2_REGISTER_GAME_OVERRIDE("Prince of Persia: The Sands of Time (NTSC-U v1.01)",
                           "SLUS_207.43", kEntry, kElfCrc32, applyPopOverrides)
