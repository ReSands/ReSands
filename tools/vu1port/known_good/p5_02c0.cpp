// Known-good answer (by hand, 26 Oct): P5 stage "02c0", the tail of the subroutine at 0x240 (called with BAL vi15
// from 0x8f8 / 0xf20; 0x240 copies the link into vi1): two more header words, eleven qwords into vf31-vf18, JR vi1.
#include "vu_task.h"

#include <cstdint>
#include <cstring>

namespace
{
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }
    int32_t ilw(const VuState &vu, int32_t qword, int lane)
    {
        uint32_t w;
        std::memcpy(&w, vu.mem + 16 * (uint32_t(qword) & 0x3FF) + 4 * lane, 4);
        return vi16(int32_t(w & 0xFFFF));
    }
    void lqi(VuState &vu, int t, int s)
    {
        std::memcpy(vu.vf[t], vu.mem + 16 * (uint32_t(vu.vi[s]) & 0x3FF), 16);
        vu.vi[s] = vi16(vu.vi[s] + 1);
    }
}

void stage(VuState &vu)
{
    vu.vi[4] = ilw(vu, vu.vi[14], 2);       // 0x2c0 ILWR.z vi4, (vi14)
    vu.vi[5] = ilw(vu, vu.vi[14], 3);       // 0x2c8 ILWR.w vi5, (vi14)
    vu.vi[14] = vi16(vu.vi[14] + 1);        // 0x2d0 IADDIU vi14, vi14, 1
    for (int t : {31, 30, 29, 28, 26, 25, 24, 23, 22, 19, 18})
        lqi(vu, t, 14);                     // 0x2d8-0x328 LQI.xyzw vfT, (vi14++)
    vu.pc = uint32_t(uint16_t(vu.vi[1])) * 8; // 0x330 JR vi1 (0x338 NOP delay slot): 0x908 or 0xf30
}
