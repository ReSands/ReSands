// Known-good answer (by hand, 25 Oct; Qwen missed it in 6 rounds): P9 stage "c_b", the middle of the curve
// point subroutine 0xae0: vf11-vf13 .y reduced by vf14-vf16 .y (as integers, times I) and by their bit 1 part,
// squared to s, then spread to (s, s^2, s^3, s^3); vf14.xyz = the three (y + 1) & 2 bits as floats, minus 1.
#include "vu_task.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace
{
    // VU operands: denormals read as zero, Inf/NaN as the largest float (sign kept); results flushed the same way
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
    float add(float a, float b) { return norm(norm(a) + norm(b)); }
    float sub(float a, float b) { return norm(norm(a) - norm(b)); }
    float mul(float a, float b) { return norm(norm(a) * norm(b)); }
    float madd(float acc, float a, float b) { return norm(norm(acc) + mul(a, b)); }
    float msub(float acc, float a, float b) { return norm(norm(acc) - mul(a, b)); }
    float vmax(float a, float b) { return norm(a) > norm(b) ? norm(a) : norm(b); }
    float vmin(float a, float b) { return norm(a) < norm(b) ? norm(a) : norm(b); }
    int32_t ftoi0(float v)
    {
        const double d = double(norm(v));
        if (d >= 2147483647.0)
            return INT32_MAX;
        if (d <= -2147483648.0)
            return INT32_MIN;
        return int32_t(d);
    }
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }
    void lq(VuState &vu, int t, int32_t qword)
    {
        std::memcpy(vu.vf[t], vu.mem + 16 * (uint32_t(qword) & 0x3FF), 16);
    }
    void isw(VuState &vu, int32_t value, int32_t qword, int lane)
    {
        const uint32_t v = uint32_t(value) & 0xFFFF;
        std::memcpy(vu.mem + 16 * (uint32_t(qword) & 0x3FF) + 4 * lane, &v, 4);
    }
    // MAC flags of one written lane (zero x = 8 .. w = 1, sign x = 0x80 .. w = 0x10)
    uint32_t macLane(float r, int c)
    {
        uint32_t b;
        std::memcpy(&b, &r, 4);
        return ((b & 0x7FFFFFFFu) == 0 ? 8u >> c : 0u) | ((b & 0x80000000u) ? 0x80u >> c : 0u);
    }
}

namespace
{
    int32_t bitsOf(float f)
    {
        int32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
    float itof0(float lane) { return norm(float(bitsOf(lane))); }
    void mfir(float &lane, int32_t v) { std::memcpy(&lane, &v, 4); }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    // 0xc00-0xc10 ITOF0.y vf17..19, vf14..16 | MTIR vi4..6, vf14..16y
    for (int k = 0; k < 3; ++k)
    {
        vf[17 + k][1] = itof0(vf[14 + k][1]);
        vi[4 + k] = vi16(bitsOf(vf[14 + k][1]));
    }
    // 0xc18-0xc28 MULi.y vf17..19, I | IAND vi7..9, vi4..6, vi2
    for (int k = 0; k < 3; ++k)
    {
        vf[17 + k][1] = mul(vf[17 + k][1], vu.i);
        vi[7 + k] = vi[4 + k] & vi[2];
    }
    // 0xc30-0xc40 SUB.y vf11..13, vf11..13, vf17..19 | MFIR.y vf14..16, vi7..9
    for (int k = 0; k < 3; ++k)
    {
        vf[11 + k][1] = sub(vf[11 + k][1], vf[17 + k][1]);
        mfir(vf[14 + k][1], vi[7 + k]);
    }
    // 0xc48-0xc58 ITOF0.y vf14..16 | IADDIU vi4..6, 1
    for (int k = 0; k < 3; ++k)
    {
        vf[14 + k][1] = itof0(vf[14 + k][1]);
        vi[4 + k] = vi16(vi[4 + k] + 1);
    }
    vi[2] = 2;                                                    // 0xc60 IADDIU vi2, vi0, 2
    for (int k = 0; k < 3; ++k)                                   // 0xc60-0xc70 MULi.y vf14..16, I
        vf[14 + k][1] = mul(vf[14 + k][1], vu.i);
    // 0xc78-0xc88 SUB.y vf11..13, vf14..16 | IAND vi4..6, vi2
    for (int k = 0; k < 3; ++k)
    {
        vf[11 + k][1] = sub(vf[11 + k][1], vf[14 + k][1]);
        vi[4 + k] = vi[4 + k] & vi[2];
    }
    for (int c = 0; c < 4; ++c)                                   // 0xc90 SUB.xyzw vf31, vf31, vf31
        vf[31][c] = sub(vf[31][c], vf[31][c]);
    vu.i = 0.5f;                                                  //       LOI 0.5
    // 0xc98-0xca8 MUL.y vf11..13 squared | MFIR.x/y/z vf14, vi4..6
    for (int k = 0; k < 3; ++k)
    {
        vf[11 + k][1] = mul(vf[11 + k][1], vf[11 + k][1]);
        mfir(vf[14][k], vi[4 + k]);
    }
    for (int k = 0; k < 3; ++k)                                   // 0xcb0-0xcc0 ADDy.xzw vfN, vf31, vfNy
    {
        const float y = vf[11 + k][1];
        for (int c : {0, 2, 3})
            vf[11 + k][c] = add(vf[31][c], y);
    }
    for (int c = 0; c < 3; ++c)                                   // 0xcc8 ITOF0.xyz vf14, vf14
        vf[14][c] = itof0(vf[14][c]);
    for (int k = 0; k < 3; ++k)                                   // 0xcd0-0xce0 MULx.yzw vfN, vfN, vfNx
        for (int c = 1; c < 4; ++c)
            vf[11 + k][c] = mul(vf[11 + k][c], vf[11 + k][0]);
    for (int c = 0; c < 3; ++c)                                   // 0xce8 SUBw.xyz vf14, vf14, vf0w
        vf[14][c] = sub(vf[14][c], 1.0f);
    for (int k = 0; k < 3; ++k)                                   // 0xcf0-0xd00 MULx.zw vfN, vfN, vfNx
        for (int c = 2; c < 4; ++c)
            vf[11 + k][c] = mul(vf[11 + k][c], vf[11 + k][0]);
    vu.pc = 0xd08;
}
