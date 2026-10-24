// Known-good answer (by hand, 24 Oct): P9 (the sprite VU1 program, ELF MPG packet 0x4E338C) stage "setup", the
// MSCALF entry 0x340: the registers for the setup subroutine at 0x000, then its call (stage p9_init).
#include "vu_task.h"

#include <cstring>

namespace
{
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
    int32_t *vi = vu.vi;
    vi[2] = 0;                          // 0x340 IADDIU vi2, vi0, 0
    vi[1] = 4;                          // 0x348 IADDIU vi1, vi0, 4
    vi[14] = 1018;                      // 0x350 IADDIU vi14, vi0, 1018
    vi[11] = 32767;                     // 0x358 IADDIU vi11, vi0, 32767
    vi[9] = vi16(vi[11] + 1);           // 0x360 IADDIU vi9, vi11, 1 (16 bits: -32768)
    vi[10] = ilw(vu, 17, 0);            // 0x368 ILW.x vi10, 17(vi0)
    vi[15] = 0x380 / 8;                 // 0x370 BAL vi15, 0x0 (returns to 0x380); 0x378 NOP (delay slot)
    vu.pc = 0x000;
}
