// Known-good answer (by hand, 26 Oct): P5 stage "1550", a loop rotating vf25 and vf26 (MR32: xyzw <- yzwx)
// vi12 times, vi12 counted down to 0.
#include "vu_task.h"

#include <cstdint>

namespace
{
    void mr32(float *v)
    {
        const float x = v[0];
        v[0] = v[1];
        v[1] = v[2];
        v[2] = v[3];
        v[3] = x;
    }
}

void stage(VuState &vu)
{
    while (vu.vi[12] != 0)                  // 0x1550 IBEQ vi12, vi0, 0x1590 (0x1558 NOP)
    {
        mr32(vu.vf[25]);                    // 0x1560 MR32.xyzw vf25, vf25
        mr32(vu.vf[26]);                    // 0x1568 MR32.xyzw vf26, vf26
        vu.vi[12] = int32_t(int16_t(vu.vi[12] - 1)); // 0x1570 ISUBIU vi12, vi12, 1
    }                                       // 0x1580 B 0x1550 (0x1588 NOP)
    vu.pc = 0x1590;
}
