// Known-good answer (by hand, 26 Oct; Qwen failed every case in 9 rounds): P4 stage "3630", the end of the
// two-bone blend (vf16 position, vf21 normal by the weights vf23), the light vector vf14 = qword 1006 - vf16
// and its squares vf13, the next vertex's loads and the light counts (words 6 and 7 .w, low 4 bits).
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
    madda(vu, vf[11], vf[20][2]);           // 0x3630 MADDAz.xyzw ACC, vf11, vf20z
    vi[6] = vi16(vi[2] + 1);                //        IADDIU vi6, vi2, 1
    maddv(vu, 26, vf[12], vf[0][3]);        // 0x3638 MADDw.xyzw vf26, vf12, vf0w
    vi[10] = vi16(vi[10] & vi[11]);         //        IAND vi10, vi10, vi11
    vf[27][1] = add(vf[0][1], vu.i);        // 0x3640 ADDi.y vf27, vf0, I
    lq(vu, 14, 1006);                       //        LQ.xyzw vf14, 1006(vi0)
    mula(vu, vf[24], vf[23][0]);            // 0x3648 MULAx.xyzw ACC, vf24, vf23x
    vi[11] = 15;                            //        IADDIU vi11, vi0, 15
    madda(vu, vf[25], vf[23][1]);           // 0x3650 MADDAy.xyzw ACC, vf25, vf23y
    vi[12] = ilw(vu, 7, 3);                 //        ILW.w vi12, 7(vi0)
    maddv(vu, 16, vf[26], vf[23][2], 0xE);  // 0x3658 MADDz.xyz vf16, vf26, vf23z
    vi[2] = ilw(vu, 6, 3);                  //        ILW.w vi2, 6(vi0)
    mula(vu, vf[9], vf[21][0]);             // 0x3660 MULAx.xyzw ACC, vf9, vf21x
    vi[3] = vi16(vi[1] + 1);                //        IADDIU vi3, vi1, 1
    madda(vu, vf[10], vf[21][1]);           // 0x3668 MADDAy.xyzw ACC, vf10, vf21y
    vi[7] = 1;                              //        IADDIU vi7, vi0, 1
    maddv(vu, 19, vf[11], vf[21][2]);       // 0x3670 MADDz.xyzw vf19, vf11, vf21z
    vi[12] = vi16(vi[12] & vi[11]);         //        IAND vi12, vi12, vi11
    for (int c = 0; c < 3; ++c)             // 0x3678 SUB.xyz vf14, vf14, vf16
        vf[14][c] = sub(vf[14][c], vf[16][c]);
    vi[2] = vi16(vi[2] & vi[11]);           //        IAND vi2, vi2, vi11
    mula(vu, vf[28], vf[23][0]);            // 0x3680 MULAx.xyzw ACC, vf28, vf23x
    lq(vu, 17, vi[3] + 120);                //        LQ.xyzw vf17, 120(vi3)
    madda(vu, vf[18], vf[23][1]);           // 0x3688 MADDAy.xyzw ACC, vf18, vf23y
    lq(vu, 30, vi[3] + 1, 0xE);             //        LQ.xyz vf30, 1(vi3)
    maddv(vu, 21, vf[19], vf[23][2], 0xE);  // 0x3690 MADDz.xyz vf21, vf19, vf23z
    lq(vu, 31, 24);                         //        LQ.xyzw vf31, 24(vi0)
    for (int c = 0; c < 4; ++c)             // 0x3698 MUL.xyzw vf13, vf14, vf14
        vf[13][c] = mul(vf[14][c], vf[14][c]);
    vu.pc = 0x36a0;
}
