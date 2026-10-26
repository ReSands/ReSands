// Known-good answer (by hand, 26 Oct): P5 stage "04b8", one particle's transforms: vf23 through the matrix in
// vf1-vf4 (vf9, then 1 / w into Q) and vf5-vf8 (vf13), vf24 through vf1-vf4 (vf10); vf14 = vf9 * Q with a size in
// w; the colour (vf21), vf13 and vf18 stored to the output buffer.
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
    // a result judged on the exact value (one rounding for a product-sum, no clamp of the product on its own)
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
    float mul(float a, float b) { a = norm(a); b = norm(b); return fix(a * b, LD(a) * LD(b)); }
    float madd(float c, float a, float b) { c = norm(c); a = norm(a); b = norm(b); return fix(c + a * b, LD(c) + LD(a) * LD(b)); }
    float vmax(float a, float b) { return norm(a) > norm(b) ? norm(a) : norm(b); }
    float vmin(float a, float b) { return norm(a) < norm(b) ? norm(a) : norm(b); }
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }
    void lq(VuState &vu, int t, int32_t qword)
    {
        if (t != 0)
            std::memcpy(vu.vf[t], vu.mem + 16 * (uint32_t(qword) & 0x3FF), 16);
    }
    void sq(VuState &vu, int s, int32_t qword) { std::memcpy(vu.mem + 16 * (uint32_t(qword) & 0x3FF), vu.vf[s], 16); }
    // DIV Q, a, b as the reference lands it: divide by zero -> the largest float, sign of a ^ sign of b
    float vdiv(float a, float b)
    {
        a = norm(a);
        b = norm(b);
        if (b == 0.0f)
            return std::signbit(a) != std::signbit(b) ? -std::numeric_limits<float>::max()
                                                      : std::numeric_limits<float>::max();
        return norm(a / b);
    }
    // a 4x4 transform's product-sum chain: ACC = col0 * v.x + col1 * v.y + col2 * v.z, d = ACC + col3 * v.w
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
        float r[4];
        for (int c = 0; c < 4; ++c)
            r[c] = madd(vu.acc[c], a[c], b);
        if (d != 0)
            std::memcpy(vu.vf[d], r, 16);
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    mula(vu, vf[1], vf[23][0]);             // 0x4b8 MULAx.xyzw ACC, vf1, vf23x
    vi[3] = vi16(vi[3] + 1);                //       IADDIU vi3, vi3, 1
    madda(vu, vf[2], vf[23][1]);            // 0x4c0 MADDAy.xyzw ACC, vf2, vf23y
    lq(vu, 21, vi[3] + 199);                //       LQ.xyzw vf21, 199(vi3)
    madda(vu, vf[3], vf[23][2]);            // 0x4c8 MADDAz.xyzw ACC, vf3, vf23z
    vi[6] = vi16(vi[6] + 12);               //       IADDIU vi6, vi6, 12
    maddv(vu, 9, vf[4], vf[0][3]);          // 0x4d0 MADDw.xyzw vf9, vf4, vf0w
    mula(vu, vf[5], vf[23][0]);             // 0x4d8 MULAx.xyzw ACC, vf5, vf23x
    vi[10] = vi16(vi[10] - 1);              //       IADDI vi10, vi10, -1
    madda(vu, vf[6], vf[23][1]);            // 0x4e0 MADDAy.xyzw ACC, vf6, vf23y
    madda(vu, vf[7], vf[23][2]);            // 0x4e8 MADDAz.xyzw ACC, vf7, vf23z
    maddv(vu, 13, vf[8], vf[0][3]);         // 0x4f0 MADDw.xyzw vf13, vf8, vf0w
    const float q = vdiv(vf[0][3], vf[9][3]); //     DIV Q, vf0w, vf9w (lands by 0x530)
    vu.acc[3] = mul(vf[0][3], vf[31][2]);   // 0x4f8 MULAz.w ACC, vf0, vf31z (= 1 * vf31z: the w lane of vf0)
    sq(vu, 21, vi[6] - 11);                 //       SQ.xyzw vf21, -11(vi6)
    vf[14][3] = madd(vu.acc[3], vf[31][3], vf[9][3]); // 0x500 MADDw.w vf14, vf31, vf9w
    sq(vu, 21, 914);                        //       SQ.xyzw vf21, 914(vi0)
    mula(vu, vf[1], vf[24][0]);             // 0x508 MULAx.xyzw ACC, vf1, vf24x
    {                                       //       ILW.y vi15, 17(vi0)
        uint32_t w;
        std::memcpy(&w, vu.mem + 16 * 17 + 4, 4);
        vi[15] = vi16(int32_t(w & 0xFFFF));
    }
    madda(vu, vf[2], vf[24][1]);            // 0x510 MADDAy.xyzw ACC, vf2, vf24y
    sq(vu, 13, 915);                        //       SQ.xyzw vf13, 915(vi0)
    madda(vu, vf[3], vf[24][2]);            // 0x518 MADDAz.xyzw ACC, vf3, vf24z
    sq(vu, 13, 923);                        //       SQ.xyzw vf13, 923(vi0)
    vf[14][3] = vmax(vf[14][3], vf[31][1]); // 0x520 MAXy.w vf14, vf14, vf31y
    sq(vu, 18, 916);                        //       SQ.xyzw vf18, 916(vi0)
    maddv(vu, 10, vf[4], vf[0][3]);         // 0x528 MADDw.xyzw vf10, vf4, vf0w
    sq(vu, 18, 924);                        //       SQ.xyzw vf18, 924(vi0)
    vu.q = q;
    for (int c = 0; c < 3; ++c)             // 0x530 MULq.xyz vf14, vf9, Q (the DIV at 0x4f0)
        vf[14][c] = mul(vf[9][c], vu.q);
    sq(vu, 21, vi[6] - 8);                  //       SQ.xyzw vf21, -8(vi6)
    vu.pc = 0x538;
}
