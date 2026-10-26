// Known-good answer (by hand, 26 Oct; Qwen failed 427-602 of 602 cases in 9 rounds): P4 stage "3dc8", the
// directional-light loop of the lit vertex: per light (vi8 up to vi7) N.L = vf5 * vf21 summed into vf26.w,
// clamped at 0 (vf25.w), colour vf28 += light colour (16(vi8)) * N.L; then vf28 capped at 255, and either on
// to the point lights (vf14 = point position - vertex, its squared length in vf14.w) or out to 0x3f88.
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
    float *acc = vu.acc;
light:
    for (int c = 0; c < 3; ++c)             // 0x3dc8 MUL.xyz vf26, vf5, vf21
        vf[26][c] = mul(vf[5][c], vf[21][c]);
    lq(vu, 8, vi[8] + 16);                  //        LQ.xyzw vf8, 16(vi8)
    for (int c = 0; c < 3; ++c)             // 0x3dd0 SUB.xyz vf14, vf6, vf16
        vf[14][c] = sub(vf[6][c], vf[16][c]);
    vu.i = 255.0f;                          //        LOI 0x437f0000
    vi[8] = vi16(vi[8] + 1);                // 0x3dd8 IADDIU vi8, vi8, 1
    vf[25][3] = vmax(vf[26][3], vf[0][0]);  // 0x3de0 MAXx.w vf25, vf26, vf0x
    acc[3] = mul(vf[0][3], vf[26][0]);      // 0x3de8 MULAx.w ACC, vf0, vf26x
    acc[3] = madd(acc[3], vf[0][3], vf[26][1]); // 0x3df0 MADDAy.w ACC, vf0, vf26y
    lq(vu, 5, vi[8] + 1011);                //        LQ.xyzw vf5, 1011(vi8)
    vf[26][3] = madd(acc[3], vf[0][3], vf[26][2]); // 0x3df8 MADDz.w vf26, vf0, vf26z
    lq(vu, 6, vi[8] + 1007);                //        LQ.xyzw vf6, 1007(vi8)
    for (int c = 0; c < 3; ++c)             // 0x3e00 MULAw.xyz ACC, vf28, vf0w
        acc[c] = mul(vf[28][c], vf[0][3]);
    {
        const bool again = vi[8] != vi[7];  //        IBNE vi8, vi7, 0x3dc8
        for (int c = 0; c < 3; ++c)         // 0x3e08 MADDw.xyz vf28, vf8, vf25w (delay slot)
            vf[28][c] = madd(acc[c], vf[8][c], vf[25][3]);
        if (again)
            goto light;
    }
    for (int c = 0; c < 4; ++c)             // 0x3e10 MINIi.xyzw vf28, vf28, I
        vf[28][c] = vmin(vf[28][c], vu.i);
    {
        const bool done = vi[8] == vi[12];  //        IBEQ vi8, vi12, 0x3f88
        for (int c = 0; c < 3; ++c)         // 0x3e18 MUL.xyz vf13, vf14, vf14 (delay slot)
            vf[13][c] = mul(vf[14][c], vf[14][c]);
        lq(vu, 31, 24);                     //        LQ.xyzw vf31, 24(vi0)
        if (done)
        {
            vu.pc = 0x3f88;
            return;
        }
    }
    for (int c = 0; c < 3; ++c)             // 0x3e20 MUL.xyz vf15, vf21, vf14
        vf[15][c] = mul(vf[21][c], vf[14][c]);
    for (int c = 0; c < 3; ++c)             // 0x3e28 ADDw.xyz vf25, vf0, vf0w
        vf[25][c] = add(vf[0][c], vf[0][3]);
    acc[3] = mul(vf[0][3], vf[13][0]);      // 0x3e30 MULAx.w ACC, vf0, vf13x
    acc[3] = madd(acc[3], vf[0][3], vf[13][1]); // 0x3e38 MADDAy.w ACC, vf0, vf13y
    vf[14][3] = madd(acc[3], vf[0][3], vf[13][2]); // 0x3e40 MADDz.w vf14, vf0, vf13z
    lq(vu, 5, vi[8] + 1010);                //        LQ.xyzw vf5, 1010(vi8)
    vu.pc = 0x3e48;
}
