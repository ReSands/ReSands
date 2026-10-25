// Known-good answer (by hand, 25 Oct; Qwen missed it in 6 rounds): P9 stage "f_b", the strip ends to screen
// space (times 1/w), the clamped strip width (vf9.w / vf10.w), the window tests (FMAND on the SUB signs),
// then the call of the strip writer 0x10d8 (BAL vi12) or the early return to 0x10c0.
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
    const float q0 = norm(vu.q);                                  // the DIV of 0xff0
    for (int c = 0; c < 3; ++c)                                   // 0x1028 MULq.xyz vf18, vf18, Q
        vf[18][c] = mul(vf[18][c], q0);
    const bool early = vi[7] != 0;                                //        IBNE vi7, vi0, 0x10c0
    for (int c = 0; c < 2; ++c)                                   // 0x1030 MULq.xy vf9, vf28, Q (delay slot)
        vf[9][c] = mul(vf[28][c], q0);
    if (early)
    {
        vu.pc = 0x10c0;
        return;
    }
    for (int c = 0; c < 2; ++c)                                   // 0x1038 MULq.xy vf10, vf29, Q
        vf[10][c] = mul(vf[29][c], q0);
    for (int c = 0; c < 2; ++c)                                   // 0x1040 MULAq.xy ACC, vf29, Q
        acc[c] = mul(vf[29][c], q0);
    for (int c = 0; c < 2; ++c)                                   // 0x1048 MSUBq.xy vf11, vf28, Q
        vf[11][c] = msub(acc[c], vf[28][c], q0);
    uint32_t mac[4] = {};                                         // MAC flags of 0x1050 .. 0x1068 (SUB.xy)
    const auto subxy = [&](int d, int a, int b, int k) {
        mac[k] = 0;
        for (int c = 0; c < 2; ++c)
        {
            vf[d][c] = sub(vf[a][c], vf[b][c]);
            mac[k] |= macLane(vf[d][c], c);
        }
    };
    subxy(17, 13, 9, 0);                                          // 0x1050 SUB.xy vf17, vf13, vf9
    subxy(22, 12, 9, 1);                                          // 0x1058 SUB.xy vf22, vf12, vf9
    vi[7] = 204;                                                  //        IADDIU vi7, vi0, 204
    subxy(30, 10, 12, 2);                                         // 0x1060 SUB.xy vf30, vf10, vf12
    vi[10] = 128;                                                 //        IADDIU vi10, vi0, 128
    subxy(30, 13, 10, 3);                                         // 0x1068 SUB.xy vf30, vf13, vf10
    vu.q = norm(norm(vf[18][2]) / norm(vf[11][0]));               //        DIV Q, vf18z, vf11x
    vf[9][3] = vmin(vf[9][3], vf[31][0]);                         // 0x1070 MINIx.w vf9, vf9, vf31x
    vi[13] = int32_t(mac[0]) & vi[7];                             //        FMAND vi13, vi7 (flags of 0x1050)
    vf[9][2] = mul(vf[25][2], vf[18][2]);                         // 0x1078 MULz.z vf9, vf25, vf18z
    vi[1] = int32_t(mac[1]) & vi[7];                              //        FMAND vi1, vi7 (0x1058)
    vf[10][2] = mul(vf[25][2], vf[18][2]);                        // 0x1080 MULz.z vf10, vf25, vf18z
    vi[12] = int32_t(mac[2]) & vi[7];                             //        FMAND vi12, vi7 (0x1060)
    for (int c = 0; c < 3; ++c)                                   // 0x1088 MULz.xyz vf19, vf19, vf18z
        vf[19][c] = mul(vf[19][c], vf[18][2]);
    vi[11] = int32_t(mac[3]) & vi[7];                             //        FMAND vi11, vi7 (0x1068)
    vu.mac = mac[3];
    vf[10][3] = vmax(vf[9][3], vf[31][1]);                        // 0x1090 MAXy.w vf10, vf9, vf31y
    const bool off = vi[13] != 0;                                 //        IBNE vi13, vi0, 0x10c0
    vf[9][3] = vmax(vf[9][3], vf[31][1]);                         // 0x1098 MAXy.w vf9, vf9, vf31y (delay slot)
    vi[7] = vi[1] & vi[10];                                       //        IAND vi7, vi1, vi10
    if (off)
    {
        vu.pc = 0x10c0;
        return;
    }
    vf[16][3] = mul(1.0f, vu.q);                                  // 0x10a0 MULq.w vf16, vf0, Q (the DIV of 0x1068)
    const bool off2 = vi[12] != 0;                                //        IBNE vi12, vi0, 0x10c0
    vu.q = norm(norm(vf[18][2]) / norm(vf[11][1]));               // 0x10a8 DIV Q, vf18z, vf11y (delay slot)
    if (off2)
    {
        vu.pc = 0x10c0;
        return;
    }
    vi[12] = 0x10c0 / 8;                                          // 0x10b0 BAL vi12, 0x10d8
    vi[4] = vi[11] & vi[10];                                      // 0x10b8 IAND vi4, vi11, vi10 (delay slot)
    vu.pc = 0x10d8;
}
