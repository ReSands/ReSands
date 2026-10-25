// Known-good answer (by hand, 25 Oct; Qwen missed it in 6 rounds): P9 stage "f_a", the strip set-up of the GS
// strip subroutine 0xf50: the curve segment's two ends to clip space (vf25, vf26), their 1/w (DIV in flight
// at the exit, landed by the snapshot), the strip width vf9.w, the near-plane test (FMAND on SUBi.w's sign).
#include "vu_task.h"

#include <cstdint>
#include <cstring>

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

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    float *acc = vu.acc;
    vf[18][2] = add(0.0f, 1.0f);                                  // 0xf50 ADDw.z vf18, vf0, vf0w
    lq(vu, 29, 13);                                               //       LQ vf29, 13(vi0)
    for (int c = 0; c < 3; ++c)                                   // 0xf58 ADDw.xyz vf19, vf0, vf0w
        vf[19][c] = add(0.0f, 1.0f);
    lq(vu, 22, vi[3]);                                            //       LQ vf22, 0(vi3)
    vf[18][0] = add(0.0f, 0.0f);                                  // 0xf60 ADD.xy vf18, vf0, vf0
    vf[18][1] = add(0.0f, 0.0f);
    lq(vu, 30, 14);                                               //       LQ vf30, 14(vi0)
    vf[18][3] = add(1.0f, 0.0f);                                  // 0xf68 ADDx.w vf18, vf0, vf0x
    lq(vu, 23, vi[3] + 1);                                        //       LQ vf23, 1(vi3)
    vf[19][3] = add(1.0f, 0.0f);                                  // 0xf70 ADDx.w vf19, vf0, vf0x
    lq(vu, 31, 15);                                               //       LQ vf31, 15(vi0)
    lq(vu, 21, vi[1] + 7);                                        // 0xf78 LQ vf21, 7(vi1)
    for (int c = 0; c < 3; ++c)                                   // 0xf80 MULAw.xyz ACC, vf29, vf22w
        acc[c] = mul(vf[29][c], vf[22][3]);
    isw(vu, vi[7], vi[14] - 1, 0);                                //       ISW.x vi7, -1(vi14)
    for (int c = 0; c < 3; ++c)                                   // 0xf88 MADDAw.xyz ACC, vf30, vf23w
        acc[c] = madd(acc[c], vf[30][c], vf[23][3]);
    lq(vu, 12, 17);                                               //       LQ vf12, 17(vi0)
    for (int c = 0; c < 3; ++c)                                   // 0xf90 MADDw.xyz vf24, vf20, vf0w
        vf[24][c] = madd(acc[c], vf[20][c], 1.0f);
    lq(vu, 13, 18);                                               //       LQ vf13, 18(vi0)
    for (int c = 0; c < 3; ++c)                                   // 0xf98 MSUBw.xyz vf23, vf20, vf0w
        vf[23][c] = msub(acc[c], vf[20][c], 1.0f);
    isw(vu, vi[10], vi[14] - 1, 1);                               //       ISW.y vi10, -1(vi14)
    for (int c = 0; c < 4; ++c)                                   // 0xfa8 FTOI0.xyzw vf21, vf21
    {
        const int32_t i = ftoi0(vf[21][c]);
        std::memcpy(&vf[21][c], &i, 4);
    }
    for (int c = 0; c < 4; ++c)                                   // 0xfb0 MULAx.xyzw ACC, vf1, vf24x
        acc[c] = mul(vf[1][c], vf[24][0]);
    for (int c = 0; c < 3; ++c)                                   // 0xfb8 SUB.xyz vf23, vf0, vf23
        vf[23][c] = sub(0.0f, vf[23][c]);
    vu.i = 2.0f;                                                  //       LOI 2
    for (int c = 0; c < 4; ++c)                                   // 0xfc0 MADDAy.xyzw ACC, vf2, vf24y
        acc[c] = madd(acc[c], vf[2][c], vf[24][1]);
    for (int c = 0; c < 4; ++c)                                   // 0xfc8 MADDAz.xyzw ACC, vf3, vf24z
        acc[c] = madd(acc[c], vf[3][c], vf[24][2]);
    for (int c = 0; c < 4; ++c)                                   // 0xfd0 MADDw.xyzw vf25, vf4, vf0w
        vf[25][c] = madd(acc[c], vf[4][c], 1.0f);
    for (int c = 0; c < 4; ++c)                                   // 0xfd8 MULAx.xyzw ACC, vf1, vf23x
        acc[c] = mul(vf[1][c], vf[23][0]);
    for (int c = 0; c < 4; ++c)                                   // 0xfe0 MADDAy.xyzw ACC, vf2, vf23y
        acc[c] = madd(acc[c], vf[2][c], vf[23][1]);
    for (int c = 0; c < 4; ++c)                                   // 0xfe8 MADDAz.xyzw ACC, vf3, vf23z
        acc[c] = madd(acc[c], vf[3][c], vf[23][2]);
    for (int c = 0; c < 4; ++c)                                   // 0xff0 MADDw.xyzw vf26, vf4, vf0w
        vf[26][c] = madd(acc[c], vf[4][c], 1.0f);
    vu.q = norm(1.0f / norm(vf[25][3]));                          //       DIV Q, vf0w, vf25w (read at 0x1028)
    vf[30][3] = sub(vf[25][3], vu.i);                             // 0xff8 SUBi.w vf30, vf25, I
    const uint32_t mac = macLane(vf[30][3], 3);
    vi[7] = 16;                                                   //       IADDIU vi7, vi0, 16
    acc[3] = mul(1.0f, vf[31][2]);                                // 0x1000 MULAz.w ACC, vf0, vf31z
    vf[9][3] = madd(acc[3], vf[31][3], vf[25][3]);                // 0x1008 MADDw.w vf9, vf31, vf25w
    vu.i = 32.0f;                                                 //        LOI 32
    for (int c = 0; c < 2; ++c)                                   // 0x1010 MINI.xy vf28, vf25, vf26
        vf[28][c] = vmin(vf[25][c], vf[26][c]);
    for (int c = 0; c < 2; ++c)                                   // 0x1018 MAX.xy vf29, vf25, vf26
        vf[29][c] = vmax(vf[25][c], vf[26][c]);
    vi[7] = vi16(int32_t(mac) & vi[7]);                           //        FMAND vi7, vi7 (flags of 0xff8)
    vu.mac = mac;
    vf[27][0] = add(0.0f, vu.i);                                  // 0x1020 ADDi.x vf27, vf0, I
    vu.pc = 0x1028;
}
