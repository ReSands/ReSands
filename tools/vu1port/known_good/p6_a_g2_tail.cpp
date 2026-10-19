// Known-good answer (by hand, as p6_a_g1_tail.cpp, the same code): P6 stage "p6_a_g2_tail", the end of grid 2's outer loop, pc 0xc98 to
// the loop branch at 0xce0 and its delay slot (then 0x930 or the tail at 0xcf0).
#include "vu_task.h"

#include <cstring>

namespace
{
    enum { X = 0, Y = 1, Z = 2 };
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }
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
    vi[4] = vi16(vi[4] - 1);              // 0xc98 ISUBIU vi4, vi4, 1
    vi[10] = ilw(vu, 1023, Z);            // 0xca0 ILW.z vi10, 1023(vi0)
    vi[14] = ilw(vu, 1023, X);            // 0xca8 ILW.x vi14
    vi[15] = ilw(vu, 1023, Y);            // 0xcb0 ILW.y vi15
    const bool skip = vi[11] != 0;        // 0xcb8 IBNE vi11, vi0, 0xcd0
    vi[3] = vi16(vi[3] - 1);              // 0xcc0 (delay slot) ISUBIU vi3, vi3, 1
    if (!skip)
        vi[2] = vi16(vi[2] - vi[10]);     // 0xcc8 ISUB vi2, vi2, vi10
    vf[21][X] = vf[22][X] + vf[0][X];     // 0xcd0 ADDx.xz vf21, vf22, vf0x | IADDIU vi11, vi11, 1
    vf[21][Z] = vf[22][Z] + vf[0][X];
    vi[11] = vi16(vi[11] + 1);
    vf[21][Y] = vf[22][Y] + vf[31][Y];    // 0xcd8 ADD.y vf21, vf22, vf31 | IADD vi7, vi2, vi10
    vi[7] = vi16(vi[2] + vi[10]);
    vf[22][Y] = vf[22][Y] + vf[31][Y];    // 0xce0 ADD.y vf22, vf22, vf31 | IBNE vi11, vi15, 0x930
    const bool loop = vi[11] != vi[15];
    vi[12] = vi[14];                      // 0xce8 (delay slot) IADD vi12, vi0, vi14
    vu.pc = loop ? 0x930 : 0xcf0;
}
