// Known-good answer (by hand, 24 Oct; also the end-to-end check of the split clip check): P6 stage "a_ck_ret",
// the clip check's return from the triangle test subroutine (0x1568): 0 = keep going at 0x6f0, else the
// polygon clipper (0x7e8, never reached in the recorded scenes).
#include "vu_task.h"

void stage(VuState &vu)
{
    // 0x7d8 IBEQ vi1, vi0, 0x6f0; 0x7e0 NOP (delay slot)
    vu.pc = vu.vi[1] == 0 ? 0x6f0 : 0x7e8;
}
