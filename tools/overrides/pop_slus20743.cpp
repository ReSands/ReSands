// Game overrides for Prince of Persia: The Sands of Time, US v1.01 (SLUS_207.43).
// rebuild.sh copies this into ps2xRuntime/src/runner/ so it links into the
// executable: overrides compiled into the ps2_runtime static library are
// dropped by the linker and never apply (upstream issue #264).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include <ps2_recompiled_functions.h>

#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

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
    // Set true to re-enable the heap debugger and file-system/kernel-call tracing
    // (used to find the guest-heap clash; slows the game and is very verbose).
    constexpr bool kDebugTracing = false;
    // Records thread/semaphore/CD kernel calls (name, a0, caller, sp) in a ring buffer that
    // is dumped when the bigfile reader stalls (see traceStreamRead).
    constexpr bool kKernelCallLog = true;
    struct KernelEvent { const char *name; uint32_t a0, caller, sp; };
    KernelEvent g_kernelRing[150];
    uint32_t g_kernelRingCount = 0;
    void kernelRingRecord(const char *name, uint32_t a0, uint32_t caller, uint32_t sp)
    {
        g_kernelRing[g_kernelRingCount++ % 150] = {name, a0, caller, sp};
    }
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
        {0x00108BD0u, "iSignalSema", nullptr},    {0x00108BB0u, "DeleteSema", nullptr},
        {0x00101A10u, "sceCdCallback", nullptr},  {0x00101BD0u, "sceCdInitEeCB", nullptr},
    };
    constexpr size_t kWatchedCount = sizeof(g_watched) / sizeof(g_watched[0]);
    // ---- Frame dumper: saves the frame the runtime presents to the window as a PNG every
    // kFrameDumpSeconds of real time (up to kFrameDumpMax), into game/recomp/screens/.
    // Driven from the instrumented kernel calls, which run constantly. ExportImage is
    // CPU-only, so this is safe off the render thread.
    constexpr bool kFrameDump = true;
    constexpr int kFrameDumpSeconds = 10, kFrameDumpMax = 9;
    void maybeDumpFrame(PS2Runtime *runtime)
    {
        static int dumped = 0;
        static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(kFrameDumpSeconds);
        if (!kFrameDump || dumped >= kFrameDumpMax || std::chrono::steady_clock::now() < next)
            return;
        next = std::chrono::steady_clock::now() + std::chrono::seconds(kFrameDumpSeconds);
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0, displayFbp = 0;
        if (!runtime->gs().copyLatchedHostPresentationFrame(pixels, w, h, &displayFbp) || w == 0 || h == 0 ||
            pixels.size() < static_cast<size_t>(w) * h * 4u)
            return;
        Image img{pixels.data(), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        const std::string path = "../../recomp/screens/frame_" + std::to_string(++dumped * kFrameDumpSeconds) + "s.png";
        const bool ok = ExportImage(img, path.c_str());
        std::cerr << "[pop] frame dump " << path << " " << w << "x" << h << " fbp=" << displayFbp << (ok ? "" : " FAILED") << std::endl;
    }

    template <size_t I>
    void watchedCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        maybeDumpFrame(runtime);
        const uint32_t caller = getRegU32(ctx, 31);
        if (kDebugTracing)
            heapWalkCheck(rdram, g_watched[I].name, caller);
        if (kKernelCallLog && g_watched[I].addr != 0x00108A90u /* GetThreadId: noise */)
            kernelRingRecord(g_watched[I].name, getRegU32(ctx, 4), caller, getRegU32(ctx, 29));
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

    // Bigfile read path: readFully(handle, buf, size) 0x494cc0 loops on
    // streamRead(reader, request, size, dest) 0x156c00 until size bytes have arrived.
    int g_readTraceBudget = 200;
    void traceReadFully(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_readTraceBudget-- > 0)
            std::cerr << "[pop:rd] readFully(" << hex(getRegU32(ctx, 4)) << ", buf=" << hex(getRegU32(ctx, 5))
                      << ", size=" << hex(getRegU32(ctx, 6)) << ") from " << hex(getRegU32(ctx, 31)) << std::endl;
        FUN_00494cc0_0x494cc0(rdram, ctx, runtime);
    }
    void traceStreamRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t req = getRegU32(ctx, 5);
        std::string args = "reader=" + hex(getRegU32(ctx, 4)) + ", req=" + hex(req) + ", size=" + hex(getRegU32(ctx, 6)) +
                           ", dest=" + hex(getRegU32(ctx, 7)) + " [req pos=" + hex(guestU32(rdram, req)) +
                           " +5=" + hex(guestU32(rdram, req + 0x14)) + " +7=" + hex(guestU32(rdram, req + 0x1c)) +
                           " +8=" + hex(guestU32(rdram, req + 0x20)) + "]";
        const uint32_t ra = getRegU32(ctx, 31);
        const uint32_t destBefore = getRegU32(ctx, 7);
        if (g_readTraceBudget-- > 0)
            traced<FUN_00156c00_0x156c00>("streamRead", rdram, ctx, runtime, args);
        else
            FUN_00156c00_0x156c00(rdram, ctx, runtime);
        // First bytes of large reads, to check them against PRINCE.BF on the host.
        static int bigDumps = 12;
        if (ctx->pc == ra && (destBefore == 0x00571bc0u || getRegU32(ctx, 2) >= 0x1000u) && bigDumps > 0)
        {
            --bigDumps;
            std::cerr << "[pop:data] " << hex(getRegU32(ctx, 2)) << " bytes at " << hex(destBefore) << ":";
            for (uint32_t i = 0; i < 32; ++i)
            {
                char b[4];
                std::snprintf(b, sizeof(b), " %02x", *getConstMemPtr(rdram, destBefore + i));
                std::cerr << b;
            }
            std::cerr << std::endl;
        }
        // Stall detector: after 3 zero-byte reads in a row, dump the recent kernel calls once.
        static int zeros = 0;
        static bool dumped = false;
        if (ctx->pc == ra && getRegU32(ctx, 2) == 0u)
            ++zeros;
        else if (ctx->pc == ra)
            zeros = 0;
        if (zeros >= 3 && !dumped)
        {
            dumped = true;
            const uint32_t n = std::min<uint32_t>(g_kernelRingCount, 150);
            std::cerr << "[pop:stall] reader returned 0 three times; last " << n << " kernel calls:" << std::endl;
            for (uint32_t i = 0; i < n; ++i)
            {
                const KernelEvent &e = g_kernelRing[(g_kernelRingCount - n + i) % 150];
                std::cerr << "[pop:stall]   " << e.name << "(" << hex(e.a0) << ") from " << hex(e.caller) << " sp=" << hex(e.sp) << std::endl;
            }
        }
    }

    // ---- CD read completion callback ----
    // The game's CD device runs a worker thread that issues sceCdRead and then blocks on
    // a semaphore until the completion callback it registered with sceCdCallback
    // (0x14EFC0) does iSignalSema(device + 0x4040). PS2Recomp's sceCdCallback is a no-op
    // and its sceCdRead completes synchronously without calling anything, so once the main
    // thread stops submitting requests (it waits for that read's data) both threads wait
    // forever: the bigfile reader returned 0 bytes in a loop. After a successful read we
    // do what the callback would: signal the worker's semaphore. The device pointer is the
    // global the callback itself reads, lw v0, -0x779C(gp) = 0x66D254.
    constexpr uint32_t kSceCdRead = 0x00102D40u;
    constexpr uint32_t kSceCdCallback = 0x00101A10u;
    constexpr uint32_t kISignalSema = 0x00108BD0u;
    constexpr uint32_t kGameCdCallback = 0x0014EFC0u;
    constexpr uint32_t kCdDeviceGlobal = 0x0066D254u;
    PS2Runtime::RecompiledFunction g_origCdRead = nullptr, g_origCdCallback = nullptr, g_iSignalSema = nullptr;
    uint32_t g_cdCallback = 0;

    void cdCallbackRecorded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t previous = g_cdCallback;
        g_cdCallback = getRegU32(ctx, 4);
        g_origCdCallback(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 2, previous); // sceCdCallback returns the previous callback
    }

    void cdReadWithCompletion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origCdRead(rdram, ctx, runtime);
        if (getRegU32(ctx, 2) == 0u || g_cdCallback == 0u)
            return; // read failed, or no callback registered
        if (g_cdCallback != kGameCdCallback)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                std::cerr << "[pop] sceCdRead: unknown CD callback " << hex(g_cdCallback) << " not emulated" << std::endl;
            }
            return;
        }
        const uint32_t device = guestU32(rdram, kCdDeviceGlobal);
        if (device == 0u)
            return;
        const uint32_t a0 = getRegU32(ctx, 4), v0 = getRegU32(ctx, 2);
        SET_GPR_U32(ctx, 4, guestU32(rdram, device + 0x4040u));
        g_iSignalSema(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 4, a0);
        SET_GPR_U32(ctx, 2, v0);
    }

    // ---- IOP modules loaded from a buffer the game DMA'd to the IOP ----
    // The game starts its 7 embedded IRX modules (incl. UbiSound_Driver, whose RPC server
    // 0x12345678 it waits for) the standard Sony way: sceSifAllocSysMemory, sceSifSetDma
    // the image into IOP memory, then sceSifLoadModuleBuffer(iopAddress). PS2Recomp's
    // handler for _sceSifLoadModuleBuffer (0x110288) reads the image from *EE* memory at
    // that address, so the load fails. We remember each SIF DMA (EE source -> IOP
    // destination) and hand the loader the EE source of the transfer to that address.
    struct SifDmaRecord { uint32_t eeSrc, iopDst, size; };
    std::vector<SifDmaRecord> g_sifDmas;
    constexpr uint32_t kSceSifSetDma = 0x00108F60u;
    constexpr uint32_t kSifLoadModuleBuffer = 0x00110288u;
    PS2Runtime::RecompiledFunction g_origSifSetDma = nullptr, g_origLoadModuleBuffer = nullptr;

    void sifSetDmaRecorded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // sceSifSetDma(sceSifDmaData *list, int count); entry = {data(EE), addr(IOP), size, mode}
        const uint32_t list = getRegU32(ctx, 4), count = getRegU32(ctx, 5);
        for (uint32_t i = 0; i < count && i < 32u; ++i)
        {
            const uint32_t e = list + i * 16u;
            g_sifDmas.push_back({guestU32(rdram, e), guestU32(rdram, e + 4u), guestU32(rdram, e + 8u)});
        }
        if (g_sifDmas.size() > 256u)
            g_sifDmas.erase(g_sifDmas.begin(), g_sifDmas.begin() + 128);
        g_origSifSetDma(rdram, ctx, runtime);
    }

    void loadModuleBufferFromEeSource(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t iopAddr = getRegU32(ctx, 4);
        for (auto it = g_sifDmas.rbegin(); it != g_sifDmas.rend(); ++it)
        {
            if (it->iopDst == iopAddr)
            {
                std::cerr << "[pop] sceSifLoadModuleBuffer: IOP " << hex(iopAddr) << " -> EE source " << hex(it->eeSrc)
                          << " (" << hex(it->size) << " bytes)" << std::endl;
                SET_GPR_U32(ctx, 4, it->eeSrc);
                break;
            }
        }
        g_origLoadModuleBuffer(rdram, ctx, runtime);
        std::cerr << "[pop] sceSifLoadModuleBuffer -> " << static_cast<int32_t>(getRegU32(ctx, 2)) << std::endl;
    }

    // Decompressor used for BIG "bin" chunks: FUN_0048dfc0(src, srcLen, dst, &dstLen, 0).
    // Saves the first few chunks' input and output to game/recomp/dumps/ so they can be
    // checked against an independent decompressor on the host.
    int g_decompDumps = 4;
    void traceDecompress(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t src = getRegU32(ctx, 4), srcLen = getRegU32(ctx, 5), dst = getRegU32(ctx, 6), dstLenPtr = getRegU32(ctx, 7);
        const uint32_t expected = guestU32(rdram, dstLenPtr), ra = getRegU32(ctx, 31);
        FUN_0048dfc0_0x48dfc0(rdram, ctx, runtime);
        if (ctx->pc != ra)
            return;
        const uint32_t produced = guestU32(rdram, dstLenPtr);
        std::cerr << "[pop:lzo] decompress(src=" << hex(src) << ", srcLen=" << hex(srcLen) << ", dst=" << hex(dst)
                  << ", expected=" << hex(expected) << ") -> produced " << hex(produced) << ", v0=" << hex(getRegU32(ctx, 2))
                  << (produced != expected ? "  MISMATCH" : "") << std::endl;
        if (g_decompDumps-- > 0 && srcLen < 0x100000u && produced < 0x100000u)
        {
            const std::string base = "../../recomp/dumps/lzo" + std::to_string(4 - g_decompDumps - 1);
            if (FILE *f = std::fopen((base + ".in").c_str(), "wb"))
            {
                std::fwrite(getConstMemPtr(rdram, src), 1, srcLen, f);
                std::fclose(f);
            }
            if (FILE *f = std::fopen((base + ".out").c_str(), "wb"))
            {
                std::fwrite(getConstMemPtr(rdram, dst), 1, produced, f);
                std::fclose(f);
            }
        }
    }

    // BIGio.c bin reader FUN_00488280(this, size): globals ReadSize 0x674e48, pos 0x674e50,
    // data 0x674e58, buf 0x674e5c, avail 0x674e60, cur 0x674e64, end 0x674e68, remaining 0x674e6c.
    int g_binTrace = 30;
    std::string binState(uint8_t *rdram)
    {
        std::string s;
        const std::pair<const char *, uint32_t> fields[] = {{"readSize", 0x674e48}, {"pos", 0x674e50}, {"data", 0x674e58},
            {"buf", 0x674e5c}, {"avail", 0x674e60}, {"cur", 0x674e64}, {"end", 0x674e68}, {"remain", 0x674e6c},
            {"base", 0x674e44}, {"swap", 0x6750d0}, {"flags", 0x674f08}, {"forced", 0x674f04}};
        for (const auto &f : fields)
            s += std::string(" ") + f.first + "=" + hex(guestU32(rdram, f.second));
        return s;
    }
    void traceBinRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t size = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
        const bool log = g_binTrace-- > 0;
        if (log)
        {
            std::cerr << "[pop:bin] read(size=" << hex(size) << ") from " << hex(ra) << " before:" << binState(rdram) << std::endl;
            for (const uint32_t at : {guestU32(rdram, 0x674e58), 0x00571bc0u, guestU32(rdram, 0x674e44)})
            {
                if (at == 0u)
                    continue;
                std::cerr << "[pop:bin]   bytes at " << hex(at) << ":";
                for (uint32_t i = 0; i < 32; ++i)
                {
                    char b[4];
                    std::snprintf(b, sizeof(b), " %02x", *getConstMemPtr(rdram, at + i));
                    std::cerr << b;
                }
                std::cerr << std::endl;
            }
        }
        FUN_00488280_0x488280(rdram, ctx, runtime);
        if (log && ctx->pc == ra)
            std::cerr << "[pop:bin]   -> " << hex(getRegU32(ctx, 2)) << " after:" << binState(rdram) << std::endl;
    }

    // FUN_00488240(size): thin wrapper over the bin reader; log who asks for what.
    int g_binCallerTrace = 40;
    void traceBinReadCaller(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_binCallerTrace-- > 0)
            std::cerr << "[pop:bin] FUN_00488240(" << hex(getRegU32(ctx, 4)) << ", " << hex(getRegU32(ctx, 5))
                      << ") from " << hex(getRegU32(ctx, 31)) << std::endl;
        FUN_00488240_0x488240(rdram, ctx, runtime);
    }

    void applyPopOverrides(PS2Runtime &runtime)
    {
        if (kKernelCallLog)
        {
            runtime.replaceFunction(0x00488240u, traceBinReadCaller);
            runtime.replaceFunction(0x0048dfc0u, traceDecompress);
            runtime.replaceFunction(0x00488280u, traceBinRead);
        }
        g_origSifSetDma = runtime.lookupFunction(kSceSifSetDma);
        g_origLoadModuleBuffer = runtime.lookupFunction(kSifLoadModuleBuffer);
        if (g_origSifSetDma && g_origLoadModuleBuffer)
        {
            runtime.replaceFunction(kSceSifSetDma, sifSetDmaRecorded);
            runtime.replaceFunction(kSifLoadModuleBuffer, loadModuleBufferFromEeSource);
        }

        if (kKernelCallLog)
        {
            runtime.replaceFunction(0x00494cc0u, traceReadFully);
            runtime.replaceFunction(0x00156c00u, traceStreamRead);
        }
        runtime.replaceFunction(0x00111c68u, initSysWithRuntimeHeapMoved);
        runtime.replaceFunction(0x00108b80u, endOfHeap);

        ps2_game_overrides::bindAddressHandler(runtime, kSceCdLayerSearchFile, "sceCdSearchFile");
        ps2_game_overrides::bindAddressHandler(runtime, kSceCdDiskReady, "sceCdDiskReady");

        g_origCdRead = runtime.lookupFunction(kSceCdRead);
        g_origCdCallback = runtime.lookupFunction(kSceCdCallback);
        g_iSignalSema = runtime.lookupFunction(kISignalSema);
        if (g_origCdRead && g_origCdCallback && g_iSignalSema)
        {
            runtime.replaceFunction(kSceCdCallback, cdCallbackRecorded);
            runtime.replaceFunction(kSceCdRead, cdReadWithCompletion);
        }
        else
        {
            std::cerr << "[pop] could not look up sceCdRead/sceCdCallback/iSignalSema handlers" << std::endl;
        }

        if (kDebugTracing || kKernelCallLog)
            installWatched(runtime, std::make_index_sequence<kWatchedCount>{});
        if (!kDebugTracing)
            return;
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
    }
}

PS2_REGISTER_GAME_OVERRIDE("Prince of Persia: The Sands of Time (NTSC-U v1.01)",
                           "SLUS_207.43", kEntry, kElfCrc32, applyPopOverrides)
