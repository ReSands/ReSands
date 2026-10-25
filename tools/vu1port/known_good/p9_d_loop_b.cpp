// Known-good answer (by hand, 25 Oct; Qwen missed it in 6 rounds): P9 stage "d_loop_b", the second half of the
// segment loop: four dot products (vf15-vf18 .x), their signs (FMAND 0x88 on each ADDz.x's MAC flags); each
// non-negative one adds its point's z to ACC.z and counts in vi9; vf31.z = the sum, vf31.w = the count.
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
    for (int c = 0; c < 4; ++c)                                   // 0x748-0x760 SUB.xyzw
    {
        vf[11][c] = sub(vf[12][c], vf[19][c]);
        vf[12][c] = sub(vf[13][c], vf[20][c]);
        vf[13][c] = sub(vf[14][c], vf[21][c]);
        vf[14][c] = sub(vf[28][c], vf[22][c]);
    }
    for (int c = 0; c < 4; ++c)                                   // 0x768-0x780 MUL.xyzw
    {
        vf[15][c] = mul(vf[11][c], vf[15][c]);
        vf[16][c] = mul(vf[12][c], vf[16][c]);
        vf[17][c] = mul(vf[13][c], vf[17][c]);
        vf[18][c] = mul(vf[14][c], vf[18][c]);
    }
    for (int r = 15; r <= 18; ++r)                                // 0x788-0x7a0 ADDy.x vfR, vfR, vfRy
        vf[r][0] = add(vf[r][0], vf[r][1]);
    uint32_t mac[4];
    for (int r = 15; r <= 18; ++r)                                // 0x7a8-0x7c0 ADDz.x vfR, vfR, vfRz
    {
        vf[r][0] = add(vf[r][0], vf[r][2]);
        mac[r - 15] = macLane(vf[r][0], 0);
    }
    vi[11] = 136;                                                 // 0x7c0 IADDIU vi11, vi0, 136
    vi[5] = int32_t(mac[0]) & vi[11];                             // 0x7c8-0x7e0 FMAND vi5..vi8, vi11
    vi[6] = int32_t(mac[1]) & vi[11];
    vi[7] = int32_t(mac[2]) & vi[11];
    vi[8] = int32_t(mac[3]) & vi[11];
    vu.mac = mac[3];
    acc[2] = mul(vf[31][2], 1.0f);                                // 0x7e0 MULAw.z ACC, vf31, vf0w
    const int sel[4] = {vi[5], vi[6], vi[7], vi[8]};              // 0x7e8-0x840: IBNE viK skips its point
    for (int k = 0; k < 4; ++k)
        if (sel[k] == 0)
        {
            acc[2] = madd(acc[2], vf[19 + k][2], 1.0f);           // MADDAw.z ACC, vf19+k, vf0w
            vi[9] = vi16(vi[9] + 1);                              // IADDIU vi9, vi9, 1
        }
    vf[31][2] = madd(acc[2], 0.0f, 1.0f);                         // 0x848 MADDw.z vf31, vf0, vf0w
    const bool again = vi[10] != 0;                               //       IBNE vi10, vi0, 0x640
    std::memcpy(&vf[31][3], &vi[9], 4);                           // 0x850 MFIR.w vf31, vi9 (delay slot)
    vu.pc = again ? 0x640 : 0x858;
}
