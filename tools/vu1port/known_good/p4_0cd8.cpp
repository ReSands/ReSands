// Known-good answer (by hand, 26 Oct; Qwen's port passed its 600 sampled cases but not whole runs: no exact
// product-sum and no denormal flush of Q): P4 stage "0cd8", the single-bone loop's store step: the previous
// vertex's screen position (FTOI4) and colour (FTOI0, capped by vf27.z) stored, the clip test (vi1), this
// vertex's transform (vf24) and light matrix (vf26), the next 1 / w (DIV, landed at the exit) and loads.
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
    vf[19][3] = vmin(vf[19][3], vf[31][0]); // 0x0cd8 MINIx.w vf19, vf19, vf31x
    std::memcpy(vf[29], vf[18], 16);        //        MOVE.xyzw vf29, vf18
    vu.clip = ((vu.clip << 6) | clipFlags(vf[18], vf[18][3])) & 0xFFFFFF; // 0x0ce0 CLIPw.xyz vf18, vf18
    vi[10] = vi16(vi[10] - 1);              //        IADDI vi10, vi10, -1
    mula(vu, vf[1], vf[16][0]);             // 0x0ce8 MULAx.xyzw ACC, vf1, vf16x
    lq(vu, 5, 1016);                        //        LQ.xyzw vf5, 1016(vi0)
    madda(vu, vf[2], vf[16][1]);            // 0x0cf0 MADDAy.xyzw ACC, vf2, vf16y
    lq(vu, 6, 1017);                        //        LQ.xyzw vf6, 1017(vi0)
    madda(vu, vf[3], vf[16][2]);            // 0x0cf8 MADDAz.xyzw ACC, vf3, vf16z
    lq(vu, 7, 1018);                        //        LQ.xyzw vf7, 1018(vi0)
    maddv(vu, 24, vf[4], vf[0][3]);         // 0x0d00 MADDw.xyzw vf24, vf4, vf0w
    lq(vu, 8, 1019);                        //        LQ.xyzw vf8, 1019(vi0)
    for (int c = 0; c < 4; ++c)             // 0x0d08 FTOI4.xyzw vf19, vf19
    {
        const int32_t v = ftoi(vf[19][c], 16.0);
        std::memcpy(&vf[19][c], &v, 4);
    }
    vi[1] = (vu.clip & 0x03ffff) != 0;      //        FCAND vi1, 0x03ffff (the CLIP at 0xce0 has landed)
    for (int c = 0; c < 3; ++c)             // 0x0d10 MINIz.xyz vf28, vf28, vf27z
        vf[28][c] = vmin(vf[28][c], vf[27][2]);
    vi[8] = vi[5];                          //        IADDIU vi8, vi5, 0
    mula(vu, vf[13], vf[21][0]);            // 0x0d18 MULAx.xyzw ACC, vf13, vf21x
    vi[3] = vi16(vi[3] + 1);                //        IADDIU vi3, vi3, 1
    madda(vu, vf[14], vf[21][1]);           // 0x0d20 MADDAy.xyzw ACC, vf14, vf21y
    vi[6] = vi16(vi[6] + 3);                //        IADDIU vi6, vi6, 3
    for (int c = 0; c < 3; ++c)             // 0x0d28 MULq.xyz vf30, vf30, Q (Q from the DIV before the entry)
        vf[30][c] = mul(vf[30][c], vu.q);
    {                                       //        MTIR vi15, vf17z
        uint32_t b;
        std::memcpy(&b, &vf[17][2], 4);
        vi[15] = vi16(int32_t(b & 0xFFFF));
    }
    for (int c = 0; c < 4; ++c)             // 0x0d30 FTOI0.xyzw vf28, vf28
    {
        const int32_t v = ftoi(vf[28][c], 1.0);
        std::memcpy(&vf[28][c], &v, 4);
    }
    sq(vu, vf[19], vi[6] - 1);              //        SQ.xyzw vf19, -1(vi6)
    maddv(vu, 26, vf[15], vf[21][2]);       // 0x0d38 MADDz.xyzw vf26, vf15, vf21z
    vu.q = vdiv(vf[0][3], vf[24][3]);       //        DIV Q, vf0w, vf24w
    mula(vu, vf[5], vf[16][0]);             // 0x0d40 MULAx.xyzw ACC, vf5, vf16x
    lq(vu, 20, vi[3] + 81, 0xE);            //        LQ.xyz vf20, 81(vi3)
    madda(vu, vf[6], vf[16][1]);            // 0x0d48 MADDAy.xyzw ACC, vf6, vf16y
    lq(vu, 17, vi[3] + 120);                //        LQ.xyzw vf17, 120(vi3)
    madda(vu, vf[7], vf[16][2]);            // 0x0d50 MADDAz.xyzw ACC, vf7, vf16z
    sq(vu, vf[30], vi[6] - 3);              //        SQ.xyzw vf30, -3(vi6)
    vu.pc = 0xd58;
}
