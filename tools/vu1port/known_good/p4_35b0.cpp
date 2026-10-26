// Known-good answer (by hand, 26 Oct; Qwen failed every case in 9 rounds): P4 stage "35b0", vertex vf20 and
// normal vf21 through two bones: first the bone already in vf9-12 (vf24 position, vf28 normal), then vi5's
// (vf25, vf18), loading the next bone (vi13) as it goes; copies qword 0(vi1) to 202 further on.
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
    int32_t *vi = vu.vi;
    mula(vu, vf[9], vf[20][0]);             // 0x35b0 MULAx.xyzw ACC, vf9, vf20x
    lq(vu, 5, vi[5] + 900);                 //        LQ.xyzw vf5, 900(vi5)
    madda(vu, vf[10], vf[20][1]);           // 0x35b8 MADDAy.xyzw ACC, vf10, vf20y
    lq(vu, 6, vi[5] + 901);                 //        LQ.xyzw vf6, 901(vi5)
    madda(vu, vf[11], vf[20][2]);           // 0x35c0 MADDAz.xyzw ACC, vf11, vf20z
    lq(vu, 7, vi[5] + 902);                 //        LQ.xyzw vf7, 902(vi5)
    maddv(vu, 24, vf[12], vf[0][3]);        // 0x35c8 MADDw.xyzw vf24, vf12, vf0w
    lq(vu, 8, vi[5] + 903);                 //        LQ.xyzw vf8, 903(vi5)
    mula(vu, vf[9], vf[21][0]);             // 0x35d0 MULAx.xyzw ACC, vf9, vf21x
    vi[11] = 32767;                         //        IADDIU vi11, vi0, 32767
    madda(vu, vf[10], vf[21][1]);           // 0x35d8 MADDAy.xyzw ACC, vf10, vf21y
    maddv(vu, 28, vf[11], vf[21][2]);       // 0x35e0 MADDz.xyzw vf28, vf11, vf21z
    mula(vu, vf[5], vf[20][0]);             // 0x35e8 MULAx.xyzw ACC, vf5, vf20x
    lq(vu, 9, vi[13] + 900);                //        LQ.xyzw vf9, 900(vi13)
    madda(vu, vf[6], vf[20][1]);            // 0x35f0 MADDAy.xyzw ACC, vf6, vf20y
    lq(vu, 10, vi[13] + 901);               //        LQ.xyzw vf10, 901(vi13)
    madda(vu, vf[7], vf[20][2]);            // 0x35f8 MADDAz.xyzw ACC, vf7, vf20z
    lq(vu, 11, vi[13] + 902);               //        LQ.xyzw vf11, 902(vi13)
    maddv(vu, 25, vf[8], vf[0][3]);         // 0x3600 MADDw.xyzw vf25, vf8, vf0w
    lq(vu, 12, vi[13] + 903);               //        LQ.xyzw vf12, 903(vi13)
    mula(vu, vf[5], vf[21][0]);             // 0x3608 MULAx.xyzw ACC, vf5, vf21x
    lq(vu, 31, vi[1]);                      //        LQ.xyzw vf31, 0(vi1)
    madda(vu, vf[6], vf[21][1]);            // 0x3610 MADDAy.xyzw ACC, vf6, vf21y
    vi[2] = vi16(vi[1] + 202);              //        IADDIU vi2, vi1, 202
    maddv(vu, 18, vf[7], vf[21][2]);        // 0x3618 MADDz.xyzw vf18, vf7, vf21z
    lq(vu, 23, vi[1] + 41);                 //        LQ.xyzw vf23, 41(vi1)
    mula(vu, vf[9], vf[20][0]);             // 0x3620 MULAx.xyzw ACC, vf9, vf20x
    vu.i = 0.03125f;                        //        LOI 0x3d000000
    madda(vu, vf[10], vf[20][1]);           // 0x3628 MADDAy.xyzw ACC, vf10, vf20y
    sq(vu, vf[31], vi[2]);                  //        SQ.xyzw vf31, 0(vi2)
    vu.pc = 0x3630;
}
