// Known-good answer (by hand, 26 Oct; Qwen failed every case in 9 rounds): P4 stage "1598", the end of a
// vertex's transform: vf17.xy, Q = 1 / w (the DIV lands before the MULq that reads it), the fog value vf19.w
// clamped below by vf31.y, the next vertex's normal transform started, the screen position vf19.xyz = vf24 * Q.
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

namespace
{
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
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    madda(vu, vf[11], vf[21][2]);           // 0x1598 MADDAz.xyzw ACC, vf11, vf21z
    maddv(vu, 17, vf[12], vf[0][3], 0xC);   // 0x15a0 MADDw.xy vf17, vf12, vf0w
    vu.q = vdiv(vf[0][3], vf[24][3]);       //        DIV Q, vf0w, vf24w
    vu.acc[3] = mul(vf[0][3], vf[31][2]);   // 0x15a8 MULAz.w ACC, vf0, vf31z
    vf[19][3] = madd(vu.acc[3], vf[31][3], vf[24][3]); // 0x15b0 MADDw.w vf19, vf31, vf24w
    vi[2] = vi16(vi[1] + 194);              //        IADDIU vi2, vi1, 194
    mula(vu, vf[5], vf[20][0]);             // 0x15b8 MULAx.xyzw ACC, vf5, vf20x
    vi[6] = vi16(vi[2] + 1);                //        IADDIU vi6, vi2, 1
    madda(vu, vf[6], vf[20][1]);            // 0x15c0 MADDAy.xyzw ACC, vf6, vf20y
    sq(vu, vf[23], vi[2]);                  //        SQ.xyzw vf23, 0(vi2)
    vf[19][3] = vmax(vf[19][3], vf[31][1]); // 0x15c8 MAXy.w vf19, vf19, vf31y
    madda(vu, vf[7], vf[20][2]);            // 0x15d0 MADDAz.xyzw ACC, vf7, vf20z
    lq(vu, 21, vi[3] + 65);                 //        LQ.xyzw vf21, 65(vi3)
    maddv(vu, 18, vf[8], vf[0][3]);         // 0x15d8 MADDw.xyzw vf18, vf8, vf0w
    lq(vu, 20, vi[3] + 1, 0xE);             //        LQ.xyz vf20, 1(vi3)
    vf[17][2] = add(vf[0][2], vf[0][3]);    // 0x15e0 ADDw.z vf17, vf0, vf0w
    for (int c = 0; c < 3; ++c)             // 0x15e8 MULq.xyz vf19, vf24, Q
        vf[19][c] = mul(vf[24][c], vu.q);
    vu.pc = 0x15f0;
}
