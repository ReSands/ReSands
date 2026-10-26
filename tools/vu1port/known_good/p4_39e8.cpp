// Known-good answer (by hand, 26 Oct; Qwen read the -1 as a byte offset in 9 rounds): P4 stage "39e8", the same flag OR in a later loop:
// the w word of qword vi6 - 1 gets vi9's bits ORed in, then on to 0x3890.
#include "vu_task.h"

#include <cstdint>
#include <cstring>

void stage(VuState &vu)
{
    int32_t *vi = vu.vi;
    uint8_t *w = vu.mem + 16 * (uint32_t(vi[6] - 1) & 0x3FF) + 12;
    uint32_t v;
    std::memcpy(&v, w, 4);
    vi[15] = int32_t(int16_t(v & 0xFFFF));            // 0x39e8 ILW.w vi15, -1(vi6)
    vi[15] = int32_t(int16_t(vi[15] | vi[9]));        //       IOR vi15, vi15, vi9
    v = uint32_t(vi[15]) & 0xFFFF;                    //       ISW.w vi15, -1(vi6) (before or in the B's delay slot)
    std::memcpy(w, &v, 4);
    vu.pc = 0x3890;
}
