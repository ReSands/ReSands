// Compares PS2Recomp's translation of every MMI / pipeline-1 instruction (mmi_cases.inc, from
// tools/mmitest/gen.cpp) with a plain reference written from the EE instruction semantics, on
// random and edge-case register values. Prints one line per instruction that differs, with the
// first failing input. Build and run: tools/mmitest/run.sh (Mac) or run.ps1 (PC).
// Registers: rs = 5, rt = 6, rd = 7 (as generated); HI/LO are 128 bits ({lo, lo1}, {hi, hi1}).
#include "ps2_runtime_macros.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

struct TestCase
{
    const char *name;
    void (*fn)(R5900Context *, uint8_t *, PS2Runtime *);
};

#include "mmi_cases.inc"

namespace
{
    struct V
    {
        uint32_t w[4];
        int16_t h(int i) const { int16_t v; std::memcpy(&v, reinterpret_cast<const uint8_t *>(w) + 2 * i, 2); return v; }
        void seth(int i, int32_t v) { const int16_t x = static_cast<int16_t>(v); std::memcpy(reinterpret_cast<uint8_t *>(w) + 2 * i, &x, 2); }
        int8_t b(int i) const { return static_cast<int8_t>(reinterpret_cast<const uint8_t *>(w)[i]); }
        void setb(int i, int32_t v) { reinterpret_cast<uint8_t *>(w)[i] = static_cast<uint8_t>(v); }
        int32_t s(int i) const { return static_cast<int32_t>(w[i]); }
        uint64_t d(int k) const { return w[2 * k] | (static_cast<uint64_t>(w[2 * k + 1]) << 32); }
        void setd(int k, uint64_t v) { w[2 * k] = static_cast<uint32_t>(v); w[2 * k + 1] = static_cast<uint32_t>(v >> 32); }
    };

    struct State
    {
        V rs, rt, rd, hi, lo;
    };

    // Which words are defined after the instruction (bit i = word i); undefined ones are not compared.
    struct Care
    {
        unsigned rd = 0xF, hi = 0xF, lo = 0xF;
    };

    uint64_t sext32(uint64_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
    int32_t sat32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : static_cast<int32_t>(v); }
    int32_t sat16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : static_cast<int32_t>(v); }
    int32_t sat8(int64_t v) { return v > 127 ? 127 : v < -128 ? -128 : static_cast<int32_t>(v); }

    bool isDiv(const std::string &n) { return n == "DIV1" || n == "DIVU1" || n == "PDIVW" || n == "PDIVUW" || n == "PDIVBW"; }

    // Reference semantics. Returns false for an unknown name.
    bool reference(const std::string &n, const State &in, State &o, Care &care)
    {
        const V &rs = in.rs, &rt = in.rt;
        V &rd = o.rd, &hi = o.hi, &lo = o.lo;
        auto shiftOp = [&](const char *prefix) { return n.rfind(prefix, 0) == 0; };
        auto shiftAmount = [&]() { return std::stoi(n.substr(n.find('_') + 1)); };

        // --- direct MMI functs
        if (n == "MADD" || n == "MADDU" || n == "MADD1" || n == "MADDU1")
        {
            const bool one = n.back() == '1';
            const int k = one ? 1 : 0;
            const uint64_t acc = (static_cast<uint64_t>(in.hi.w[2 * k]) << 32) | in.lo.w[2 * k];
            const bool u = n == "MADDU" || n == "MADDU1";
            const uint64_t prod = u ? static_cast<uint64_t>(rs.w[0]) * rt.w[0]
                                    : static_cast<uint64_t>(static_cast<int64_t>(rs.s(0)) * rt.s(0));
            const uint64_t r = acc + prod;
            lo.setd(k, sext32(r));
            hi.setd(k, sext32(r >> 32));
            rd.setd(0, sext32(r));
            care.rd = 0x3;
            return true;
        }
        if (n == "MULT1" || n == "MULTU1")
        {
            const uint64_t r = n == "MULTU1" ? static_cast<uint64_t>(rs.w[0]) * rt.w[0]
                                             : static_cast<uint64_t>(static_cast<int64_t>(rs.s(0)) * rt.s(0));
            lo.setd(1, sext32(r));
            hi.setd(1, sext32(r >> 32));
            rd.setd(0, sext32(r));
            care.rd = 0x3;
            return true;
        }
        if (n == "DIV1")
        {
            lo.setd(1, sext32(static_cast<uint32_t>(rs.s(0) / rt.s(0))));
            hi.setd(1, sext32(static_cast<uint32_t>(rs.s(0) % rt.s(0))));
            return true;
        }
        if (n == "DIVU1")
        {
            lo.setd(1, sext32(rs.w[0] / rt.w[0]));
            hi.setd(1, sext32(rs.w[0] % rt.w[0]));
            return true;
        }
        if (n == "MFHI1") { rd.setd(0, in.hi.d(1)); care.rd = 0x3; return true; }
        if (n == "MFLO1") { rd.setd(0, in.lo.d(1)); care.rd = 0x3; return true; }
        if (n == "MTHI1") { hi.setd(1, rs.d(0)); return true; }
        if (n == "MTLO1") { lo.setd(1, rs.d(0)); return true; }
        if (n == "PLZCW")
        {
            for (int i = 0; i < 2; ++i)
            {
                uint32_t x = rs.w[i];
                if (x & 0x80000000u)
                    x = ~x;
                rd.w[i] = static_cast<uint32_t>((x ? std::countl_zero(x) : 32) - 1);
            }
            care.rd = 0x3;
            return true;
        }
        if (n == "PMFHL_LW") { rd = {{in.lo.w[0], in.hi.w[0], in.lo.w[2], in.hi.w[2]}}; return true; }
        if (n == "PMFHL_UW") { rd = {{in.lo.w[1], in.hi.w[1], in.lo.w[3], in.hi.w[3]}}; return true; }
        if (n == "PMFHL_SLW")
        {
            for (int k = 0; k < 2; ++k)
            {
                const int64_t v = static_cast<int64_t>((static_cast<uint64_t>(in.hi.w[2 * k]) << 32) | in.lo.w[2 * k]);
                rd.setd(k, static_cast<uint64_t>(static_cast<int64_t>(sat32(v))));
            }
            return true;
        }
        if (n == "PMFHL_LH")
        {
            const int16_t v[8] = {in.lo.h(0), in.lo.h(2), in.hi.h(0), in.hi.h(2), in.lo.h(4), in.lo.h(6), in.hi.h(4), in.hi.h(6)};
            for (int i = 0; i < 8; ++i)
                rd.seth(i, v[i]);
            return true;
        }
        if (n == "PMFHL_SH")
        {
            const int32_t v[8] = {in.lo.s(0), in.lo.s(1), in.hi.s(0), in.hi.s(1), in.lo.s(2), in.lo.s(3), in.hi.s(2), in.hi.s(3)};
            for (int i = 0; i < 8; ++i)
                rd.seth(i, sat16(v[i]));
            return true;
        }
        if (n == "PMTHL_LW") { lo.w[0] = rs.w[0]; hi.w[0] = rs.w[1]; lo.w[2] = rs.w[2]; hi.w[2] = rs.w[3]; return true; }
        if (shiftOp("PSLLH_") || shiftOp("PSRLH_") || shiftOp("PSRAH_"))
        {
            const int sa = shiftAmount() & 15;
            for (int i = 0; i < 8; ++i)
            {
                const uint16_t x = static_cast<uint16_t>(rt.h(i));
                rd.seth(i, n[3] == 'L' && n[2] == 'L' ? x << sa : n[3] == 'L' ? x >> sa : rt.h(i) >> sa);
            }
            return true;
        }
        if (shiftOp("PSLLW_") || shiftOp("PSRLW_") || shiftOp("PSRAW_"))
        {
            const int sa = shiftAmount() & 31;
            for (int i = 0; i < 4; ++i)
                rd.w[i] = n[2] == 'L' ? rt.w[i] << sa : n[3] == 'L' ? rt.w[i] >> sa : static_cast<uint32_t>(rt.s(i) >> sa);
            return true;
        }

        // --- word lanes
        auto words = [&](auto f) { for (int i = 0; i < 4; ++i) rd.w[i] = static_cast<uint32_t>(f(i)); return true; };
        auto halves = [&](auto f) { for (int i = 0; i < 8; ++i) rd.seth(i, f(i)); return true; };
        auto bytes = [&](auto f) { for (int i = 0; i < 16; ++i) rd.setb(i, f(i)); return true; };
        if (n == "PADDW") return words([&](int i) { return rs.w[i] + rt.w[i]; });
        if (n == "PSUBW") return words([&](int i) { return rs.w[i] - rt.w[i]; });
        if (n == "PCGTW") return words([&](int i) { return rs.s(i) > rt.s(i) ? 0xFFFFFFFFu : 0u; });
        if (n == "PCEQW") return words([&](int i) { return rs.w[i] == rt.w[i] ? 0xFFFFFFFFu : 0u; });
        if (n == "PMAXW") return words([&](int i) { return std::max(rs.s(i), rt.s(i)); });
        if (n == "PMINW") return words([&](int i) { return std::min(rs.s(i), rt.s(i)); });
        if (n == "PADDSW") return words([&](int i) { return sat32(static_cast<int64_t>(rs.s(i)) + rt.s(i)); });
        if (n == "PSUBSW") return words([&](int i) { return sat32(static_cast<int64_t>(rs.s(i)) - rt.s(i)); });
        if (n == "PADDUW") return words([&](int i) { const uint64_t v = static_cast<uint64_t>(rs.w[i]) + rt.w[i]; return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(v); });
        if (n == "PSUBUW") return words([&](int i) { return rs.w[i] > rt.w[i] ? rs.w[i] - rt.w[i] : 0u; });
        if (n == "PABSW") return words([&](int i) { const int32_t x = rt.s(i); return x == INT32_MIN ? INT32_MAX : (x < 0 ? -x : x); });
        if (n == "PEXTLW") return words([&](int i) { return (i & 1) ? rs.w[i / 2] : rt.w[i / 2]; });
        if (n == "PEXTUW") return words([&](int i) { return (i & 1) ? rs.w[2 + i / 2] : rt.w[2 + i / 2]; });
        if (n == "PPACW") { rd = {{rt.w[0], rt.w[2], rs.w[0], rs.w[2]}}; return true; }
        if (n == "PCPYLD") { rd = {{rt.w[0], rt.w[1], rs.w[0], rs.w[1]}}; return true; }
        if (n == "PCPYUD") { rd = {{rs.w[2], rs.w[3], rt.w[2], rt.w[3]}}; return true; }
        if (n == "PEXEW") { rd = {{rt.w[2], rt.w[1], rt.w[0], rt.w[3]}}; return true; }
        if (n == "PEXCW") { rd = {{rt.w[0], rt.w[2], rt.w[1], rt.w[3]}}; return true; }
        if (n == "PROT3W") { rd = {{rt.w[1], rt.w[2], rt.w[0], rt.w[3]}}; return true; }
        if (n == "PAND") return words([&](int i) { return rs.w[i] & rt.w[i]; });
        if (n == "POR") return words([&](int i) { return rs.w[i] | rt.w[i]; });
        if (n == "PXOR") return words([&](int i) { return rs.w[i] ^ rt.w[i]; });
        if (n == "PNOR") return words([&](int i) { return ~(rs.w[i] | rt.w[i]); });
        if (n == "PSLLVW" || n == "PSRLVW" || n == "PSRAVW")
        {
            for (int k = 0; k < 2; ++k)
            {
                const uint32_t x = rt.w[2 * k], sa = rs.w[2 * k] & 31u;
                const uint32_t r = n == "PSLLVW" ? x << sa : n == "PSRLVW" ? x >> sa : static_cast<uint32_t>(static_cast<int32_t>(x) >> sa);
                rd.setd(k, sext32(r));
            }
            return true;
        }
        if (n == "PEXT5") return words([&](int i) { const uint32_t x = rt.w[i]; return ((x & 0x1F) << 3) | (((x >> 5) & 0x1F) << 11) | (((x >> 10) & 0x1F) << 19) | ((x & 0x8000) << 16); });
        if (n == "PPAC5") return words([&](int i) { const uint32_t x = rt.w[i]; return ((x >> 3) & 0x1F) | (((x >> 11) & 0x1F) << 5) | (((x >> 19) & 0x1F) << 10) | ((x >> 16) & 0x8000); });

        // --- halfword lanes
        if (n == "PADDH") return halves([&](int i) { return rs.h(i) + rt.h(i); });
        if (n == "PSUBH") return halves([&](int i) { return rs.h(i) - rt.h(i); });
        if (n == "PCGTH") return halves([&](int i) { return rs.h(i) > rt.h(i) ? -1 : 0; });
        if (n == "PCEQH") return halves([&](int i) { return rs.h(i) == rt.h(i) ? -1 : 0; });
        if (n == "PMAXH") return halves([&](int i) { return std::max(rs.h(i), rt.h(i)); });
        if (n == "PMINH") return halves([&](int i) { return std::min(rs.h(i), rt.h(i)); });
        if (n == "PADDSH") return halves([&](int i) { return sat16(rs.h(i) + rt.h(i)); });
        if (n == "PSUBSH") return halves([&](int i) { return sat16(rs.h(i) - rt.h(i)); });
        if (n == "PADDUH") return halves([&](int i) { return std::min<int32_t>(static_cast<uint16_t>(rs.h(i)) + static_cast<uint16_t>(rt.h(i)), 0xFFFF); });
        if (n == "PSUBUH") return halves([&](int i) { return std::max<int32_t>(static_cast<uint16_t>(rs.h(i)) - static_cast<uint16_t>(rt.h(i)), 0); });
        if (n == "PABSH") return halves([&](int i) { const int32_t x = rt.h(i); return x == -32768 ? 32767 : (x < 0 ? -x : x); });
        if (n == "PADSBH") return halves([&](int i) { return i < 4 ? rs.h(i) - rt.h(i) : rs.h(i) + rt.h(i); });
        if (n == "PEXTLH") return halves([&](int i) { return (i & 1) ? rs.h(i / 2) : rt.h(i / 2); });
        if (n == "PEXTUH") return halves([&](int i) { return (i & 1) ? rs.h(4 + i / 2) : rt.h(4 + i / 2); });
        if (n == "PPACH") return halves([&](int i) { return i < 4 ? rt.h(2 * i) : rs.h(2 * (i - 4)); });
        if (n == "PINTH") return halves([&](int i) { return (i & 1) ? rs.h(4 + i / 2) : rt.h(i / 2); });
        if (n == "PINTEH") return halves([&](int i) { return (i & 1) ? rs.h(i - 1) : rt.h(i); });
        if (n == "PCPYH") return halves([&](int i) { return rt.h(i < 4 ? 0 : 4); });
        if (n == "PEXEH") { static const int m[8] = {2, 1, 0, 3, 6, 5, 4, 7}; return halves([&](int i) { return rt.h(m[i]); }); }
        if (n == "PEXCH") { static const int m[8] = {0, 2, 1, 3, 4, 6, 5, 7}; return halves([&](int i) { return rt.h(m[i]); }); }
        if (n == "PREVH") { static const int m[8] = {3, 2, 1, 0, 7, 6, 5, 4}; return halves([&](int i) { return rt.h(m[i]); }); }

        // --- byte lanes
        if (n == "PADDB") return bytes([&](int i) { return rs.b(i) + rt.b(i); });
        if (n == "PSUBB") return bytes([&](int i) { return rs.b(i) - rt.b(i); });
        if (n == "PCGTB") return bytes([&](int i) { return rs.b(i) > rt.b(i) ? -1 : 0; });
        if (n == "PCEQB") return bytes([&](int i) { return rs.b(i) == rt.b(i) ? -1 : 0; });
        if (n == "PADDSB") return bytes([&](int i) { return sat8(rs.b(i) + rt.b(i)); });
        if (n == "PSUBSB") return bytes([&](int i) { return sat8(rs.b(i) - rt.b(i)); });
        if (n == "PADDUB") return bytes([&](int i) { return std::min<int32_t>(static_cast<uint8_t>(rs.b(i)) + static_cast<uint8_t>(rt.b(i)), 0xFF); });
        if (n == "PSUBUB") return bytes([&](int i) { return std::max<int32_t>(static_cast<uint8_t>(rs.b(i)) - static_cast<uint8_t>(rt.b(i)), 0); });
        if (n == "PEXTLB") return bytes([&](int i) { return (i & 1) ? rs.b(i / 2) : rt.b(i / 2); });
        if (n == "PEXTUB") return bytes([&](int i) { return (i & 1) ? rs.b(8 + i / 2) : rt.b(8 + i / 2); });
        if (n == "PPACB") return bytes([&](int i) { return i < 8 ? rt.b(2 * i) : rs.b(2 * (i - 8)); });

        // --- HI/LO
        if (n == "PMFHI") { rd = in.hi; return true; }
        if (n == "PMFLO") { rd = in.lo; return true; }
        if (n == "PMTHI") { hi = rs; return true; }
        if (n == "PMTLO") { lo = rs; return true; }
        if (n == "PMULTW" || n == "PMULTUW" || n == "PMADDW" || n == "PMADDUW" || n == "PMSUBW")
        {
            const bool u = n == "PMULTUW" || n == "PMADDUW";
            for (int k = 0; k < 2; ++k)
            {
                const int i = 2 * k;
                const uint64_t prod = u ? static_cast<uint64_t>(rs.w[i]) * rt.w[i]
                                        : static_cast<uint64_t>(static_cast<int64_t>(rs.s(i)) * rt.s(i));
                const uint64_t acc = (static_cast<uint64_t>(in.hi.w[i]) << 32) | in.lo.w[i];
                const uint64_t r = n.find("MADD") != std::string::npos ? acc + prod : n == "PMSUBW" ? acc - prod : prod;
                lo.setd(k, sext32(r));
                hi.setd(k, sext32(r >> 32));
                rd.setd(k, r);
            }
            return true;
        }
        if (n == "PDIVW" || n == "PDIVUW")
        {
            for (int k = 0; k < 2; ++k)
            {
                const int i = 2 * k;
                if (n == "PDIVW")
                {
                    lo.setd(k, sext32(static_cast<uint32_t>(rs.s(i) / rt.s(i))));
                    hi.setd(k, sext32(static_cast<uint32_t>(rs.s(i) % rt.s(i))));
                }
                else
                {
                    lo.setd(k, sext32(rs.w[i] / rt.w[i]));
                    hi.setd(k, sext32(rs.w[i] % rt.w[i]));
                }
            }
            return true;
        }
        if (n == "PDIVBW")
        {
            const int32_t d = rt.h(0);
            for (int i = 0; i < 4; ++i)
            {
                lo.w[i] = static_cast<uint32_t>(rs.s(i) / d);
                hi.w[i] = static_cast<uint32_t>(rs.s(i) % d);
            }
            return true;
        }
        if (n == "PMULTH" || n == "PMADDH" || n == "PMSUBH" || n == "PHMADH" || n == "PHMSBH")
        {
            int32_t p[8];
            for (int i = 0; i < 8; ++i)
                p[i] = static_cast<int32_t>(rs.h(i)) * rt.h(i);
            if (n == "PHMADH" || n == "PHMSBH")
            {
                const bool add = n == "PHMADH";
                auto f = [&](int a, int b) { return static_cast<uint32_t>(add ? p[b] + p[a] : p[b] - p[a]); };
                lo.w[0] = f(0, 1); hi.w[0] = f(2, 3); lo.w[2] = f(4, 5); hi.w[2] = f(6, 7);
                care.lo = care.hi = 0x5;
            }
            else
            {
                const int sign = n == "PMSUBH" ? -1 : 1;
                const bool acc = n != "PMULTH";
                uint32_t *dst[8] = {&lo.w[0], &lo.w[1], &hi.w[0], &hi.w[1], &lo.w[2], &lo.w[3], &hi.w[2], &hi.w[3]};
                for (int i = 0; i < 8; ++i)
                    *dst[i] = static_cast<uint32_t>((acc ? static_cast<int32_t>(*dst[i]) : 0) + sign * p[i]);
            }
            rd = {{lo.w[0], hi.w[0], lo.w[2], hi.w[2]}};
            return true;
        }
        return false;
    }

    V load(const __m128i &v) { V x; std::memcpy(x.w, &v, 16); return x; }
    __m128i store(const V &x) { __m128i v; std::memcpy(&v, x.w, 16); return v; }

    uint32_t interesting(std::mt19937 &rng)
    {
        static const uint32_t edge[] = {0u, 1u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x7FFF7FFFu, 0x80008000u, 0x00010001u, 0x7F7F7F7Fu, 0x80808080u, 0xFFFF0000u, 0x0000FFFFu};
        const uint32_t r = rng();
        switch (r % 4u)
        {
        case 0: return edge[rng() % std::size(edge)];
        case 1: return rng() & 0xFFu;
        default: return rng();
        }
    }
}

int main(int argc, char **argv)
{
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 20000;
    std::mt19937 rng(12345);
    alignas(16) static R5900Context ctx;
    int failed = 0, unknown = 0;
    for (const TestCase &tc : kCases)
    {
        const std::string name = tc.name;
        int bad = 0;
        for (int it = 0; it < iterations && bad == 0; ++it)
        {
            State in{};
            for (V *v : {&in.rs, &in.rt, &in.rd, &in.hi, &in.lo})
                for (uint32_t &w : v->w)
                    w = interesting(rng);
            if (isDiv(name))
            {
                // Divide by zero and INT_MIN / -1 are left out (results are hardware-specific).
                for (int i = 0; i < 4; ++i)
                {
                    if (name == "PDIVBW")
                    {
                        if (in.rt.h(0) == 0 || in.rt.h(0) == -1) in.rt.seth(0, 3);
                    }
                    else if (in.rt.w[i] == 0u || in.rt.w[i] == 0xFFFFFFFFu)
                        in.rt.w[i] = 7u;
                }
            }
            State want = in;
            Care care;
            if (!reference(name, in, want, care))
            {
                ++unknown;
                std::printf("%-10s no reference\n", tc.name);
                break;
            }
            std::memset(&ctx, 0, sizeof(ctx));
            ctx.r[5] = store(in.rs);
            ctx.r[6] = store(in.rt);
            ctx.r[7] = store(in.rd);
            ctx.lo = in.lo.d(0); ctx.lo1 = in.lo.d(1);
            ctx.hi = in.hi.d(0); ctx.hi1 = in.hi.d(1);
            tc.fn(&ctx, nullptr, nullptr);
            State got{load(ctx.r[5]), load(ctx.r[6]), load(ctx.r[7]), {}, {}};
            got.lo.setd(0, ctx.lo); got.lo.setd(1, ctx.lo1);
            got.hi.setd(0, ctx.hi); got.hi.setd(1, ctx.hi1);
            auto differs = [](const V &a, const V &b, unsigned mask) {
                for (int i = 0; i < 4; ++i)
                    if ((mask >> i & 1u) && a.w[i] != b.w[i]) return true;
                return false;
            };
            const char *what = differs(got.rd, want.rd, care.rd) ? "rd" : differs(got.hi, want.hi, care.hi) ? "HI"
                             : differs(got.lo, want.lo, care.lo) ? "LO" : differs(got.rs, in.rs, 0xF) || differs(got.rt, in.rt, 0xF) ? "source" : nullptr;
            if (what)
            {
                ++bad;
                ++failed;
                auto hex = [](const V &v) { static char buf[4][48]; static int k; char *b = buf[k++ & 3]; std::snprintf(b, 48, "%08x %08x %08x %08x", v.w[3], v.w[2], v.w[1], v.w[0]); return b; };
                std::printf("%-10s WRONG %s\n  rs %s  rt %s\n  hi %s  lo %s\n", tc.name, what, hex(in.rs), hex(in.rt), hex(in.hi), hex(in.lo));
                const V &g = what[0] == 'r' ? got.rd : what[0] == 'H' ? got.hi : what[0] == 'L' ? got.lo : got.rs;
                const V &w = what[0] == 'r' ? want.rd : what[0] == 'H' ? want.hi : what[0] == 'L' ? want.lo : in.rs;
                std::printf("  got  %s\n  want %s\n", hex(g), hex(w));
            }
        }
    }
    std::printf("%zu instructions, %d wrong, %d without reference\n", std::size(kCases), failed, unknown);
    return failed ? 1 : 0;
}
