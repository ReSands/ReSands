// Known-good answer (by hand, 24 Oct): P9 stage "main", the MSCNT entry 0x390 up to the per-sprite loop (0x408):
// the buffer registers (vi1 = TOP; the two GS packet buffers at TOP+600 / TOP+800, their GIF tags' yzw from
// qword 16), the sprite count (TOP+100), the random seed (RINIT from 32767).
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
    void sqLanes(VuState &vu, const float *src, int32_t qword, unsigned mask) // mask bit 0 = x .. bit 3 = w
    {
        for (int c = 0; c < 4; ++c)
            if (mask & (1u << c))
                std::memcpy(vu.mem + 16 * (uint32_t(qword) & 0x3FF) + 4 * c, &src[c], 4);
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    vi[1] = vi16(int32_t(vu.top));                  // 0x390 XTOP vi1
    vi[11] = 32767;                                 // 0x398 IADDIU vi11, vi0, 32767
    float vf27[4];
    std::memcpy(vf27, vu.mem + 16 * 16, 16);        // 0x3a0 LQ vf27, 16(vi0)
    std::memcpy(vf[27], vf27, 16);
    const int32_t bits = vi[11];                    // 0x3a8 MFIR.x vf10, vi11 (the sign-extended bits)
    std::memcpy(&vf[10][0], &bits, 4);
    vi[10] = ilw(vu, vi[1] + 100, 0);               // 0x3b0 ILW.x vi10, 100(vi1)
    vi[4] = vi16(vi[1] + 600);                      // 0x3b8
    vi[5] = vi16(vi[1] + 800);                      // 0x3c0
    uint32_t x;                                     // 0x3c8 RINIT R, vf10x
    std::memcpy(&x, &vf[10][0], 4);
    vu.r = 0x3F800000u | (x & 0x7FFFFFu);
    vi[8] = 0;                                      // 0x3d0 IADD vi8, vi0, vi0
    vi[10] = vi16(vi[10] & vi[11]);                 // 0x3d8 IAND vi10, vi10, vi11
    sqLanes(vu, vf27, vi[4], 0xE);                  // 0x3e0 SQ.yzw vf27, 0(vi4)
    sqLanes(vu, vf27, vi[5], 0xE);                  // 0x3e8 SQ.yzw vf27, 0(vi5)
    vi[2] = vi[4];                                  // 0x3f0 IADD vi2, vi4, vi0
    vi[6] = vi16(vi[2] + 1);                        // 0x3f8 IADDIU vi6, vi2, 1
    vi[3] = vi16(vi[1] + 101);                      // 0x400 IADDIU vi3, vi1, 101
    vu.pc = 0x408;
}
