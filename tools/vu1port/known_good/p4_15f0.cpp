// Known-good answer (by hand, 26 Oct; Qwen failed every case in 9 rounds): P4 stage "15f0", the middle of the
// single-bone vertex loop: the previous vertex's screen position (FTOI4) and colour stored for the GS packet,
// this vertex's transform (vf24 clip space, vf18 for the clip test), the next 1 / w (DIV, landed at the exit),
// and vi1 = the vertex's ADC bit (vi15, word 63 .w) + whether it lies outside the last three clip tests.
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
    int32_t ftoi(float v, double scale)
    {
        const double d = double(norm(v)) * scale;
        if (d >= 2147483647.0)
            return INT32_MAX;
        if (d <= -2147483648.0)
            return INT32_MIN;
        return int32_t(d);
    }
    float itof(float bits, float scale)
    {
        int32_t v;
        std::memcpy(&v, &bits, 4);
        return float(v) / scale;
    }
    // CLIPw.xyz: compares the bit patterns against |w| (w's exponent 0 -> 0x7fffff), as the reference does
    uint32_t clipFlags(const float *s, float w)
    {
        uint32_t wb;
        std::memcpy(&wb, &w, 4);
        const int32_t limit = (wb & 0x7F800000u) ? int32_t(wb & 0x7FFFFFFFu) : 0x007FFFFF;
        uint32_t flags = 0;
        for (int c = 0; c < 3; ++c)
        {
            uint32_t b;
            std::memcpy(&b, &s[c], 4);
            if (int32_t(b) > limit)
                flags |= 1u << (2 * c);
            if (int32_t(b ^ 0x80000000u) > limit)
                flags |= 2u << (2 * c);
        }
        return flags;
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    for (int c = 0; c < 3; ++c)             // 0x15f0 ITOF4.xyz vf21, vf21
        vf[21][c] = itof(vf[21][c], 16.0f);
    std::memcpy(vf[16], vf[25], 16);        //        MOVE.xyzw vf16, vf25
    for (int c = 0; c < 3; ++c)             // 0x15f8 MULq.xyz vf30, vf17, Q (Q from the DIV before the entry)
        vf[30][c] = mul(vf[17][c], vu.q);
    vi[3] = vi16(vi[3] + 1);                //        IADDIU vi3, vi3, 1
    vf[19][3] = vmin(vf[19][3], vf[31][0]); // 0x1600 MINIx.w vf19, vf19, vf31x
    vi[6] = vi16(vi[6] + 3);                //        IADDIU vi6, vi6, 3
    mula(vu, vf[1], vf[20][0]);             // 0x1608 MULAx.xyzw ACC, vf1, vf20x
    vi[15] = ilw(vu, vi[3] + 63, 3);        //        ILW.w vi15, 63(vi3)
    madda(vu, vf[2], vf[20][1]);            // 0x1610 MADDAy.xyzw ACC, vf2, vf20y
    std::memcpy(vf[25], vf[27], 16);        //        MOVE.xyzw vf25, vf27
    for (int c = 0; c < 3; ++c)             // 0x1618 MULi.xyz vf21, vf21, I
        vf[21][c] = mul(vf[21][c], vu.i);
    lq(vu, 28, vi[3] + 127);                //        LQ.xyzw vf28, 127(vi3)
    sq(vu, vf[17], vi[3] + 127);            // 0x1620 SQ.xyzw vf17, 127(vi3) (reads vf17, not written by the FTOI4)
    for (int c = 0; c < 4; ++c)             //        FTOI4.xyzw vf19, vf19
    {
        const int32_t v = ftoi(vf[19][c], 16.0);
        std::memcpy(&vf[19][c], &v, 4);
    }
    madda(vu, vf[3], vf[20][2]);            // 0x1628 MADDAz.xyzw ACC, vf3, vf20z
    maddv(vu, 24, vf[4], vf[0][3]);         // 0x1630 MADDw.xyzw vf24, vf4, vf0w
    vu.clip = ((vu.clip << 6) | clipFlags(vf[18], vf[18][3])) & 0xFFFFFF; // 0x1638 CLIPw.xyz vf18, vf18
    std::memcpy(vf[27], vf[18], 16);        //        MOVE.xyzw vf27, vf18
    mula(vu, vf[9], vf[21][0]);             // 0x1640 MULAx.xyzw ACC, vf9, vf21x
    {                                       //        MTIR vi12, vf19w
        uint32_t b;
        std::memcpy(&b, &vf[19][3], 4);
        vi[12] = vi16(int32_t(b & 0xFFFF));
    }
    madda(vu, vf[10], vf[21][1]);           // 0x1648 MADDAy.xyzw ACC, vf10, vf21y
    sq(vu, vf[19], vi[6] - 1);              //        SQ.xyzw vf19, -1(vi6)
    madda(vu, vf[11], vf[21][2]);           // 0x1650 MADDAz.xyzw ACC, vf11, vf21z
    vu.q = vdiv(vf[0][3], vf[24][3]);       //        DIV Q, vf0w, vf24w
    maddv(vu, 17, vf[12], vf[0][3], 0xC);   // 0x1658 MADDw.xy vf17, vf12, vf0w
    vi[1] = (vu.clip & 0x03ffff) != 0;      //        FCAND vi1, 0x03ffff (the CLIP at 0x1638 has landed)
    vu.acc[3] = mul(vf[0][3], vf[31][2]);   // 0x1660 MULAz.w ACC, vf0, vf31z
    sq(vu, vf[28], vi[6] - 2);              //        SQ.xyzw vf28, -2(vi6)
    vf[19][3] = madd(vu.acc[3], vf[31][3], vf[24][3]); // 0x1668 MADDw.w vf19, vf31, vf24w
    vi[1] = vi16(vi[1] + vi[15]);           //        IADD vi1, vi1, vi15
    vu.pc = 0x1670;
}
