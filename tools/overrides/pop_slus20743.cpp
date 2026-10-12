// Game overrides for Prince of Persia: The Sands of Time, US v1.01 (SLUS_207.43).
// rebuild.sh copies this into ps2xRuntime/src/runner/ so it links into the
// executable: overrides compiled into the ps2_runtime static library are
// dropped by the linker and never apply (upstream issue #264).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include <ps2_recompiled_functions.h>

#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

void installPopMovies(PS2Runtime &runtime); // pop_movies.cpp: Bink movies from another version

namespace
{
    void drainAsyncGs(); // async GS experiment: wait for queued GIF packets (defined below)

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
    // is dumped when the bigfile reader stalls (see traceStreamRead), and traces bigfile reads
    // and decompression. Costs speed, so opt-in: POP_TRACE=1.
    bool kernelCallLog()
    {
        static const bool on = [] { const char *v = std::getenv("POP_TRACE"); return v && std::atoi(v) != 0; }();
        return on;
    }
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
    constexpr int kFrameDumpSeconds = 15, kFrameDumpMax = 40;
    void maybeDumpFrame(PS2Runtime *runtime)
    {
        static int dumped = 0;
        static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(kFrameDumpSeconds);
        if (!kFrameDump || dumped >= kFrameDumpMax || std::chrono::steady_clock::now() < next)
            return;
        next = std::chrono::steady_clock::now() + std::chrono::seconds(kFrameDumpSeconds);
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0, displayFbp = 0;
        drainAsyncGs();
        if (!runtime->gs().copyLatchedHostPresentationFrame(pixels, w, h, &displayFbp) || w == 0 || h == 0 ||
            pixels.size() < static_cast<size_t>(w) * h * 4u)
            return;
        Image img{pixels.data(), static_cast<int>(w), static_cast<int>(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        const std::string path = "../../recomp/screens/frame_" + std::to_string(++dumped * kFrameDumpSeconds) + "s.png";
        const bool ok = ExportImage(img, path.c_str());
        std::cerr << "[pop] frame dump " << path << " " << w << "x" << h << " fbp=" << displayFbp << (ok ? "" : " FAILED") << std::endl;
        // Raw GS VRAM alongside the 20 s and 30 s frames, for offline decoding (tools/gsvram.py).
        if (dumped == 2 || dumped == 3)
        {
            const std::string vpath = "../../recomp/screens/vram_" + std::to_string(dumped * kFrameDumpSeconds) + "s.bin";
            if (FILE *f = std::fopen(vpath.c_str(), "wb"))
            {
                std::fwrite(runtime->memory().getGSVRAM(), 1, PS2_GS_VRAM_SIZE, f);
                std::fclose(f);
                std::cerr << "[pop] vram dump " << vpath << std::endl;
            }
        }
    }

    // ---- Render-path sampler: every 3 s, the VIF1 (D1) and GIF (D2) DMA channel registers,
    // VU1 microcode uploads, and GS packet/image counters. Shows which part of the
    // CPU -> DMA -> VIF1 -> VU1 -> GS path the game uses and where output stops.
    constexpr bool kRenderStats = true;
    // Everything the GS receives through the GIF arbiter (PATH1 XGKICK, PATH2 DIRECT, PATH3).
    uint64_t g_gifPackets = 0, g_gifBytes = 0, g_gifPrimTags = 0;
    PS2Runtime *g_runtime = nullptr;

    // Decodes GIF packets (PACKED and A+D) for one 300-packet window about 20 s into the boot:
    // logs A+D register writes (FRAME, ZBUF, TEST, ALPHA, SCISSOR, TEX0...), PRIM values, and
    // aggregates RGBAQ colours and XYZ ranges, to see where and in what colour the game draws.
    void decodeGifSample(const uint8_t *data, uint32_t size)
    {
        // Repeats every 10 s (first at 10 s); A+D register values are logged in the first window only.
        static auto start = std::chrono::steady_clock::now();
        static int window = -1, windowsDone = 0; // -1 waiting, >0 packets left
        if (window == -1)
        {
            // POP_GIF_SAMPLE_AT=<seconds>: first window starts then (default 10).
            static const int firstAt = std::getenv("POP_GIF_SAMPLE_AT") ? std::atoi(std::getenv("POP_GIF_SAMPLE_AT")) : 10;
            if (windowsDone >= 6 || std::chrono::steady_clock::now() - start < std::chrono::seconds(windowsDone == 0 ? firstAt : 10))
                return;
            window = 300;
            std::cerr << "[pop:gif] --- sample window " << windowsDone << " start ---" << std::endl;
        }
        --window;
        static std::unordered_map<uint32_t, int> adSeen;
        static uint64_t frame[2] = {0, 0}, test[2] = {0, 0}, alphaReg[2] = {0, 0}, xyoff[2] = {0, 0}, tex0[2] = {0, 0};
        static uint32_t curPrim = 0, curAlpha = 0;
        static std::unordered_map<std::string, int> combos;
        static uint64_t bigTex0 = 0;
        auto notePrim = [&](uint32_t prim)
        {
            curPrim = prim;
            const uint32_t c = (prim >> 9) & 1u;
            char key[260];
            const uint64_t t = tex0[c];
            std::snprintf(key, sizeof(key), "type=%u tme=%u abe=%u ctxt=%u FRAME fbp=%u fbmsk=%08x TEST=%05llx ALPHA=%llx A=%02x TEX0 tbp=%u tbw=%u psm=%#x tw=%u th=%u cbp=%u",
                          prim & 7u, (prim >> 4) & 1u, (prim >> 6) & 1u, c, static_cast<unsigned>(frame[c] & 0x1FF),
                          static_cast<unsigned>(frame[c] >> 32), static_cast<unsigned long long>(test[c] & 0x7FFFF),
                          static_cast<unsigned long long>(alphaReg[c]), curAlpha,
                          static_cast<unsigned>(t & 0x3FFF), static_cast<unsigned>((t >> 14) & 0x3F), static_cast<unsigned>((t >> 20) & 0x3F),
                          static_cast<unsigned>((t >> 26) & 0xF), static_cast<unsigned>((t >> 30) & 0xF), static_cast<unsigned>((t >> 37) & 0x3FFF));
            ++combos[key];
            // Raw dump of the first two 3D triangle-strip packets (window 0), to read the VU1 output.
            static int stripDumps = 0;
            if ((prim & 7u) == 4u && windowsDone == 0 && stripDumps < 2)
            {
                ++stripDumps;
                std::cerr << "[pop:raw3d] packet size=" << size << " prim=" << hex(prim) << std::endl;
                for (uint32_t q = 0; q + 16u <= size && q < 16u * 40u; q += 16u)
                {
                    uint64_t lo, hi;
                    std::memcpy(&lo, data + q, 8);
                    std::memcpy(&hi, data + q + 8, 8);
                    char line[80];
                    std::snprintf(line, sizeof(line), "[pop:raw3d] %04x: %016llx %016llx", q,
                                  static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
                    std::cerr << line << std::endl;
                }
            }
            if ((prim & 7u) == 6u && ((prim >> 4) & 1u) && ((tex0[c] >> 26) & 0xF) == 9u)
            {
                bigTex0 = tex0[c];
                // Raw dump of the first two font-draw packets (window 1), to read vertex colours/positions.
                static int rawDumps = 0;
                if (windowsDone == 1 && rawDumps < 2)
                {
                    ++rawDumps;
                    std::cerr << "[pop:raw] packet size=" << size << " prim=" << hex(prim) << std::endl;
                    for (uint32_t q = 0; q + 16u <= size && q < 16u * 48u; q += 16u)
                    {
                        uint64_t lo, hi;
                        std::memcpy(&lo, data + q, 8);
                        std::memcpy(&hi, data + q + 8, 8);
                        char line[80];
                        std::snprintf(line, sizeof(line), "[pop:raw] %04x: %016llx %016llx", q,
                                      static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
                        std::cerr << line << std::endl;
                    }
                }
            }
        };
        static uint32_t minX = ~0u, maxX = 0, minY = ~0u, maxY = 0, colours = 0;
        static std::unordered_map<uint32_t, int> colourCounts;
        uint32_t off = 0;
        while (off + 16u <= size)
        {
            uint64_t tag, regs;
            std::memcpy(&tag, data + off, 8);
            std::memcpy(&regs, data + off + 8, 8);
            off += 16u;
            const uint32_t nloop = tag & 0x7FFFu, flg = (tag >> 58) & 3u;
            uint32_t nreg = (tag >> 60) & 0xFu;
            if (nreg == 0)
                nreg = 16;
            if ((tag >> 46) & 1u)
            {
                const uint32_t prim = (tag >> 47) & 0x7FFu;
                notePrim(prim);
                if (adSeen[0x10000u | prim]++ == 0)
                    std::cerr << "[pop:gif] PRIM(tag)=" << hex(prim) << " type=" << (prim & 7) << " tme=" << ((prim >> 4) & 1)
                              << " abe=" << ((prim >> 6) & 1) << " ctxt=" << ((prim >> 9) & 1) << std::endl;
            }
            if (flg == 0) // PACKED
            {
                for (uint32_t l = 0; l < nloop && off + 16u <= size; ++l)
                    for (uint32_t r = 0; r < nreg && off + 16u <= size; ++r, off += 16u)
                    {
                        const uint32_t reg = (regs >> (r * 4)) & 0xF;
                        uint64_t lo, hi;
                        std::memcpy(&lo, data + off, 8);
                        std::memcpy(&hi, data + off + 8, 8);
                        if (reg == 0xE) // A+D
                        {
                            const uint32_t addr = hi & 0xFF;
                            if (addr == 0x4C || addr == 0x4D) frame[addr - 0x4C] = lo;
                            if (addr == 0x06 || addr == 0x07) tex0[addr - 0x06] = lo;
                            if (addr == 0x47 || addr == 0x48) test[addr - 0x47] = lo;
                            if (addr == 0x42 || addr == 0x43) alphaReg[addr - 0x42] = lo;
                            if (addr == 0x00) notePrim(static_cast<uint32_t>(lo & 0x7FF));
                            if (windowsDone == 0 && adSeen[addr]++ < 3)
                                std::cerr << "[pop:gif] A+D reg " << hex(addr) << " = 0x" << std::hex << lo << std::dec << std::endl;
                        }
                        else if (reg == 0x1) // RGBAQ packed: R,G,B,A in the low bytes of 4 words
                        {
                            const uint32_t rgba = (lo & 0xFF) | ((lo >> 32 & 0xFF) << 8) | ((hi & 0xFF) << 16) | ((hi >> 32 & 0xFF) << 24);
                            curAlpha = rgba >> 24;
                            if (colourCounts[rgba]++ == 0)
                                ++colours;
                        }
                        else if (reg == 0x4 || reg == 0x5) // XYZF2 / XYZ2 packed: X, Y 12.4 fixed
                        {
                            const uint32_t x = (lo & 0xFFFF) >> 4, y = ((lo >> 32) & 0xFFFF) >> 4;
                            minX = std::min(minX, x); maxX = std::max(maxX, x);
                            minY = std::min(minY, y); maxY = std::max(maxY, y);
                        }
                    }
            }
            else if (flg == 2) // IMAGE
                off += nloop * 16u;
            else // REGLIST
                off += ((nloop * nreg + 1u) & ~1u) * 8u;
        }
        if (window == 0)
        {
            std::cerr << "[pop:gif] XYZ range x " << minX << ".." << maxX << " y " << minY << ".." << maxY
                      << " (GS units incl. offset), distinct colours " << colours << ":";
            int shown = 0;
            for (const auto &[c, n] : colourCounts)
                if (shown++ < 12)
                    std::cerr << " " << hex(c) << "x" << n;
            std::cerr << std::endl;
            for (const auto &[k, n] : combos)
                std::cerr << "[pop:gif] prims " << n << "x: " << k << std::endl;
            if (windowsDone == 0)
            {
                // VU1 code + data, mid-scene, for offline analysis of the 3D transform.
                if (FILE *f = std::fopen("../../recomp/screens/vu1data_3d.bin", "wb"))
                {
                    std::fwrite(g_runtime->memory().getVU1Data(), 1, PS2_VU1_DATA_SIZE, f);
                    std::fclose(f);
                }
                if (FILE *f = std::fopen("../../recomp/screens/vu1code_3d.bin", "wb"))
                {
                    std::fwrite(g_runtime->memory().getVU1Code(), 1, PS2_VU1_CODE_SIZE, f);
                    std::fclose(f);
                }
                std::cerr << "[pop:gif] dumped VU1 code/data" << std::endl;
            }
            if (bigTex0 != 0)
            {
                drainAsyncGs();
                // VRAM at the end of the window, with the 512-wide texture's TEX0, for tools/gsvram.py.
                const std::string vpath = "../../recomp/screens/vram_w" + std::to_string(windowsDone) + ".bin";
                if (FILE *f = std::fopen(vpath.c_str(), "wb"))
                {
                    std::fwrite(g_runtime->memory().getGSVRAM(), 1, PS2_GS_VRAM_SIZE, f);
                    std::fclose(f);
                }
                std::cerr << "[pop:gif] vram " << vpath << " bigTex0=0x" << std::hex << bigTex0 << std::dec << std::endl;
                if (windowsDone == 1)
                {
                    // VU1 data memory and EE RAM, to trace where the font draws' zero alpha comes from.
                    if (FILE *f = std::fopen("../../recomp/screens/vu1data_w1.bin", "wb"))
                    {
                        std::fwrite(g_runtime->memory().getVU1Data(), 1, PS2_VU1_DATA_SIZE, f);
                        std::fclose(f);
                    }
                    if (FILE *f = std::fopen("../../recomp/screens/rdram_w1.bin", "wb"))
                    {
                        std::fwrite(g_runtime->memory().getRDRAM(), 1, PS2_RAM_SIZE, f);
                        std::fclose(f);
                    }
                }
            }
            combos.clear();
            colourCounts.clear();
            colours = 0;
            ++windowsDone;
            window = -1;
            start = std::chrono::steady_clock::now();
        }
    }

    // EXPERIMENT: rewrite TEST_1/TEST_2 A+D writes to ZTST=ALWAYS (keep everything else),
    // to check whether the game's draws are rejected by the depth test.
    constexpr bool kForceZtstAlways = false;
    void forceDepthAlways(std::vector<uint8_t> &pkt)
    {
        uint32_t off = 0;
        const uint32_t size = static_cast<uint32_t>(pkt.size());
        while (off + 16u <= size)
        {
            uint64_t tag, regs;
            std::memcpy(&tag, pkt.data() + off, 8);
            std::memcpy(&regs, pkt.data() + off + 8, 8);
            off += 16u;
            const uint32_t nloop = tag & 0x7FFFu, flg = (tag >> 58) & 3u;
            uint32_t nreg = (tag >> 60) & 0xFu;
            if (nreg == 0)
                nreg = 16;
            if (flg == 0)
            {
                for (uint32_t l = 0; l < nloop && off + 16u <= size; ++l)
                    for (uint32_t r = 0; r < nreg && off + 16u <= size; ++r, off += 16u)
                    {
                        if (((regs >> (r * 4)) & 0xF) != 0xE)
                            continue;
                        uint64_t lo, hi;
                        std::memcpy(&lo, pkt.data() + off, 8);
                        std::memcpy(&hi, pkt.data() + off + 8, 8);
                        const uint32_t addr = hi & 0xFF;
                        if (addr == 0x47 || addr == 0x48) // TEST_1 / TEST_2
                        {
                            lo = (lo & ~(3ull << 17)) | (1ull << 16) | (1ull << 17); // ZTE=1, ZTST=ALWAYS
                            std::memcpy(pkt.data() + off, &lo, 8);
                        }
                    }
            }
            else if (flg == 2)
                off += nloop * 16u;
            else
                off += ((nloop * nreg + 1u) & ~1u) * 8u;
        }
    }

    // Pass-by-pass capture (POP_PASSDUMP_AT=<seconds>): from then, for the next 60 GIF packets
    // that draw a textured primitive reading framebuffer memory (TBP below 12000) or target the
    // half-res buffer (FBP 280), save the target buffer after the packet as
    // screens/pass_NNN_fbpF.png (PSMCT32, FBW from FRAME). Shows which post-process pass does what.
    struct PassInfo
    {
        bool interesting = false;
        uint32_t fbp = 0, fbw = 10;
        std::string desc;
    };
    PassInfo classifyPacket(const uint8_t *data, uint32_t size)
    {
        static uint64_t frame[2] = {0, 0}, tex0[2] = {0, 0}, alpha[2] = {0, 0}, scissor[2] = {0, 0};
        PassInfo info;
        uint32_t off = 0;
        while (off + 16u <= size)
        {
            uint64_t tag, regs;
            std::memcpy(&tag, data + off, 8);
            std::memcpy(&regs, data + off + 8, 8);
            off += 16u;
            const uint32_t nloop = tag & 0x7FFFu, flg = (tag >> 58) & 3u;
            uint32_t nreg = (tag >> 60) & 0xFu;
            if (nreg == 0)
                nreg = 16;
            auto notePrim = [&](uint32_t prim)
            {
                const uint32_t c = (prim >> 9) & 1u;
                const uint32_t tbp = tex0[c] & 0x3FFF, fbp = frame[c] & 0x1FF;
                const bool tme = (prim >> 4) & 1u;
                if ((tme && tbp < 12000u) || fbp == 280u)
                {
                    info.interesting = true;
                    info.fbp = fbp;
                    info.fbw = (frame[c] >> 16) & 0x3F;
                    char d[160];
                    std::snprintf(d, sizeof(d), "prim=%03x fbp=%u fbmsk=%08x tme=%u tbp=%u tw=%u th=%u tfx=%u tcc=%u alpha=%llx scissor=%llx",
                                  prim, fbp, static_cast<unsigned>(frame[c] >> 32), tme ? 1u : 0u, tbp,
                                  static_cast<unsigned>((tex0[c] >> 26) & 0xF), static_cast<unsigned>((tex0[c] >> 30) & 0xF),
                                  static_cast<unsigned>((tex0[c] >> 35) & 3), static_cast<unsigned>((tex0[c] >> 34) & 1),
                                  static_cast<unsigned long long>(alpha[c]), static_cast<unsigned long long>(scissor[c]));
                    info.desc = d;
                }
            };
            if ((tag >> 46) & 1u)
                notePrim((tag >> 47) & 0x7FFu);
            if (flg == 0)
            {
                for (uint32_t l = 0; l < nloop && off + 16u <= size; ++l)
                    for (uint32_t r = 0; r < nreg && off + 16u <= size; ++r, off += 16u)
                    {
                        if (((regs >> (r * 4)) & 0xF) != 0xE)
                            continue;
                        uint64_t lo, hi;
                        std::memcpy(&lo, data + off, 8);
                        std::memcpy(&hi, data + off + 8, 8);
                        const uint32_t addr = hi & 0xFF;
                        if (addr == 0x4C || addr == 0x4D) frame[addr - 0x4C] = lo;
                        if (addr == 0x06 || addr == 0x07) tex0[addr - 0x06] = lo;
                        if (addr == 0x42 || addr == 0x43) alpha[addr - 0x42] = lo;
                        if (addr == 0x40 || addr == 0x41) scissor[addr - 0x40] = lo;
                        if (addr == 0x00) notePrim(static_cast<uint32_t>(lo & 0x7FF));
                    }
            }
            else if (flg == 2)
                off += nloop * 16u;
            else
                off += ((nloop * nreg + 1u) & ~1u) * 8u;
        }
        return info;
    }
    void dumpFbp(uint32_t fbp, uint32_t fbw, int w, int h, const std::string &path)
    {
        const uint8_t *vram = g_runtime->memory().getGSVRAM();
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const uint32_t a = GSPSMCT32::addrPSMCT32(fbp * 32u, fbw ? fbw : 1u, x, y) % PS2_GS_VRAM_SIZE;
                uint8_t *o = &px[(static_cast<size_t>(y) * w + x) * 4];
                o[0] = vram[a];
                o[1] = vram[a + 1];
                o[2] = vram[a + 2];
                o[3] = 255;
            }
        Image img{px.data(), w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        ExportImage(img, path.c_str());
    }
    void maybePassDump(const uint8_t *data, uint32_t size)
    {
        static const char *env = std::getenv("POP_PASSDUMP_AT");
        if (!env)
            return;
        static auto start = std::chrono::steady_clock::now();
        static int dumped = 0;
        if (dumped >= 60)
            return;
        const PassInfo info = classifyPacket(data, size); // always, to track FRAME/TEX0/ALPHA state
        if (!info.interesting || std::chrono::steady_clock::now() - start < std::chrono::seconds(std::atoi(env)))
            return;
        ++dumped;
        if (dumped == 1)
        {
            dumpFbp(0, 10, 640, 448, "../../recomp/screens/pass_000_before_fbp0.png");
            dumpFbp(140, 10, 640, 448, "../../recomp/screens/pass_000_before_fbp140.png");
        }
        // The callers process the packet first; see the arbiter hook.
        char path[128];
        std::snprintf(path, sizeof(path), "../../recomp/screens/pass_%03d_fbp%u.png", dumped, info.fbp);
        dumpFbp(info.fbp, info.fbw, 640, 448, path); // whole buffer: 280 also holds smaller levels
        std::cerr << "[pop:pass] " << path << " " << info.desc << std::endl;
        static int raws = 0;
        if (info.fbp != 280u && raws < 2)
        {
            ++raws;
            for (uint32_t q = 0; q + 16u <= size && q < 16u * 32u; q += 16u)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, data + q, 8);
                std::memcpy(&hi, data + q + 8, 8);
                char line[80];
                std::snprintf(line, sizeof(line), "[pop:passraw] %04x: %016llx %016llx", q,
                              static_cast<unsigned long long>(hi), static_cast<unsigned long long>(lo));
                std::cerr << line << std::endl;
            }
        }
    }

    void countGifPacket(const uint8_t *data, uint32_t size)
    {
        decodeGifSample(data, size);
        ++g_gifPackets;
        g_gifBytes += size;
        if (size >= 16u)
        {
            uint64_t tag;
            std::memcpy(&tag, data, sizeof(tag));
            if ((tag >> 46) & 1u) // PRE: tag carries a PRIM value
                ++g_gifPrimTags;
        }
    }
    // GS rasterizer self-test (once, 35 s in): draw a red untextured sprite into FBP 0 with
    // depth/alpha tests off through the normal GIF path, then read the pixel back from VRAM.
    // Off: it drew a visible red square once per run and overwrote GS drawing state.
    constexpr bool kGsSelfTest = false;
    void maybeGsSelfTest(PS2Runtime *runtime)
    {
        if (!kGsSelfTest)
            return;
        drainAsyncGs();
        static auto when = std::chrono::steady_clock::now() + std::chrono::seconds(35);
        static bool done = false;
        if (done || std::chrono::steady_clock::now() < when)
            return;
        done = true;
        auto ad = [](uint64_t *q, uint64_t data, uint64_t reg) { q[0] = data; q[1] = reg; };
        uint64_t pkt[2 * 12] = {};
        // GIFtag: NLOOP=11, EOP=1, FLG=PACKED, NREG=1, REGS=A+D (0xE)
        pkt[0] = 11ull | (1ull << 15) | (0ull << 58) | (1ull << 60);
        pkt[1] = 0xE;
        uint64_t *q = pkt + 2;
        ad(q, 0x000A0000ull, 0x4C); q += 2;          // FRAME_1: FBP 0, FBW 10, PSMCT32, no mask
        ad(q, 0x0000ull | (1ull << 32), 0x4E); q += 2; // ZBUF_1: ZMSK=1 (no z writes)
        ad(q, 0x30000ull, 0x47); q += 2;              // TEST_1: ZTE=1, ZTST=ALWAYS, no alpha test
        ad(q, (639ull << 16) | (447ull << 48), 0x40); q += 2; // SCISSOR_1: 0..639 x 0..447
        ad(q, 0x0ull, 0x18); q += 2;                  // XYOFFSET_1: 0,0
        ad(q, 0x0ull, 0x42); q += 2;                  // ALPHA_1
        ad(q, 6ull, 0x00); q += 2;                    // PRIM: sprite, no texture, no blend
        ad(q, 0xFFull | (0x80ull << 24), 0x01); q += 2; // RGBAQ: red, A=0x80
        ad(q, (16ull * 100) | ((16ull * 100) << 16), 0x05); q += 2; // XYZ2 (100,100)
        ad(q, (16ull * 200) | ((16ull * 200) << 16), 0x05); q += 2; // XYZ2 (200,200) -> kick
        ad(q, 0x0ull, 0x61); q += 2;                  // FINISH
        runtime->gs().processGIFPacket(reinterpret_cast<const uint8_t *>(pkt), sizeof(pkt));
        const uint8_t *vram = runtime->memory().getGSVRAM();
        const uint32_t a = GSPSMCT32::addrPSMCT32(0, 10, 150, 150) % PS2_GS_VRAM_SIZE;
        std::cerr << "[pop:gstest] after red sprite, pixel (150,150) of FBP 0 = " << hex(vram[a]) << "," << hex(vram[a + 1])
                  << "," << hex(vram[a + 2]) << "," << hex(vram[a + 3]) << (vram[a] == 0xFF ? "  -> RASTERIZER WORKS" : "  -> NOT DRAWN")
                  << std::endl;
    }

    void maybeSampleRenderPath(PS2Runtime *runtime)
    {
        maybeGsSelfTest(runtime);
        static int samples = 0;
        static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        if (!kRenderStats || samples >= 25 || std::chrono::steady_clock::now() < next)
            return;
        next = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        ++samples;
        auto &mem = runtime->memory();
        auto reg = [&](uint32_t a) { return hex(mem.readIORegister(a)); };
        std::cerr << "[pop:gfx] dmaStarts=" << mem.dmaStartCount() << " gifCopies=" << mem.gifCopyCount()
                  << " gsWrites=" << mem.gsWriteCount() << " vu1CodeGen=" << mem.getVU1CodeGeneration()
                  << " gsPackets=" << runtime->gs().nativePackedGIFPacketCount() << " gsImages=" << runtime->gs().nativeImageUploadCount()
                  << " | D1 chcr=" << reg(0x10009000) << " madr=" << reg(0x10009010) << " qwc=" << reg(0x10009020) << " tadr=" << reg(0x10009030)
                  << " | D2 chcr=" << reg(0x1000A000) << " madr=" << reg(0x1000A010) << " tadr=" << reg(0x1000A030)
                  << " | VIF1 stat=" << reg(0x10003C00)
                  << " | GIF in: packets=" << g_gifPackets << " bytes=" << g_gifBytes << " primTags=" << g_gifPrimTags << std::endl;
    }

}

#if PS2X_HAS_VU1_PROGRAMS
// Generated by vu1_recomp from a PS2X_VU1_CAPTURE run (see tools/windows/vu1.ps1).
void registerGeneratedVu1Programs();
#endif

namespace ps2_stubs
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
}

namespace
{
    // VU0 macro self-test (once, at the first watched call): run the game's own recompiled
    // libvu0 routines on known inputs and compare with a C++ reference. Matrices are 4 rows of
    // 4 floats; libvu0 treats vectors as rows: ApplyMatrix(v0, m, v1) gives v0 = v1 * m,
    // MulMatrix(m0, m1, m2) gives m0 = m2 * m1 (m1 applied first).
    constexpr bool kVu0SelfTest = true;
    void callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t fn,
                   uint32_t a0, uint32_t a1, uint32_t a2, float f12 = 0.0f)
    {
        R5900Context saved = *ctx;
        std::memcpy(&ctx->f[12], &f12, sizeof(f12));
        SET_GPR_U32(ctx, 4, a0);
        SET_GPR_U32(ctx, 5, a1);
        SET_GPR_U32(ctx, 6, a2);
        SET_GPR_U32(ctx, 31, 0x00100008u);
        ctx->pc = fn;
        runtime->lookupFunction(fn)(rdram, ctx, runtime);
        *ctx = saved;
    }
    void maybeVu0SelfTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static bool done = false;
        if (!kVu0SelfTest || done)
            return;
        done = true;
        const uint32_t buf = runtime->guestMalloc(0x200, 16);
        auto put = [&](uint32_t addr, const float *v, int n) { std::memcpy(rdram + addr, v, n * 4); };
        auto get = [&](uint32_t addr, float *v, int n) { std::memcpy(v, rdram + addr, n * 4); };
        const uint32_t mA = buf, mB = buf + 0x40, mOut = buf + 0x80, vIn = buf + 0xC0, vOut = buf + 0xD0;
        const float A[16] = {1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 10, 0, 1, -2, 3, 1};
        const float B[16] = {2, 0, 1, 0, 0, 3, 0, 0, 1, 0, 2, 0, -1, 4, 0.5f, 1};
        const float V[4] = {1, 2, 3, 1};
        put(mA, A, 16);
        put(mB, B, 16);
        put(vIn, V, 4);
        auto rowTimes = [](const float *v, const float *m, float *out)
        {
            for (int j = 0; j < 4; ++j)
                out[j] = v[0] * m[0 * 4 + j] + v[1] * m[1 * 4 + j] + v[2] * m[2 * 4 + j] + v[3] * m[3 * 4 + j];
        };
        auto report = [](const char *name, const float *got, const float *want, int n)
        {
            bool ok = true;
            for (int i = 0; i < n; ++i)
                ok = ok && std::fabs(got[i] - want[i]) < 1e-3f;
            std::cerr << "[pop:vu0test] " << name << (ok ? " OK" : " WRONG") << " got";
            for (int i = 0; i < n; ++i)
                std::cerr << " " << got[i];
            if (!ok)
            {
                std::cerr << " want";
                for (int i = 0; i < n; ++i)
                    std::cerr << " " << want[i];
            }
            std::cerr << std::endl;
        };
        float got[16], want[16];
        callGuest(rdram, ctx, runtime, 0x004CAF00u, vOut, mA, vIn); // sceVu0ApplyMatrix
        get(vOut, got, 4);
        rowTimes(V, A, want);
        report("ApplyMatrix", got, want, 4);
        // MulMatrix(m0, m1, m2) is m0 = m1 x m2 in Sony's column-vector convention, which with
        // rows as stored here means the rows of m2 transformed by m1. (PS2Recomp's C++ stub had
        // this backwards.)
        callGuest(rdram, ctx, runtime, 0x004CAF30u, mOut, mA, mB); // sceVu0MulMatrix
        get(mOut, got, 16);
        for (int r = 0; r < 4; ++r)
            rowTimes(B + r * 4, A, want + r * 4);
        report("MulMatrix(out,A,B) = A x B (column vectors)", got, want, 16);
        callGuest(rdram, ctx, runtime, 0x004CB000u, mOut, mA, 0); // sceVu0TransposeMatrix
        get(mOut, got, 16);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                want[r * 4 + c] = A[c * 4 + r];
        report("TransposeMatrix", got, want, 16);
        callGuest(rdram, ctx, runtime, 0x004CB228u, mOut, 0, 0); // sceVu0UnitMatrix
        get(mOut, got, 16);
        for (int i = 0; i < 16; ++i)
            want[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        report("UnitMatrix", got, want, 16);

        // InversMatrix(out, R): libvu0 assumes a rotation + translation, so test with RotZ + T.
        {
            const float cz = std::cos(0.7f), sz = std::sin(0.7f);
            const float RT[16] = {cz, sz, 0, 0, -sz, cz, 0, 0, 0, 0, 1, 0, 5, -3, 2, 1};
            put(mA, RT, 16);
            callGuest(rdram, ctx, runtime, 0x004CB048u, mOut, mA, 0);
            get(mOut, got, 16);
            float prod[16];
            for (int r = 0; r < 4; ++r)
                rowTimes(RT + r * 4, got, prod + r * 4);
            for (int i = 0; i < 16; ++i)
                want[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            report("InversMatrix (RT*inv)", prod, want, 16);
            put(mA, A, 16);
        }

        // Normalize(v0, v1): xyz scaled to unit length.
        const float N[4] = {3, 4, 12, 7};
        put(vIn, N, 4);
        callGuest(rdram, ctx, runtime, 0x004CAFC0u, vOut, vIn, 0);
        get(vOut, got, 4);
        want[0] = 3 / 13.0f; want[1] = 4 / 13.0f; want[2] = 12 / 13.0f; want[3] = got[3];
        report("Normalize xyz", got, want, 3);

        // DivVector(v0, v1, q): v1 / q.
        put(vIn, N, 4);
        callGuest(rdram, ctx, runtime, 0x004CB0B8u, vOut, vIn, 0, 4.0f);
        get(vOut, got, 4);
        for (int i = 0; i < 4; ++i)
            want[i] = N[i] / 4.0f;
        report("DivVector /4", got, want, 4);

        // InnerProduct(v0, v1) -> $f0.
        {
            R5900Context saved = *ctx;
            put(vIn, N, 4);
            put(vOut, V, 4);
            SET_GPR_U32(ctx, 4, vIn);
            SET_GPR_U32(ctx, 5, vOut);
            SET_GPR_U32(ctx, 31, 0x00100008u);
            ctx->pc = 0x004CAF98u;
            runtime->lookupFunction(0x004CAF98u)(rdram, ctx, runtime);
            float f0;
            std::memcpy(&f0, &ctx->f[0], sizeof(f0));
            *ctx = saved;
            want[0] = 3 * 1 + 4 * 2 + 12 * 3;
            report("InnerProduct xyz", &f0, want, 1);
        }

        // RotMatrixZ(m0, m1, rz): m1 followed by a rotation of rz about Z (row vectors).
        callGuest(rdram, ctx, runtime, 0x004CB228u, mB, 0, 0); // unit matrix into mB
        callGuest(rdram, ctx, runtime, 0x004CB2C8u, mOut, mB, 0, 0.5f);
        get(mOut, got, 16);
        const float c = std::cos(0.5f), sn = std::sin(0.5f);
        const float Rz[16] = {c, sn, 0, 0, -sn, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        report("RotMatrixZ(0.5) (either sign convention shown)", got, Rz, 16);

        // libm (float ABI: argument in $f12, result in $f0), whatever currently backs them.
        struct MathCase
        {
            const char *name;
            uint32_t addr;
            float (*ref)(float);
        };
        const MathCase cases[] = {
            {"sin", 0x001340C0u, [](float x) { return std::sin(x); }},
            {"cos", 0x00133D00u, [](float x) { return std::cos(x); }},
            {"atan", 0x001335C0u, [](float x) { return std::atan(x); }},
            {"floor", 0x00133E80u, [](float x) { return std::floor(x); }},
            {"ceil", 0x00133AC0u, [](float x) { return std::ceil(x); }},
            {"fabs", 0x00133E40u, [](float x) { return std::fabs(x); }},
        };
        const float args[] = {0.3f, -1.7f, 2.5f, 7.0f, -12.25f, 100.0f};
        for (const MathCase &mc : cases)
        {
            float gotv[6], wantv[6];
            for (int i = 0; i < 6; ++i)
            {
                R5900Context saved = *ctx;
                std::memcpy(&ctx->f[12], &args[i], sizeof(float));
                SET_GPR_U32(ctx, 31, 0x00100008u);
                ctx->pc = mc.addr;
                runtime->lookupFunction(mc.addr)(rdram, ctx, runtime);
                std::memcpy(&gotv[i], &ctx->f[0], sizeof(float));
                *ctx = saved;
                wantv[i] = mc.ref(args[i]);
            }
            report(mc.name, gotv, wantv, 6);
        }
    }

    // rand() 0x118918. ps2_analyzer names it, so PS2Recomp replaces it with its stand-in,
    // which returns std::rand() & 0x7FFF. The game's newlib rand returns 31 bits, and lRand
    // (0x1B9A40) scales by 1/2^31, so with the stand-in lRand(a, b) is always a, and the first
    // in-game cutscene after CINE_010 spins forever drawing random numbers. This is the game's
    // own algorithm on its own state (seeded by its own srand): 64-bit LCG in the reent block.
    void newlibRand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t reent = guestU32(rdram, 0x004E8768u);
        auto *state = reinterpret_cast<uint64_t *>(getMemPtr(rdram, reent + 0xA8u));
        *state = *state * 0x5851F42D4C957F2Dull + 1ull;
        SET_GPR_U32(ctx, 2, static_cast<uint32_t>(*state >> 32) & 0x7FFFFFFFu);
    }

    // FUN_004A4D00: "while (bc2t) ;", the wait for VU1 to go idle before the next VIF1 kick
    // (0x4A2C80, after its DMA/VIF1 waits and before the GIF one). On the EE, the COP2 branch
    // condition is VU1 running. PS2Recomp translates bc2t as VU0's status flag register bit 0
    // (the Z flag), so once VU0 maths leaves Z set the loop never ends (froze the first in-game
    // cutscene after CINE_010). It is the game's only bc2t. VU1 is idle here as far as the
    // game can tell (the runtimes run VU1 work in order), so return at once.
    void vu1IdleWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
    }

    // ---- Texture identity: which asset (bigfile key) is in GS memory at which address ----
    // PS2_Texture_Load(display, TEX_tdst_Data *data, TEX_tdst_File_Desc *desc, index) 0x4B4E80
    // registers texture `index` with the GS texture cache; the first word of `data` is the
    // texture's bigfile key. CJadeTextureManager::LoadTexture(manager, index, ...) 0x4B3840
    // uploads it when needed; the manager (a0, normally 0x70000050 in scratchpad) starts with a
    // pointer to 0x50-byte slots, and slot +0x18 is the GS block address the draws use as TEX0.TBP0
    // (+4/+6 width/height, +0x1C bits per pixel). For high-res texture replacement.
    struct GsTexture
    {
        uint32_t key = 0;
        uint16_t width = 0, height = 0;
        uint8_t bpp = 0;
    };
    std::unordered_map<uint32_t, uint32_t> g_textureKeys;   // texture index -> bigfile key
    std::unordered_map<uint32_t, GsTexture> g_gsTextures;  // GS block address -> texture
    PS2Runtime::RecompiledFunction g_origTextureLoad = nullptr, g_origLoadTexture = nullptr;
    bool textureLog()
    {
        static const bool on = [] { const char *v = std::getenv("POP_TEXLOG"); return v && *v == '1'; }();
        return on;
    }

    void textureLoadMapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t data = getRegU32(ctx, 5), index = getRegU32(ctx, 7);
        if (data)
            g_textureKeys[index] = guestU32(rdram, data);
        g_origTextureLoad(rdram, ctx, runtime);
    }

    void loadTextureMapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t manager = getRegU32(ctx, 4), index = getRegU32(ctx, 5); // before the call reuses them
        g_origLoadTexture(rdram, ctx, runtime);
        const auto key = g_textureKeys.find(index);
        if (key == g_textureKeys.end())
            return;
        const uint32_t slot = guestU32(rdram, manager) + index * 0x50u; // manager +0: slot array
        const uint32_t tbp = guestU32(rdram, slot + 0x18u) & 0x3FFFu;
        GsTexture &t = g_gsTextures[tbp];
        const bool changed = t.key != key->second;
        t.key = key->second;
        t.width = static_cast<uint16_t>(guestU32(rdram, slot + 4u));
        t.height = static_cast<uint16_t>(guestU32(rdram, slot + 4u) >> 16);
        t.bpp = static_cast<uint8_t>(guestU32(rdram, slot + 0x1Cu));
        static int logged = 0;
        if (textureLog() && changed && logged < 400)
        {
            ++logged;
            std::fprintf(stderr, "[pop:tex] tbp=0x%04x key=%08x %ux%u %ubpp index=%u\n", tbp, t.key, t.width, t.height,
                         t.bpp, index);
        }
    }

    // Arrow keys as the left stick. The runtime only maps WASD to the stick once the game puts
    // the pad in analog mode, which PoP does not, and PoP does not walk with the D-pad. While an
    // arrow key is held, this sets the whole pad state (stick plus the runtime's button keys,
    // so jumping while moving works) through the same override as scripted input.
    void keyboardStick()
    {
        static bool active = false;
        if (!IsWindowReady())
            return;
        int x = 0, y = 0;
        x += IsKeyDown(KEY_RIGHT) ? 1 : 0;
        x -= IsKeyDown(KEY_LEFT) ? 1 : 0;
        y += IsKeyDown(KEY_DOWN) ? 1 : 0;
        y -= IsKeyDown(KEY_UP) ? 1 : 0;
        if (x == 0 && y == 0)
        {
            if (active)
                ps2_stubs::clearPadOverrideState();
            active = false;
            return;
        }
        static const std::pair<int, uint16_t> kKeys[] = {
            {KEY_RIGHT_SHIFT, 1u << 0}, {KEY_LEFT_CONTROL, 1u << 1}, {KEY_RIGHT_CONTROL, 1u << 2}, {KEY_ENTER, 1u << 3},
            {KEY_ONE, 1u << 8}, {KEY_THREE, 1u << 9}, {KEY_Q, 1u << 10}, {KEY_E, 1u << 11},
            {KEY_V, 1u << 12}, {KEY_C, 1u << 13}, {KEY_X, 1u << 14}, {KEY_Z, 1u << 15}};
        uint16_t pressed = 0;
        for (const auto &[key, mask] : kKeys)
            if (IsKeyDown(key))
                pressed |= mask;
        auto axis = [](int v) { return static_cast<uint8_t>(v < 0 ? 0x00 : v > 0 ? 0xFF : 0x80); };
        ps2_stubs::setPadOverrideState(static_cast<uint16_t>(0xFFFFu & ~pressed), axis(x), axis(y), 0x80, 0x80);
        active = true;
    }

    // Scripted pad input for unattended boots: POP_INPUT="start@75,cross@95,lup@120:4" presses each
    // button (or pushes the left stick: lup/ldown/lleft/lright) at that many seconds after boot,
    // for 0.8 s or the given :seconds. Between inputs the real keyboard / gamepad is used.
    void maybeScriptedInput()
    {
        struct Press
        {
            double at, hold;
            uint16_t mask;
            uint8_t lx, ly;
            std::string name;
        };
        static std::vector<Press> presses;
        static bool parsed = false;
        static size_t next = 0;
        static bool held = false;
        static auto start = std::chrono::steady_clock::now();
        if (!parsed)
        {
            parsed = true;
            const char *env = std::getenv("POP_INPUT");
            static const std::unordered_map<std::string, uint16_t> kButtons = {
                {"select", 1u << 0}, {"l3", 1u << 1}, {"r3", 1u << 2}, {"start", 1u << 3},
                {"up", 1u << 4}, {"right", 1u << 5}, {"down", 1u << 6}, {"left", 1u << 7},
                {"l2", 1u << 8}, {"r2", 1u << 9}, {"l1", 1u << 10}, {"r1", 1u << 11},
                {"triangle", 1u << 12}, {"circle", 1u << 13}, {"cross", 1u << 14}, {"square", 1u << 15}};
            static const std::unordered_map<std::string, std::pair<uint8_t, uint8_t>> kSticks = {
                {"lup", {0x80, 0x00}}, {"ldown", {0x80, 0xFF}}, {"lleft", {0x00, 0x80}}, {"lright", {0xFF, 0x80}}};
            std::string spec = env ? env : "";
            size_t pos = 0;
            while (pos < spec.size())
            {
                size_t comma = spec.find(',', pos);
                if (comma == std::string::npos)
                    comma = spec.size();
                const std::string item = spec.substr(pos, comma - pos);
                pos = comma + 1;
                const size_t at = item.find('@');
                if (at == std::string::npos)
                    continue;
                const std::string name = item.substr(0, at);
                const size_t colon = item.find(':', at);
                Press press{std::atof(item.c_str() + at + 1), colon == std::string::npos ? 0.8 : std::atof(item.c_str() + colon + 1),
                            0, 0x80, 0x80, name};
                if (kButtons.count(name))
                    press.mask = kButtons.at(name);
                else if (kSticks.count(name))
                {
                    press.lx = kSticks.at(name).first;
                    press.ly = kSticks.at(name).second;
                }
                else
                    continue;
                presses.push_back(press);
            }
            if (!presses.empty())
                std::cerr << "[pop:input] " << presses.size() << " scripted inputs" << std::endl;
        }
        if (!held)
            keyboardStick();
        if (next >= presses.size())
            return;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const Press &p = presses[next];
        if (!held && t >= p.at)
        {
            ps2_stubs::setPadOverrideState(static_cast<uint16_t>(0xFFFFu & ~p.mask), p.lx, p.ly, 0x80, 0x80);
            held = true;
            std::cerr << "[pop:input] " << p.name << " at " << t << " s for " << p.hold << " s" << std::endl;
        }
        else if (held && t >= p.at + p.hold)
        {
            ps2_stubs::clearPadOverrideState();
            held = false;
            ++next;
        }
    }

    template <size_t I>
    void watchedCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        maybeVu0SelfTest(rdram, ctx, runtime);
        maybeDumpFrame(runtime);
        maybeSampleRenderPath(runtime);
        maybeScriptedInput();
        const uint32_t caller = getRegU32(ctx, 31);
        if (kDebugTracing)
            heapWalkCheck(rdram, g_watched[I].name, caller);
        if (kernelCallLog() && g_watched[I].addr != 0x00108A90u /* GetThreadId: noise */)
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

    // Movies (*.INT: Ubisoft logo, PoP logo, intro, cutscenes) need the IPU MPEG decoder,
    // which PS2Recomp does not emulate: the movie manager stays busy forever. Until there is
    // a decoder, report movie files as missing so the game skips them. pop_movies.cpp plays
    // the Bink version of a movie instead when another version's files are present.
    constexpr bool kSkipMovies = true;
    PS2Runtime::RecompiledFunction g_cdSearchFile = nullptr;
    void cdSearchFileSkippingMovies(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (kSkipMovies)
        {
            std::string name;
            for (uint32_t a = getRegU32(ctx, 5), i = 0; i < 64; ++i)
            {
                const char c = static_cast<char>(rdram[(a + i) & 0x1FFFFFFu]);
                if (!c)
                    break;
                name += c;
            }
            if (name.size() >= 6 && name.compare(name.size() - 6, 6, ".INT;1") == 0)
            {
                std::cerr << "[pop] skipping movie " << name << " (no IPU decoder)" << std::endl;
                SET_GPR_U32(ctx, 2, 0u);
                return;
            }
        }
        g_cdSearchFile(rdram, ctx, runtime);
    }

    // DEBUG (boot 54+): FUN_004547C0 (particle spawner, frame 0x180, $ra saved at sp+0xB0)
    // returns to a float. Watch its saved $ra slot across its calls to the vec3 helper
    // FUN_00495D60 and rand(), and report the first call that changes it.
    constexpr bool kWatchParticleFrame = false;
    PS2Runtime::RecompiledFunction g_origSpawner = nullptr, g_origVecMadd = nullptr, g_origRand = nullptr;
    uint32_t g_spawnerRaSlot = 0, g_spawnerRa = 0;
    void checkSpawnerFrame(uint8_t *rdram, R5900Context *ctx, const char *who, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
    {
        if (!g_spawnerRaSlot)
            return;
        const uint32_t now = guestU32(rdram, g_spawnerRaSlot);
        if (now == g_spawnerRa)
            return;
        std::cerr << "[pop:watch] spawner saved $ra at " << hex(g_spawnerRaSlot) << " changed " << hex(g_spawnerRa)
                  << " -> " << hex(now) << " after " << who << "(a0=" << hex(a0) << " a1=" << hex(a1) << " a2=" << hex(a2)
                  << " a3=" << hex(a3) << ") called from " << hex(getRegU32(ctx, 31)) << std::endl;
        g_spawnerRa = now;
    }
    void watchedSpawner(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t outerSlot = g_spawnerRaSlot, outerRa = g_spawnerRa;
        g_spawnerRaSlot = getRegU32(ctx, 29) - 0x180u + 0xB0u;
        g_spawnerRa = getRegU32(ctx, 31);
        std::cerr << "[pop:watch] spawner enter sp=" << hex(getRegU32(ctx, 29)) << " ra=" << hex(g_spawnerRa)
                  << " a0=" << hex(getRegU32(ctx, 4)) << " a1=" << hex(getRegU32(ctx, 5)) << " a2=" << hex(getRegU32(ctx, 6))
                  << " a3=" << hex(getRegU32(ctx, 7)) << " t0=" << hex(getRegU32(ctx, 8)) << std::endl;
        g_origSpawner(rdram, ctx, runtime);
        g_spawnerRaSlot = outerSlot;
        g_spawnerRa = outerRa;
    }
    void watchedVecMadd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
        g_origVecMadd(rdram, ctx, runtime);
        checkSpawnerFrame(rdram, ctx, "vecMadd", a0, a1, a2, a3);
    }
    void watchedRand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
        g_origRand(rdram, ctx, runtime);
        checkSpawnerFrame(rdram, ctx, "rand", a0, a1, a2, a3);
    }

    // EXPERIMENT (POP_ASYNC_GS=1): rasterize GIF packets on a worker thread, in order, so the
    // game thread (EE code + VU1) runs in parallel with the CPU rasterizer. The queue is drained
    // before each end-of-frame flip and before anything reads VRAM, so presentation only sees
    // finished frames. GS FINISH/SIGNAL only set atomic CSR bits, so no guest code runs on the
    // worker.
    struct AsyncGs
    {
        std::mutex m;
        std::condition_variable work, idle;
        std::deque<std::vector<uint8_t>> queue;
        bool busy = false;
        PS2Runtime *rt = nullptr;

        void start(PS2Runtime *runtime)
        {
            rt = runtime;
            std::thread([this] { run(); }).detach();
        }
        void run()
        {
            for (;;)
            {
                std::vector<uint8_t> packet;
                {
                    std::unique_lock<std::mutex> lock(m);
                    work.wait(lock, [&] { return !queue.empty(); });
                    packet = std::move(queue.front());
                    queue.pop_front();
                    busy = true;
                }
                rt->gs().processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));
                {
                    std::lock_guard<std::mutex> lock(m);
                    busy = false;
                    if (queue.empty())
                        idle.notify_all();
                }
            }
        }
        void push(const uint8_t *data, uint32_t size)
        {
            {
                std::lock_guard<std::mutex> lock(m);
                queue.emplace_back(data, data + size);
            }
            work.notify_one();
        }
        void drain()
        {
            std::unique_lock<std::mutex> lock(m);
            idle.wait(lock, [&] { return queue.empty() && !busy; });
        }
    };
    AsyncGs *g_asyncGs = nullptr; // set when POP_ASYNC_GS=1
    void drainAsyncGs()
    {
        if (g_asyncGs)
            g_asyncGs->drain();
    }

    // Frame-rate counter: wraps the display driver's end-of-frame flip (FUN_004B5200, slot 7)
    // and prints frames per second every 5 s.
    PS2Runtime::RecompiledFunction g_origFlip = nullptr;
    void countedFlip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint64_t frames = 0;
        static auto last = std::chrono::steady_clock::now();
        ++frames;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        if (dt >= 5.0)
        {
            std::cerr << "[pop:fps] " << (frames / dt) << std::endl;
            frames = 0;
            last = now;
        }
        drainAsyncGs();
        g_origFlip(rdram, ctx, runtime);
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
#if PS2X_HAS_VU1_PROGRAMS
        registerGeneratedVu1Programs();
        std::fprintf(stderr, "[pop] compiled VU1 programs registered\n");
#endif
        // The GIF tap replaces the runtime's GIF -> GS hand-off. Runtimes that route packets
        // through their own pipeline (the SotC fork's async VIF/GS) would be bypassed, so the
        // tap (and with it the GIF sampler, pass dumps and POP_ASYNC_GS) is opt-in: POP_GIF_TAP=1.
        const char *tapEnv = std::getenv("POP_GIF_TAP");
        if (kRenderStats && tapEnv && std::atoi(tapEnv) != 0)
        {
            PS2Runtime *rt = &runtime;
            g_runtime = rt;
            if (const char *async = std::getenv("POP_ASYNC_GS"); async && std::atoi(async) != 0)
            {
                g_asyncGs = new AsyncGs();
                g_asyncGs->start(rt);
                std::cerr << "[pop] async GS rasterizer thread on" << std::endl;
            }
            runtime.gifArbiter().setProcessPacketFn([rt](const uint8_t *data, uint32_t size)
                                                    {
                                                        countGifPacket(data, size);
                                                        if (g_asyncGs)
                                                        {
                                                            g_asyncGs->push(data, size);
                                                            return;
                                                        }
                                                        if (!kForceZtstAlways)
                                                        {
                                                            rt->gs().processGIFPacket(data, size);
                                                            maybePassDump(data, size);
                                                            return;
                                                        }
                                                        std::vector<uint8_t> copy(data, data + size);
                                                        forceDepthAlways(copy);
                                                        rt->gs().processGIFPacket(copy.data(), size);
                                                    });
        }
        if (kernelCallLog())
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

        if (kernelCallLog())
        {
            runtime.replaceFunction(0x00494cc0u, traceReadFully);
            runtime.replaceFunction(0x00156c00u, traceStreamRead);
        }
        runtime.replaceFunction(0x00111c68u, initSysWithRuntimeHeapMoved);
        runtime.replaceFunction(0x00108b80u, endOfHeap);

        ps2_game_overrides::bindAddressHandler(runtime, kSceCdLayerSearchFile, "sceCdSearchFile");
        if (kWatchParticleFrame)
        {
            g_origSpawner = runtime.lookupFunction(0x004547C0u);
            g_origVecMadd = runtime.lookupFunction(0x00495D60u);
            g_origRand = runtime.lookupFunction(0x00118918u);
            if (g_origSpawner && g_origVecMadd && g_origRand)
            {
                runtime.replaceFunction(0x004547C0u, watchedSpawner);
                runtime.replaceFunction(0x00495D60u, watchedVecMadd);
                runtime.replaceFunction(0x00118918u, watchedRand);
            }
        }
        g_origFlip = runtime.lookupFunction(0x004B5200u);
        if (g_origFlip)
            runtime.replaceFunction(0x004B5200u, countedFlip);
        g_cdSearchFile = runtime.lookupFunction(kSceCdLayerSearchFile);
        if (g_cdSearchFile)
            runtime.replaceFunction(kSceCdLayerSearchFile, cdSearchFileSkippingMovies);
        installPopMovies(runtime);
        runtime.replaceFunction(0x00118918u, newlibRand);
        runtime.replaceFunction(0x004A4D00u, vu1IdleWait);
        g_origTextureLoad = runtime.lookupFunction(0x004B4E80u);
        g_origLoadTexture = runtime.lookupFunction(0x004B3840u);
        if (g_origTextureLoad && g_origLoadTexture)
        {
            runtime.replaceFunction(0x004B4E80u, textureLoadMapped);
            runtime.replaceFunction(0x004B3840u, loadTextureMapped);
        }
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

        if (true) // scripted input, frame dumps and self-tests ride on these hooks
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
