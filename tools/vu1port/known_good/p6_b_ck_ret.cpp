// Known-good answer (by hand, 24 Oct; also the end-to-end check of the split clip check): P6 stage "b_ck_ret",
// the clip check's return from the triangle test subroutine (0x2550): 0 = keep going at 0x19f0, else the
// polygon clipper (0x1ae8, never reached in the recorded scenes).
#include "vu_task.h"

void stage(VuState &vu)
{
    // 0x1ad8 IBEQ vi1, vi0, 0x19f0; 0x1ae0 NOP (delay slot)
    vu.pc = vu.vi[1] == 0 ? 0x19f0 : 0x1ae8;
}
