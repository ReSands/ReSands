// The judge of a VU1 stage port (vu_task.h): runs stage() on every case recorded by
// `vu1replay --stage` and compares with the interpreter's result.
//
//   judge <cases> [--ulps N] [--report N] [--quiet]
// Exact: the exit pc, vi registers, clip flags, XGKICK count. Within N units in the last place (default 4):
// vf, ACC, Q, P, I, and every 32-bit word of the data memory and of the kicked packets (integers in them
// must then be within N as well: GS coordinates come from float conversions). MAC and status flags are
// not compared (a port computes them only where the stage reads them). The FPU rounds toward zero, as
// the interpreter runs VU code. Prints "PASS <n> cases" or the first differences of the first failing
// cases (the model's feedback).
#include "vu_gif.h"
#include "vu_task.h"

#include <xmmintrin.h>

#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#if defined(__GNUC__) || defined(__clang__)
#define JUDGE_PRINTF __attribute__((format(printf, 2, 3)))
#else
#define JUDGE_PRINTF
#endif

namespace
{
    std::vector<std::vector<uint8_t>> g_kicks;

    uint32_t bits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }
    // distance in representable values between two 32-bit patterns read as floats (+0 and -0 equal)
    uint64_t ulps(uint32_t a, uint32_t b)
    {
        const auto ordered = [](uint32_t u) -> int64_t { return (u & 0x80000000u) ? -int64_t(u & 0x7FFFFFFFu) : int64_t(u); };
        const int64_t d = ordered(a) - ordered(b);
        return uint64_t(d < 0 ? -d : d);
    }

    struct Report
    {
        int lines = 0, limit = 12;
        std::string text;
        void add(const char *fmt, ...) JUDGE_PRINTF
        {
            if (lines++ >= limit)
                return;
            char buf[512];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
            text += buf;
            text += '\n';
        }
    };

    const char kLane[] = "xyzw";

    void compare(const VuState &want, const VuState &got, const std::vector<std::vector<uint8_t>> &wantKicks, uint64_t tol, Report &r)
    {
        if (want.pc != got.pc)
            r.add("exit pc: expected 0x%x, got 0x%x", want.pc, got.pc);
        for (int i = 1; i < 16; ++i)
            if (int16_t(want.vi[i]) != int16_t(got.vi[i]))
                r.add("vi%02d: expected %d, got %d", i, want.vi[i], got.vi[i]);
        for (int i = 1; i < 32; ++i)
            for (int c = 0; c < 4; ++c)
                if (ulps(bits(want.vf[i][c]), bits(got.vf[i][c])) > tol)
                    r.add("vf%02d.%c: expected %.9g (0x%08x), got %.9g (0x%08x)", i, kLane[c], double(want.vf[i][c]), bits(want.vf[i][c]),
                          double(got.vf[i][c]), bits(got.vf[i][c]));
        for (int c = 0; c < 4; ++c)
            if (ulps(bits(want.acc[c]), bits(got.acc[c])) > tol)
                r.add("ACC.%c: expected %.9g, got %.9g", kLane[c], double(want.acc[c]), double(got.acc[c]));
        if (ulps(bits(want.q), bits(got.q)) > tol)
            r.add("Q: expected %.9g, got %.9g", double(want.q), double(got.q));
        if (ulps(bits(want.p), bits(got.p)) > tol)
            r.add("P: expected %.9g, got %.9g", double(want.p), double(got.p));
        if (bits(want.i) != bits(got.i))
            r.add("I: expected %.9g, got %.9g", double(want.i), double(got.i));
        if ((want.clip & 0xFFFFFF) != (got.clip & 0xFFFFFF))
            r.add("clip flags: expected 0x%06x, got 0x%06x", want.clip & 0xFFFFFF, got.clip & 0xFFFFFF);
        for (uint32_t w = 0; w < 0x1000; ++w)
        {
            uint32_t a, b;
            std::memcpy(&a, want.mem + 4 * w, 4);
            std::memcpy(&b, got.mem + 4 * w, 4);
            if (ulps(a, b) > tol)
                r.add("data qword %u (address 0x%x).%c: expected 0x%08x (%.9g as float), got 0x%08x (%.9g)", w / 4, w / 4, kLane[w % 4], a,
                      double(*reinterpret_cast<float *>(&a)), b, double(*reinterpret_cast<float *>(&b)));
        }
        if (wantKicks.size() != g_kicks.size())
            r.add("XGKICK: expected %zu packets, got %zu", wantKicks.size(), g_kicks.size());
        for (size_t k = 0; k < wantKicks.size() && k < g_kicks.size(); ++k)
        {
            const auto &a = wantKicks[k], &b = g_kicks[k];
            if (a.size() != b.size())
                r.add("XGKICK %zu: expected %zu bytes, got %zu", k, a.size(), b.size());
            for (size_t o = 0; o + 4 <= a.size() && o + 4 <= b.size(); o += 4)
            {
                uint32_t x, y;
                std::memcpy(&x, a.data() + o, 4);
                std::memcpy(&y, b.data() + o, 4);
                if (ulps(x, y) > tol)
                    r.add("XGKICK %zu qword %zu.%c: expected 0x%08x, got 0x%08x", k, o / 16, kLane[(o / 4) % 4], x, y);
            }
        }
    }

    // a stage that runs longer than 20 s is an endless loop: exit 3 (a watchdog thread, so the judge
    // also builds with MSVC on the PC, which has no alarm())
    std::atomic<int64_t> g_deadline{0}; // steady-clock milliseconds; 0 = no stage running
    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void watchdog()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const int64_t d = g_deadline.load();
            if (d && nowMs() > d)
                std::_Exit(3);
        }
    }
}

void xgkick(VuState &vu, uint32_t qwordAddress) { g_kicks.push_back(vuGifPacket(vu.mem, qwordAddress)); }

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: judge <cases> [--ulps N] [--report N] [--quiet]\n");
        return 2;
    }
    uint64_t tol = 4;
    int reportCases = 3;
    for (int i = 2; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--ulps") && i + 1 < argc)
            tol = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--report") && i + 1 < argc)
            reportCases = std::atoi(argv[++i]);
    }
    std::thread(watchdog).detach();
    std::ifstream f(argv[1], std::ios::binary);
    auto in = std::make_unique<VuState>(), want = std::make_unique<VuState>(), got = std::make_unique<VuState>();
    int n = 0, failed = 0, shown = 0;
    std::string out;
    for (;;)
    {
        uint32_t head[3];
        if (!f.read(reinterpret_cast<char *>(head), sizeof head))
            break;
        f.read(reinterpret_cast<char *>(in.get()), sizeof *in);
        f.read(reinterpret_cast<char *>(want.get()), sizeof *want);
        uint32_t nk = 0;
        f.read(reinterpret_cast<char *>(&nk), 4);
        std::vector<std::vector<uint8_t>> kicks(nk);
        for (auto &k : kicks)
        {
            uint32_t size = 0;
            f.read(reinterpret_cast<char *>(&size), 4);
            k.resize(size);
            f.read(reinterpret_cast<char *>(k.data()), size);
        }
        *got = *in;
        g_kicks.clear();
        const unsigned csr = _mm_getcsr();
        _mm_setcsr(csr | 0x6000u); // round toward zero, like the VU
        g_deadline = nowMs() + 20000;
        stage(*got);
        g_deadline = 0;
        _mm_setcsr(csr);
        Report r;
        compare(*want, *got, kicks, tol, r);
        ++n;
        if (r.lines)
        {
            ++failed;
            if (shown++ < reportCases)
            {
                char buf[160];
                std::snprintf(buf, sizeof buf, "case %d (trace record %u, entry pc 0x%x): %d differences%s\n", n - 1, head[1], in->pc, r.lines,
                              r.lines > r.limit ? " (first 12)" : "");
                out += buf + r.text;
            }
        }
    }
    if (!n)
    {
        std::printf("no cases\n");
        return 2;
    }
    if (!failed)
    {
        std::printf("PASS %d cases\n", n);
        return 0;
    }
    std::printf("FAIL %d of %d cases\n%s", failed, n, out.c_str());
    return 1;
}
