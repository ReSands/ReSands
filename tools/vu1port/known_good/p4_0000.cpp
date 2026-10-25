// Known-good answer (by hand, 25 Oct; the end-to-end check of the P4 set-up): P4 stage "0000", the MSCAL entry:
// a count from qword 30 .w (vi10 = its low 15 bits), vi14 = 1006, vf17 = qword 1015, then on to 0x5a0. The branch
// to 0x58 (a count above 7) never ran in the recorded scenes.
#include "vu_task.h"

#include <cstdint>
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
    vi[10] = ilw(vu, 30, 3);                        // 0x00 ILW.w vi10, 30(vi0)
    vi[11] = 32767;                                 // 0x08 IADDIU vi11, vi0, 32767
    vi[14] = 1006;                                  // 0x10 IADDIU vi14, vi0, 1006
    vi[1] = 8;                                      // 0x18 IADDIU vi1, vi0, 8
    vi[10] = vi16(vi[11] & vi[10]);                 // 0x20 IAND vi10, vi11, vi10
    vi[3] = vi16(vi[10] - 7);                       // 0x28 ISUBIU vi3, vi10, 7
    vi[4] = 1;                                      // 0x30 IADDIU vi4, vi0, 1
    const bool many = vi[3] > 0;                    // 0x38 IBGTZ vi3, 0x58
    std::memcpy(vu.vf[17], vu.mem + 16 * 1015, 16); // 0x40 LQ.xyzw vf17, 1015(vi0) (delay slot)
    if (many)
    {
        vu.pc = 0x58;
        return;
    }
    vu.pc = 0x5a0;                                  // 0x48 B 0x5a0 | 0x50 NOP (delay slot)
}
