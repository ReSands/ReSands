// Known-good answer (by hand, 24 Oct; also the end-to-end check of the split clip check): P6 stage "a_ck_next",
// the end of the clip check's vertex loop, pc 0x6f0 to the loop branch (taken: its delay slot, then 0x698;
// not taken: B 0xcf0, the program's tail).
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

int32_t ftoi4(float v)
{
    const double d = double(norm(v)) * 16.0;
    if (d >= 2147483647.0)
        return INT32_MAX;
    if (d <= -2147483648.0)
        return INT32_MIN;
    return int32_t(d);
}
}

void stage(VuState &vu)
{
    // 0x6f0 MULz.xy vf26, vf13, vf13z | IBNE vi10, vi0, 0x698
    const float z = norm(vu.vf[13][2]);
    vu.vf[26][0] = norm(norm(vu.vf[13][0]) * z);
    vu.vf[26][1] = norm(norm(vu.vf[13][1]) * z);
    const bool loop = vu.vi[10] != 0;
    // 0x6f8 FTOI4.xyzw vf25, vf25 | ILW.w vi9, 2(vi2)   (delay slot)
    for (int c = 0; c < 4; c++)
    {
        const int32_t i = ftoi4(vu.vf[25][c]);
        std::memcpy(&vu.vf[25][c], &i, 4);
    }
    uint32_t w;
    std::memcpy(&w, vu.mem + 16 * ((vu.vi[2] + 2) & 0x3ff) + 12, 4);
    vu.vi[9] = int16_t(w & 0xffff);
    if (loop)
    {
        vu.pc = 0x698;
        return;
    }
    // 0x700 B 0xcf0 | 0x708 NOP (delay slot)
    vu.pc = 0xcf0;
}
