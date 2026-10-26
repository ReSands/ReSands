// Known-good answer (by hand, 26 Oct): P5 stage "1700", the loop body after a particle: its size clamped into
// vf25.w and converted (FTOI4 into vf26), the next vertex (vf20 through vf17-vf19, vf22 into vf29, 1 / w into Q),
// the previous one scaled by the old Q (vf28), a qword copied from vi8 to the output at vi7.
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

namespace
{
    int32_t ftoi(float v, double scale)
    {
        const double d = double(norm(v)) * scale;
        if (d >= 2147483647.0)
            return INT32_MAX;
        if (d <= -2147483648.0)
            return INT32_MIN;
        return int32_t(d);
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    vf[25][3] = madd(vu.acc[3], vf[31][3], vf[29][3]); // 0x1700 MADDw.w vf25, vf31, vf29w
    vi[3] = vi16(vi[3] + 2);                //        IADDIU vi3, vi3, 2
    mula(vu, vf[17], vf[20][0]);            // 0x1708 MULAx.xyzw ACC, vf17, vf20x
    madda(vu, vf[18], vf[20][1]);           // 0x1710 MADDAy.xyzw ACC, vf18, vf20y
    vi[7] = vi16(vi[7] + 3);                //        IADDIU vi7, vi7, 3
    madda(vu, vf[19], vf[20][2]);           // 0x1718 MADDAz.xyzw ACC, vf19, vf20z
    lq(vu, 27, vi[8]);                      //        LQ.xyzw vf27, 0(vi8)
    maddv(vu, 29, vf[22], vf[20][3]);       // 0x1720 MADDw.xyzw vf29, vf22, vf20w
    lq(vu, 21, vi[3] - 1);                  //        LQ.xyzw vf21, -1(vi3)
    vf[25][3] = vmin(vf[25][3], vf[31][0]); // 0x1728 MINIx.w vf25, vf25, vf31x
    sq(vu, 27, vi[7] - 2);                  // 0x1738 SQ.xyzw vf27, -2(vi7)
    for (int c = 0; c < 3; ++c)             // 0x1740 MULq.xyz vf28, vf21, Q (the DIV before this pair)
        vf[28][c] = mul(vf[21][c], vu.q);
    vu.q = vdiv(vf[0][3], vf[29][3]);       //        DIV Q, vf0w, vf29w
    vf[25][3] = vmax(vf[25][3], vf[31][1]); // 0x1748 MAXy.w vf25, vf25, vf31y
    lq(vu, 20, vi[3] + 2);                  //        LQ.xyzw vf20, 2(vi3)
    for (int c = 0; c < 4; ++c)             // 0x1768 FTOI4.xyzw vf26, vf25
    {
        const int32_t v = ftoi(vf[25][c], 16.0);
        std::memcpy(&vf[26][c], &v, 4);
    }
    vi[10] = vi16(vi[10] - 1);              //        IADDI vi10, vi10, -1
    vu.acc[3] = mul(vf[0][3], vf[31][2]);   // 0x1770 MULAz.w ACC, vf0, vf31z (= 1 * vf31z: the w lane of vf0)
    vu.pc = 0x1780;
}
