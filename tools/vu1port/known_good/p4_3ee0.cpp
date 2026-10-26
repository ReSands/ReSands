// Known-good answer (by hand, 26 Oct; Qwen failed every case in 9 rounds): P4 stage "3ee0", a point light:
// squared distance vf14.w, Q = 1 / distance (RSQRT, landed at the exit), the light's colour vf8 scaled by
// N.L (vf15.x, clamped at 0), the falloff terms in vf31.y / .w clamped to [0, 1], dot products vf15.x (vf7)
// and ACC.y (vf26) with vf25 begun, and the next light's loads.
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

namespace
{
    // RSQRT Q, a, b as the reference lands it: a / sqrt(|b|); a zero root -> the largest float, sign of a
    float vrsqrt(float a, float b)
    {
        a = norm(a);
        const float den = std::sqrt(std::fabs(norm(b)));
        if (den == 0.0f)
            return std::signbit(a) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        return norm(a / den);
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    float *acc = vu.acc;
    acc[3] = mul(vf[0][3], vf[13][0]);      // 0x3ee0 MULAx.w ACC, vf0, vf13x
    {                                       //        MTIR vi3, vf6w
        uint32_t b;
        std::memcpy(&b, &vf[6][3], 4);
        vi[3] = vi16(int32_t(b & 0xFFFF));
    }
    acc[3] = madd(acc[3], vf[0][3], vf[13][1]); // 0x3ee8 MADDAy.w ACC, vf0, vf13y
    vf[14][3] = madd(acc[3], vf[0][3], vf[13][2]); // 0x3ef0 MADDz.w vf14, vf0, vf13z
    vf[15][0] = vmax(vf[15][0], vf[0][0]);  // 0x3ef8 MAXx.x vf15, vf15, vf0x
    vf[31][1] = vmax(vf[31][1], vf[0][0]);  // 0x3f00 MAXx.yw vf31, vf31, vf0x
    vf[31][3] = vmax(vf[31][3], vf[0][0]);
    vi[3] = vi16(vi[3] & vi[7]);            //        IAND vi3, vi3, vi7
    for (int c = 0; c < 3; ++c)             // 0x3f08 MUL.xyz vf7, vf14, vf21
        vf[7][c] = mul(vf[14][c], vf[21][c]);
    for (int c = 0; c < 3; ++c)             // 0x3f10 MUL.xyz vf26, vf14, vf5
        vf[26][c] = mul(vf[14][c], vf[5][c]);
    vu.q = vrsqrt(vf[0][3], vf[14][3]);     //        RSQRT Q, vf0w, vf14w
    {
        const float s = vf[15][0];          // 0x3f18 MULx.xyzw vf13, vf8, vf15x
        for (int c = 0; c < 4; ++c)
            vf[13][c] = mul(vf[8][c], s);
    }
    vf[31][1] = vmin(vf[31][1], vf[0][3]);  // 0x3f20 MINIw.yw vf31, vf31, vf0w
    vf[31][3] = vmin(vf[31][3], vf[0][3]);
    acc[0] = mul(vf[25][0], vf[7][0]);      // 0x3f28 MULAx.x ACC, vf25, vf7x
    acc[0] = madd(acc[0], vf[25][0], vf[7][1]); // 0x3f30 MADDAy.x ACC, vf25, vf7y
    vf[15][0] = madd(acc[0], vf[25][0], vf[7][2]); // 0x3f38 MADDz.x vf15, vf25, vf7z
    lq(vu, 6, vi[8] + 1007);                //        LQ.xyzw vf6, 1007(vi8)
    vf[31][1] = mul(vf[31][1], vf[31][3]);  // 0x3f40 MULw.y vf31, vf31, vf31w
    lq(vu, 5, vi[8] + 1011);                //        LQ.xyzw vf5, 1011(vi8)
    acc[1] = mul(vf[25][1], vf[26][0]);     // 0x3f48 MULAx.y ACC, vf25, vf26x
    lq(vu, 7, vi[8] + 20);                  //        LQ.xyzw vf7, 20(vi8)
    acc[1] = madd(acc[1], vf[25][1], vf[26][1]); // 0x3f50 MADDAy.y ACC, vf25, vf26y
    lq(vu, 8, vi[8] + 16);                  //        LQ.xyzw vf8, 16(vi8)
    vu.pc = 0x3f58;
}
