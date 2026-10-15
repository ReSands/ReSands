// Compares PS2Recomp's translation of every EE FPU instruction (fpu_cases.inc, from
// tools/mmitest/fpugen.cpp) with a reference model of the EE FPU, on random and edge-case values.
// Build and run: tools/mmitest/run.sh.
//
// Reference model (public sources: the EE's documented behaviour as PCSX2 models it in its default
// settings - "Chop/Zero" rounding, "Nearest" for division - and the edge cases Burnout 3's port
// lists): no infinities or NaNs; denormal inputs read as zero (sign kept); overflow gives the
// largest float with the result's sign; a denormal result becomes zero; ADD/SUB/MUL and the
// accumulator forms round toward zero (ADD/SUB after the operand alignment PS2Recomp models: the
// smaller operand's bits below the guard bit dropped); DIV, SQRT and RSQRT round to nearest;
// x / 0 gives the largest float with the sign of the operands' signs combined; SQRT and RSQRT take
// the magnitude of a negative operand; CVT.W.S truncates and saturates (|x| >= 2^31); CVT.S.W
// rounds toward zero; compares read denormals as zero (-0 == +0).
// The rounding and denormal rules are the parts this test is for; exact hardware results in the
// last bit are only as good as this model.
//
// Registers: fs = f1, ft = f2, fd = f3; the accumulator; FCR31 bit 23 (C) for the compares.
// Sticky flags in FCR31 are not compared.
#include "ps2_runtime_macros.h"

#include <cfenv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <random>
#include <string>

struct TestCase
{
    const char *name;
    void (*fn)(R5900Context *, uint8_t *, PS2Runtime *);
};

#include "fpu_cases.inc"

namespace
{
    uint32_t bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
    float fl(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

    bool isDenormOrZero(uint32_t b) { return (b & 0x7F800000u) == 0u; }
    uint32_t daz(uint32_t b) { return isDenormOrZero(b) ? (b & 0x80000000u) : b; }

    // Finishes a host result: infinity -> largest float, denormal -> zero (signs kept).
    uint32_t finish(float r)
    {
        uint32_t b = bits(r);
        if ((b & 0x7F800000u) == 0x7F800000u)
            return (b & 0x80000000u) | 0x7F7FFFFFu;
        return daz(b);
    }

    template <typename F>
    float withRounding(int mode, F f)
    {
        const int old = std::fegetround();
        std::fesetround(mode);
        volatile float r = f();
        std::fesetround(old);
        return r;
    }

    // PS2Recomp's operand alignment (the EE adder keeps one guard bit of the smaller operand).
    void align(uint32_t &ua, uint32_t &ub)
    {
        const int diff = static_cast<int>((ua >> 23) & 0xFFu) - static_cast<int>((ub >> 23) & 0xFFu);
        if (diff >= 25) ub &= 0x80000000u;
        else if (diff > 0) ub &= 0xFFFFFFFFu << (diff - 1);
        else if (diff <= -25) ua &= 0x80000000u;
        else if (diff < 0) ua &= 0xFFFFFFFFu << (-diff - 1);
    }

    uint32_t add(uint32_t a, uint32_t b, bool sub)
    {
        a = daz(a);
        b = daz(b);
        align(a, b);
        const float x = fl(a), y = fl(b);
        return finish(withRounding(FE_TOWARDZERO, [&] { return sub ? x - y : x + y; }));
    }

    uint32_t mul(uint32_t a, uint32_t b)
    {
        const float x = fl(daz(a)), y = fl(daz(b));
        return finish(withRounding(FE_TOWARDZERO, [&] { return x * y; }));
    }

    uint32_t maxFloat(uint32_t sign) { return (sign & 0x80000000u) | 0x7F7FFFFFu; }

    uint32_t div(uint32_t a, uint32_t b)
    {
        a = daz(a);
        b = daz(b);
        if (isDenormOrZero(b))
            return maxFloat(a ^ b);
        const float x = fl(a), y = fl(b);
        return finish(withRounding(FE_TONEAREST, [&] { return x / y; }));
    }

    uint32_t sqrtRef(uint32_t a)
    {
        a = daz(a) & 0x7FFFFFFFu;
        const float x = fl(a);
        return finish(withRounding(FE_TONEAREST, [&] { return std::sqrt(x); }));
    }

    uint32_t rsqrt(uint32_t a, uint32_t b)
    {
        a = daz(a);
        b = daz(b);
        if (isDenormOrZero(b))
            return maxFloat(a ^ b);
        const float x = fl(a), y = fl(b & 0x7FFFFFFFu);
        return finish(withRounding(FE_TONEAREST, [&] { const float s = std::sqrt(y); return x / s; }));
    }

    // Ordering of floats with denormals as zero and -0 == +0.
    int compare(uint32_t a, uint32_t b)
    {
        const float x = fl(daz(a)), y = fl(daz(b));
        return x < y ? -1 : x > y ? 1 : 0;
    }

    struct Out
    {
        uint32_t fd = 0, acc = 0;
        bool c = false;
        bool fdSet = false, accSet = false, cSet = false;
    };

    // Returns false for an unknown name.
    bool reference(const std::string &n, uint32_t fs, uint32_t ft, uint32_t acc, Out &o)
    {
        auto setFd = [&](uint32_t v) { o.fd = v; o.fdSet = true; };
        auto setAcc = [&](uint32_t v) { o.acc = v; o.accSet = true; };
        auto setC = [&](bool v) { o.c = v; o.cSet = true; };
        if (n == "ADD_S") return setFd(add(fs, ft, false)), true;
        if (n == "SUB_S") return setFd(add(fs, ft, true)), true;
        if (n == "MUL_S") return setFd(mul(fs, ft)), true;
        if (n == "DIV_S") return setFd(div(fs, ft)), true;
        if (n == "SQRT_S") return setFd(sqrtRef(ft)), true;
        if (n == "RSQRT_S") return setFd(rsqrt(fs, ft)), true;
        if (n == "ABS_S") return setFd(fs & 0x7FFFFFFFu), true;
        if (n == "MOV_S") return setFd(fs), true;
        if (n == "NEG_S") return setFd(fs ^ 0x80000000u), true;
        if (n == "ADDA_S") return setAcc(add(fs, ft, false)), true;
        if (n == "SUBA_S") return setAcc(add(fs, ft, true)), true;
        if (n == "MULA_S") return setAcc(mul(fs, ft)), true;
        if (n == "MADD_S") return setFd(add(acc, mul(fs, ft), false)), true;
        if (n == "MSUB_S") return setFd(add(acc, mul(fs, ft), true)), true;
        if (n == "MADDA_S") return setAcc(add(acc, mul(fs, ft), false)), true;
        if (n == "MSUBA_S") return setAcc(add(acc, mul(fs, ft), true)), true;
        if (n == "CVT_W_S")
        {
            const uint32_t a = daz(fs);
            int32_t r;
            if ((a & 0x7F800000u) >= 0x4F000000u)
                r = (a & 0x80000000u) ? INT32_MIN : INT32_MAX;
            else
                r = static_cast<int32_t>(fl(a)); // truncates
            return setFd(static_cast<uint32_t>(r)), true;
        }
        if (n == "CVT_S_W")
        {
            const int32_t i = static_cast<int32_t>(fs);
            return setFd(finish(withRounding(FE_TOWARDZERO, [&] { return static_cast<float>(i); }))), true;
        }
        if (n == "MAX_S") return setFd(compare(fs, ft) >= 0 ? fs : ft), true;
        if (n == "MIN_S") return setFd(compare(fs, ft) <= 0 ? fs : ft), true;
        if (n == "C_F_S") return setC(false), true;
        if (n == "C_EQ_S") return setC(compare(fs, ft) == 0), true;
        if (n == "C_LT_S") return setC(compare(fs, ft) < 0), true;
        if (n == "C_LE_S") return setC(compare(fs, ft) <= 0), true;
        return false;
    }

    // Values: random bit patterns of every class, weighted towards the interesting ones.
    uint32_t randomValue(std::mt19937 &rng, const std::string &n)
    {
        std::uniform_int_distribution<uint32_t> any;
        const uint32_t sign = any(rng) & 0x80000000u;
        switch (any(rng) % 12u)
        {
        case 0: return sign;                                              // +-0
        case 1: return sign | (any(rng) & 0x007FFFFFu);                   // denormal
        case 2: return sign | 0x7F7FFFFFu;                                // largest
        case 3: return sign | 0x7F800000u | (any(rng) & 0x007FFFFFu);     // inf/NaN bit patterns
        case 4: return sign | ((0xF0u + any(rng) % 15u) << 23) | (any(rng) & 0x007FFFFFu); // huge
        case 5: return sign | ((1u + any(rng) % 8u) << 23) | (any(rng) & 0x007FFFFFu);     // tiny
        case 6: return bits(static_cast<float>(static_cast<int32_t>(any(rng) % 2001u) - 1000)); // small ints
        case 7: return n == "CVT_S_W" ? any(rng) : sign | ((157u + any(rng) % 3u) << 23) | (any(rng) & 0x007FFFFFu); // near 2^31
        default: return sign | ((100u + any(rng) % 56u) << 23) | (any(rng) & 0x007FFFFFu); // ordinary
        }
    }

    // Classifies a difference for the summary.
    const char *kind(uint32_t got, uint32_t want, uint32_t fs, uint32_t ft, uint32_t acc)
    {
        if (isDenormOrZero(fs) || isDenormOrZero(ft) || isDenormOrZero(acc) || isDenormOrZero(got) || isDenormOrZero(want))
            if (!(got & 0x7F800000u) || !(want & 0x7F800000u) || (isDenormOrZero(fs) && fs & 0x7FFFFFu) ||
                (isDenormOrZero(ft) && ft & 0x7FFFFFu) || (isDenormOrZero(acc) && acc & 0x7FFFFFu))
                return "denormal";
        const int64_t d = static_cast<int64_t>(got) - static_cast<int64_t>(want);
        if ((got & 0x80000000u) == (want & 0x80000000u) && (d == 1 || d == -1))
            return "last bit";
        if ((got & 0x7F800000u) == 0x7F800000u)
            return "inf/nan";
        return "other";
    }
}

int main(int argc, char **argv)
{
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 20000;
    std::mt19937 rng(12345);
    int failed = 0, unknown = 0;
    std::printf("%-9s %8s %8s %8s %8s %8s   first difference (fs ft acc: got / want)\n", "", "cases", "wrong", "lastbit", "denorm", "other");
    for (const TestCase &tc : kCases)
    {
        std::map<std::string, int> kinds;
        int wrong = 0;
        std::string first;
        bool known = true;
        for (int i = 0; i < iterations && known; ++i)
        {
            const uint32_t fs = randomValue(rng, tc.name), ft = randomValue(rng, tc.name), acc = randomValue(rng, tc.name);
            Out want;
            if (!reference(tc.name, fs, ft, acc, want))
            {
                known = false;
                ++unknown;
                break;
            }
            R5900Context ctx{};
            ctx.f[1] = fl(fs);
            ctx.f[2] = fl(ft);
            ctx.f[3] = fl(0xDEADBEEFu);
            ctx.f_acc = fl(acc);
            ctx.fcr31 = (i & 1) ? 0x800000u : 0u;
            tc.fn(&ctx, nullptr, nullptr);
            const uint32_t gotFd = bits(ctx.f[3]), gotAcc = bits(ctx.f_acc);
            const bool gotC = (ctx.fcr31 & 0x800000u) != 0;
            const char *k = nullptr;
            uint32_t g = 0, w = 0;
            if (want.fdSet && gotFd != want.fd) { g = gotFd; w = want.fd; k = kind(g, w, fs, ft, acc); }
            else if (want.accSet && gotAcc != want.acc) { g = gotAcc; w = want.acc; k = kind(g, w, fs, ft, acc); }
            else if (want.cSet && gotC != want.c) { g = gotC; w = want.c; k = "other"; }
            if (!k)
                continue;
            ++wrong;
            ++kinds[k];
            if (first.empty())
            {
                char buf[160];
                std::snprintf(buf, sizeof buf, "%08x %08x %08x: %08x / %08x (%s)", fs, ft, acc, g, w, k);
                first = buf;
            }
        }
        if (!known)
        {
            std::printf("%-9s no reference\n", tc.name);
            continue;
        }
        if (wrong)
            ++failed;
        std::printf("%-9s %8d %8d %8d %8d %8d   %s\n", tc.name, iterations, wrong, kinds["last bit"], kinds["denormal"],
                    kinds["other"] + kinds["inf/nan"], first.c_str());
    }
    std::printf("%zu instructions, %d differ from the model, %d without reference\n", std::size(kCases), failed, unknown);
    return failed ? 1 : 0;
}
