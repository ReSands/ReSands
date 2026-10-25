// Known-good answer (by hand, 25 Oct; Qwen missed it in 34 rounds): P6 stage "a_ck_head", the set-up of
// variant A's clip check: the buffer (XTOP), the first vertex through vf1-vf4 (vf24) and vf5-vf8 (vf10),
// vf25 = vf24.xyz * vf13.z (FTOI4), vf26.xy = vf13.xy * vf13.z, clip flags cleared.
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

namespace
{
    int32_t ftoi4(float v)
    {
        const double d = double(norm(v)) * 16.0;
        if (d >= 2147483647.0)
            return INT32_MAX;
        if (d <= -2147483648.0)
            return INT32_MIN;
        return int32_t(d);
    }
    int32_t ilw(const VuState &vu, int32_t qword, int lane)
    {
        uint32_t v;
        std::memcpy(&v, vu.mem + 16 * (uint32_t(qword) & 0x3FF) + 4 * lane, 4);
        return vi16(int32_t(v & 0xFFFF));
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    float *acc = vu.acc;
    vi[12] = vi[14];                                              // 0x5e0 IADD vi12, vi0, vi14
    vi[14] = 1023;                                                // 0x5e8 IADDIU vi14, vi0, 1023
    vi[1] = vi16(int32_t(vu.top));                                // 0x5f0 XTOP vi1
    vi[13] = ilw(vu, vi[1], 0);                                   // 0x5f8 ILW.x vi13, 0(vi1)
    lq(vu, 12, vi[1] + 3);                                        // 0x600 LQ vf12, 3(vi1)
    vi[2] = vi16(vi[1] + 51);                                     // 0x608 IADDIU vi2, vi1, 51
    lq(vu, 31, vi[1] + 7);                                        // 0x610 LQ vf31, 7(vi1)
    lq(vu, 9, vi[2] + 2);                                         // 0x618 LQ vf9, 2(vi2)
    lq(vu, 13, vi[2]);                                            // 0x620 LQ vf13, 0(vi2)
    vi[9] = ilw(vu, vi[2] + 2, 3);                                // 0x628 ILW.w vi9, 2(vi2)
    {                                                             // 0x630 FTOI0.z vf12, vf12
        const float v = norm(vf[12][2]);
        const double d = double(v);
        const int32_t i = d >= 2147483647.0 ? INT32_MAX : d <= -2147483648.0 ? INT32_MIN : int32_t(d);
        std::memcpy(&vf[12][2], &i, 4);
    }
    vu.clip = 0;                                                  //       FCSET 0x000000
    for (int c = 0; c < 4; ++c)                                   // 0x638 MULAx.xyzw ACC, vf1, vf9x
        acc[c] = mul(vf[1][c], vf[9][0]);
    lq(vu, 5, 5);                                                 //       LQ vf5, 5(vi0)
    for (int c = 0; c < 4; ++c)                                   // 0x640 MADDAy.xyzw ACC, vf2, vf9y
        acc[c] = madd(acc[c], vf[2][c], vf[9][1]);
    lq(vu, 6, 6);                                                 //       LQ vf6, 6(vi0)
    for (int c = 0; c < 4; ++c)                                   // 0x648 MADDAz.xyzw ACC, vf3, vf9z
        acc[c] = madd(acc[c], vf[3][c], vf[9][2]);
    lq(vu, 7, 7);                                                 //       LQ vf7, 7(vi0)
    for (int c = 0; c < 4; ++c)                                   // 0x650 MADDw.xyzw vf24, vf4, vf0w
        vf[24][c] = madd(acc[c], vf[4][c], 1.0f);
    lq(vu, 8, 8);                                                 //       LQ vf8, 8(vi0)
    for (int c = 0; c < 4; ++c)                                   // 0x658 MULAx.xyzw ACC, vf5, vf9x
        acc[c] = mul(vf[5][c], vf[9][0]);
    {                                                             //       MTIR vi10, vf12z
        uint32_t b;
        std::memcpy(&b, &vf[12][2], 4);
        vi[10] = vi16(int32_t(b & 0xFFFF));
    }
    for (int c = 0; c < 4; ++c)                                   // 0x660 MADDAy.xyzw ACC, vf6, vf9y
        acc[c] = madd(acc[c], vf[6][c], vf[9][1]);
    vi[12] = 16384;                                               //       IADDIU vi12, vi0, 16384
    for (int c = 0; c < 4; ++c)                                   // 0x668 MADDAz.xyzw ACC, vf7, vf9z
        acc[c] = madd(acc[c], vf[7][c], vf[9][2]);
    vi[12] = vi16(vi[12] + vi[12]);                               //       IADD vi12, vi12, vi12
    for (int c = 0; c < 3; ++c)                                   // 0x670 MULz.xyz vf25, vf24, vf13z
        vf[25][c] = mul(vf[24][c], vf[13][2]);
    lq(vu, 9, vi[2] + 5);                                         //       LQ vf9, 5(vi2)
    for (int c = 0; c < 4; ++c)                                   // 0x678 MADDw.xyzw vf10, vf8, vf0w
        vf[10][c] = madd(acc[c], vf[8][c], 1.0f);
    for (int c = 0; c < 2; ++c)                                   // 0x680 MULz.xy vf26, vf13, vf13z
        vf[26][c] = mul(vf[13][c], vf[13][2]);
    vi[11] = 32767;                                               // 0x688 IADDIU vi11, vi0, 32767
    for (int c = 0; c < 4; ++c)                                   // 0x690 FTOI4.xyzw vf25, vf25
    {
        const int32_t i = ftoi4(vf[25][c]);
        std::memcpy(&vf[25][c], &i, 4);
    }
    vu.pc = 0x698;
}
