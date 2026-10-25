// Known-good answer (by hand, 25 Oct; Qwen missed it in 6 rounds): P9 stage "d_save", the entry of the
// nearest-segment subroutine 0x548: saves vi4-vi11 (ISW) and vf28-vf31 (SQD) on the stack at vi14, then sets
// up the segment loop (vi3 = the curve points, vi4 = TOP + 58, vi9 = 0, vi10 = the point count, vf31 = 0).
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
    isw(vu, vi[4], vi[14], 1);                                    // 0x5c0 ISW.y vi4, 0(vi14)
    isw(vu, vi[5], vi[14], 2);                                    // 0x5c8 ISW.z vi5, 0(vi14)
    isw(vu, vi[6], vi[14], 3);                                    // 0x5d0 ISW.w vi6, 0(vi14)
    isw(vu, vi[7], vi[14] + 1, 0);                                // 0x5d8 ISW.x vi7, 1(vi14)
    isw(vu, vi[8], vi[14] + 1, 1);                                // 0x5e0 ISW.y vi8, 1(vi14)
    isw(vu, vi[9], vi[14] + 1, 2);                                // 0x5e8 ISW.z vi9, 1(vi14)
    isw(vu, vi[10], vi[14] + 1, 3);                               // 0x5f0 ISW.w vi10, 1(vi14)
    isw(vu, vi[11], vi[14] + 2, 0);                               // 0x5f8 ISW.x vi11, 2(vi14)
    for (int r = 28; r <= 31; ++r)                                // 0x600-0x618 SQD.xyzw vfR, (vi14--)
    {
        vi[14] = vi16(vi[14] - 1);
        std::memcpy(vu.mem + 16 * (uint32_t(vi[14]) & 0x3FF), vf[r], 16);
    }
    uint32_t w;                                                   // 0x620 ILW.x vi10, 15(vi1)
    std::memcpy(&w, vu.mem + 16 * (uint32_t(vi[1] + 15) & 0x3FF), 4);
    vi[10] = vi16(int32_t(w & 0xFFFF));
    vi[3] = vi16(vi[1] + 16);                                     // 0x628 IADDIU vi3, vi1, 16
    vi[4] = vi16(vi[1] + 58);                                     // 0x630 IADDIU vi4, vi1, 58
    for (int c = 0; c < 4; ++c)                                   // 0x638 SUB.xyzw vf31, vf31, vf31
        vf[31][c] = sub(vf[31][c], vf[31][c]);
    vi[9] = 0;                                                    //       IADD vi9, vi0, vi0
    vu.pc = 0x640;
}
