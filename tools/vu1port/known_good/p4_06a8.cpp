// Known-good answer (by hand, 26 Oct; Qwen failed every case in 9 rounds): P4 stage "06a8", the first vertex of
// the single-bone path: position vf20 (w = 1) and normal vf21 (ITOF4, * 1/8) through the bone (vf1-4 to clip
// space vf24, vf5-8 to vf18) and the light matrix (vf13-15 -> vf26), 1 / w (DIV, landed at the exit), the
// vertex count vi10 and the packet pointers vi2, vi6.
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
    lq(vu, 21, vi[1] + 129);                // 0x06a8 LQ.xyzw vf21, 129(vi1)
    lq(vu, 20, vi[1] + 1, 0xE);             // 0x06b0 LQ.xyz vf20, 1(vi1)
    for (int c = 0; c < 3; ++c)             // 0x06b8 ITOF4.xyz vf21, vf21
    {
        int32_t v;
        std::memcpy(&v, &vf[21][c], 4);
        vf[21][c] = float(v) / 16.0f;
    }
    vu.i = 0.125f;                          //        LOI 0x3e000000
    vf[20][3] = add(vf[0][3], vf[0][0]);    // 0x06c0 ADDx.w vf20, vf0, vf0x
    vi[11] = 32767;                         //        IADDIU vi11, vi0, 32767
    mula(vu, vf[1], vf[20][0]);             // 0x06c8 MULAx.xyzw ACC, vf1, vf20x
    vi[10] = ilw(vu, vi[1], 0);             //        ILWR.x vi10, (vi1)
    for (int c = 0; c < 3; ++c)             // 0x06d0 MULi.xyz vf21, vf21, I
        vf[21][c] = mul(vf[21][c], vu.i);
    lq(vu, 23, vi[1]);                      //        LQ.xyzw vf23, 0(vi1)
    madda(vu, vf[2], vf[20][1]);            // 0x06d8 MADDAy.xyzw ACC, vf2, vf20y
    lq(vu, 29, 29);                         //        LQ.xyzw vf29, 29(vi0)
    madda(vu, vf[3], vf[20][2]);            // 0x06e0 MADDAz.xyzw ACC, vf3, vf20z
    vi[3] = vi16(vi[1] + 1);                //        IADDIU vi3, vi1, 1
    maddv(vu, 24, vf[4], vf[0][3]);         // 0x06e8 MADDw.xyzw vf24, vf4, vf0w
    vi[1] = vi16(vi[1] + 1);                //        IADDIU vi1, vi1, 1
    mula(vu, vf[13], vf[21][0]);            // 0x06f0 MULAx.xyzw ACC, vf13, vf21x
    vi[10] = vi16(vi[10] & vi[11]);         //        IAND vi10, vi10, vi11
    madda(vu, vf[14], vf[21][1]);           // 0x06f8 MADDAy.xyzw ACC, vf14, vf21y
    vi[2] = vi16(vi[1] + 194);              //        IADDIU vi2, vi1, 194
    maddv(vu, 26, vf[15], vf[21][2]);       // 0x0700 MADDz.xyzw vf26, vf15, vf21z
    vi[6] = vi16(vi[2] + 1);                //        IADDIU vi6, vi2, 1
    mula(vu, vf[5], vf[20][0]);             // 0x0708 MULAx.xyzw ACC, vf5, vf20x
    vu.q = vdiv(vf[0][3], vf[24][3]);       //        DIV Q, vf0w, vf24w
    madda(vu, vf[6], vf[20][1]);            // 0x0710 MADDAy.xyzw ACC, vf6, vf20y
    sq(vu, vf[23], vi[2]);                  //        SQ.xyzw vf23, 0(vi2)
    madda(vu, vf[7], vf[20][2]);            // 0x0718 MADDAz.xyzw ACC, vf7, vf20z
    lq(vu, 17, vi[3] + 64);                 //        LQ.xyzw vf17, 64(vi3)
    maddv(vu, 18, vf[8], vf[0][3]);         // 0x0720 MADDw.xyzw vf18, vf8, vf0w
    lq(vu, 31, 24);                         //        LQ.xyzw vf31, 24(vi0)
    vu.pc = 0x728;
}
