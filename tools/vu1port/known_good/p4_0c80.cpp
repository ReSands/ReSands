// Known-good answer (by hand, 26 Oct; Qwen failed 1 case of 594 in 9 rounds, the same code each time): P4 stage
// "0c80", two bone matrices (qwords 900-903: vi8's bone, loaded before, in vf5-8; vi7's in vf9-12) applied to the
// vertex vf20, then blended by the weights at 41(vi3). The failing case: an overflowing product next to a large
// opposite ACC; the reference judges a product-sum on its exact value (fix / madd below), not a clamped product.
#include "vu_task.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace
{
    // VU operands: denormals read as zero, Inf/NaN as the largest float (sign kept)
    float norm(float v)
    {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        const uint32_t e = b & 0x7f800000u;
        if (e == 0)
            b &= 0x80000000u;
        else if (e == 0x7f800000u)
            b = (b & 0x80000000u) | 0x7f7fffffu;
        std::memcpy(&v, &b, 4);
        return v;
    }
    // A result as the reference interpreter lands it: judged on the exact value (one rounding for a product-sum,
    // no clamp of the product on its own), so an overflowing product with a large opposite ACC still saturates
    float fix(float r, long double exact)
    {
        const long double m = std::fabs(exact);
        const float zero = std::signbit(exact) ? -0.0f : 0.0f;
        if (m == 0.0L || m < (long double)std::numeric_limits<float>::min())
            return zero;
        if (m > (long double)std::numeric_limits<float>::max())
            return std::signbit(exact) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        return r;
    }
    using LD = long double;
    float add(float a, float b) { a = norm(a); b = norm(b); return fix(a + b, LD(a) + LD(b)); }
    float sub(float a, float b) { a = norm(a); b = norm(b); return fix(a - b, LD(a) - LD(b)); }
    float mul(float a, float b) { a = norm(a); b = norm(b); return fix(a * b, LD(a) * LD(b)); }
    float madd(float c, float a, float b) { c = norm(c); a = norm(a); b = norm(b); return fix(c + a * b, LD(c) + LD(a) * LD(b)); }
    float msub(float c, float a, float b) { c = norm(c); a = norm(a); b = norm(b); return fix(c - a * b, LD(c) - LD(a) * LD(b)); }
    float vmax(float a, float b) { return norm(a) > norm(b) ? norm(a) : norm(b); }
    float vmin(float a, float b) { return norm(a) < norm(b) ? norm(a) : norm(b); }
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }
    void lq(VuState &vu, int t, int32_t qword, unsigned mask = 0xF) // mask: x = 8, y = 4, z = 2, w = 1
    {
        const uint8_t *src = vu.mem + 16 * (uint32_t(qword) & 0x3FF);
        if (t == 0)
            return;
        for (int c = 0; c < 4; ++c)
            if (mask & (8u >> c))
                std::memcpy(&vu.vf[t][c], src + 4 * c, 4);
    }
    void sq(VuState &vu, const float *v, int32_t qword, unsigned mask = 0xF)
    {
        uint8_t *dst = vu.mem + 16 * (uint32_t(qword) & 0x3FF);
        for (int c = 0; c < 4; ++c)
            if (mask & (8u >> c))
                std::memcpy(dst + 4 * c, &v[c], 4);
    }
    int32_t ilw(const VuState &vu, int32_t qword, int lane)
    {
        uint32_t v;
        std::memcpy(&v, vu.mem + 16 * (uint32_t(qword) & 0x3FF) + 4 * lane, 4);
        return vi16(int32_t(v & 0xFFFF));
    }
    // ACC / vf product-sum rows of the 4x4 transforms this program is made of: d = ACC-chain over the masked lanes
    void mula(VuState &vu, const float *a, float b, unsigned mask = 0xF)
    {
        for (int c = 0; c < 4; ++c)
            if (mask & (8u >> c))
                vu.acc[c] = mul(a[c], b);
    }
    void madda(VuState &vu, const float *a, float b, unsigned mask = 0xF)
    {
        for (int c = 0; c < 4; ++c)
            if (mask & (8u >> c))
                vu.acc[c] = madd(vu.acc[c], a[c], b);
    }
    void maddv(VuState &vu, int d, const float *a, float b, unsigned mask = 0xF)
    {
        float r[4];
        for (int c = 0; c < 4; ++c)
            r[c] = madd(vu.acc[c], a[c], b);
        if (d != 0)
            for (int c = 0; c < 4; ++c)
                if (mask & (8u >> c))
                    vu.vf[d][c] = r[c];
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    const int32_t *vi = vu.vi;
    mula(vu, vf[5], vf[20][0]);             // 0xc80 MULAx.xyzw ACC, vf5, vf20x
    lq(vu, 9, vi[7] + 900);                 //       LQ.xyzw vf9, 900(vi7)
    madda(vu, vf[6], vf[20][1]);            // 0xc88 MADDAy.xyzw ACC, vf6, vf20y
    lq(vu, 10, vi[7] + 901);                //       LQ.xyzw vf10, 901(vi7)
    madda(vu, vf[7], vf[20][2]);            // 0xc90 MADDAz.xyzw ACC, vf7, vf20z
    lq(vu, 11, vi[7] + 902);                //       LQ.xyzw vf11, 902(vi7)
    maddv(vu, 25, vf[8], vf[0][3]);         // 0xc98 MADDw.xyzw vf25, vf8, vf0w
    lq(vu, 12, vi[7] + 903);                //       LQ.xyzw vf12, 903(vi7)
    mula(vu, vf[9], vf[20][0]);             // 0xca0 MULAx.xyzw ACC, vf9, vf20x
    lq(vu, 24, vi[3] + 41, 0xE);            //       LQ.xyz vf24, 41(vi3)
    madda(vu, vf[10], vf[20][1]);           // 0xca8 MADDAy.xyzw ACC, vf10, vf20y
    madda(vu, vf[11], vf[20][2]);           // 0xcb0 MADDAz.xyzw ACC, vf11, vf20z
    maddv(vu, 26, vf[12], vf[0][3]);        // 0xcb8 MADDw.xyzw vf26, vf12, vf0w
    mula(vu, vf[16], vf[24][0]);            // 0xcc0 MULAx.xyzw ACC, vf16, vf24x
    madda(vu, vf[25], vf[24][1]);           // 0xcc8 MADDAy.xyzw ACC, vf25, vf24y
    maddv(vu, 16, vf[26], vf[24][2]);       // 0xcd0 MADDz.xyzw vf16, vf26, vf24z
    vu.pc = 0xcd8;
}
