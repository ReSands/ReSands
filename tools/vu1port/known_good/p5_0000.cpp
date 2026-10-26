// Known-good answer (by hand, 26 Oct; the end-to-end check of the P5 set-up): P5 stage "0000", the MSCAL entry of
// the particle program: a 4x4 matrix (vf9-12) and three column vectors (vf17-19) read with LQI from vi1 / vi2,
// their product rows into vf1, vf2 and ACC, four more rows (vf25-28) read on.
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
    void lqi(VuState &vu, int t, int s)
    {
        lq(vu, t, vu.vi[s]);
        vu.vi[s] = vi16(vu.vi[s] + 1);
    }
    // a 4x4 transform's product-sum chain: ACC = col0 * v.x + col1 * v.y + col2 * v.z (+ col3 * v.w into d)
    void mula(VuState &vu, const float *a, float b)
    {
        for (int c = 0; c < 4; ++c)
            vu.acc[c] = mul(a[c], b);
    }
    void madda(VuState &vu, const float *a, float b)
    {
        for (int c = 0; c < 4; ++c)
            vu.acc[c] = madd(vu.acc[c], a[c], b);
    }
    void maddv(VuState &vu, int d, const float *a, float b)
    {
        for (int c = 0; c < 4; ++c)
            vu.vf[d][c] = madd(vu.acc[c], a[c], b);
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    lqi(vu, 17, 2);                         // 0x00 LQI.xyzw vf17, (vi2++)
    lqi(vu, 9, 1);                          // 0x08 LQI.xyzw vf9, (vi1++)
    lqi(vu, 10, 1);                         // 0x10 LQI.xyzw vf10, (vi1++)
    lqi(vu, 11, 1);                         // 0x18 LQI.xyzw vf11, (vi1++)
    lqi(vu, 12, 1);                         // 0x20 LQI.xyzw vf12, (vi1++)
    mula(vu, vf[9], vf[17][0]);             // 0x28 MULAx.xyzw ACC, vf9, vf17x
    lqi(vu, 18, 2);                         //      LQI.xyzw vf18, (vi2++)
    madda(vu, vf[10], vf[17][1]);           // 0x30 MADDAy.xyzw ACC, vf10, vf17y
    lqi(vu, 19, 2);                         //      LQI.xyzw vf19, (vi2++)
    madda(vu, vf[11], vf[17][2]);           // 0x38 MADDAz.xyzw ACC, vf11, vf17z
    lqi(vu, 20, 2);                         //      LQI.xyzw vf20, (vi2++)
    maddv(vu, 1, vf[12], vf[17][3]);        // 0x40 MADDw.xyzw vf1, vf12, vf17w
    lqi(vu, 25, 1);                         //      LQI.xyzw vf25, (vi1++)
    mula(vu, vf[9], vf[18][0]);             // 0x48 MULAx.xyzw ACC, vf9, vf18x
    lqi(vu, 26, 1);                         //      LQI.xyzw vf26, (vi1++)
    madda(vu, vf[10], vf[18][1]);           // 0x50 MADDAy.xyzw ACC, vf10, vf18y
    lqi(vu, 27, 1);                         //      LQI.xyzw vf27, (vi1++)
    madda(vu, vf[11], vf[18][2]);           // 0x58 MADDAz.xyzw ACC, vf11, vf18z
    lqi(vu, 28, 1);                         //      LQI.xyzw vf28, (vi1++)
    maddv(vu, 2, vf[12], vf[18][3]);        // 0x60 MADDw.xyzw vf2, vf12, vf18w
    mula(vu, vf[9], vf[19][0]);             // 0x68 MULAx.xyzw ACC, vf9, vf19x
    madda(vu, vf[10], vf[19][1]);           // 0x70 MADDAy.xyzw ACC, vf10, vf19y
    madda(vu, vf[11], vf[19][2]);           // 0x78 MADDAz.xyzw ACC, vf11, vf19z
    vu.pc = 0x80;
}
