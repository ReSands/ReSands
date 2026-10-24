// Known-good answer (by hand, 24 Oct): P9 stage "init", the setup subroutine 0x000-0x140 (called from 0x370):
// two matrices times four vectors (vf1-vf4 = (vf9..vf12) * vf17..vf20, vf5-vf8 = (vf25..vf28) * vf17..vf20,
// the vectors and matrices read with LQI from vi2 / vi1), then the constants at 13-17; returns with JR vi15.
#include "vu_task.h"

#include <cstring>

namespace
{
    enum { X = 0, Y = 1, Z = 2, W = 3 };
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }
    void lq(VuState &vu, float *dst, int32_t qword) { std::memcpy(dst, vu.mem + 16 * (uint32_t(qword) & 0x3FF), 16); }
    int32_t ilw(const VuState &vu, int32_t qword, int lane)
    {
        uint32_t v;
        std::memcpy(&v, vu.mem + 16 * (uint32_t(qword) & 0x3FF) + 4 * lane, 4);
        return vi16(int32_t(v & 0xFFFF));
    }
    // VU float operand: a denormal reads as zero of the same sign, an infinity / NaN bit pattern as the largest float
    float norm(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        if ((b & 0x7F800000u) == 0)
            b &= 0x80000000u;
        else if ((b & 0x7F800000u) == 0x7F800000u)
            b = (b & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&f, &b, 4);
        return f;
    }
    // MULAx ACC, m0, v.x; MADDAy ACC, m1, v.y; MADDAz ACC, m2, v.z; MADDw out, m3, v.w
    void mulMatrix(VuState &vu, float *out, float (*m)[4], const float *v)
    {
        float o[4];
        for (int c = 0; c < 4; ++c)
        {
            float acc = norm(norm(m[0][c]) * norm(v[X]));
            acc = norm(acc + norm(norm(m[1][c]) * norm(v[Y])));
            acc = norm(acc + norm(norm(m[2][c]) * norm(v[Z])));
            vu.acc[c] = acc;
            o[c] = norm(acc + norm(norm(m[3][c]) * norm(v[W])));
        }
        std::memcpy(out, o, 16);
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    // 0x000-0x020: vf17 from vi2, vf9-vf12 from vi1 (LQI: use the address, then add 1)
    lq(vu, vf[17], vi[2]), vi[2] = vi16(vi[2] + 1);
    for (int r = 9; r <= 12; ++r)
        lq(vu, vf[r], vi[1]), vi[1] = vi16(vi[1] + 1);
    // 0x028-0x040: vf1 = (vf9..vf12) * vf17; the lowers load vf18-vf20 (vi2++) and vf25 (vi1++)
    float m1[4][4];
    std::memcpy(m1, &vf[9], sizeof m1);
    mulMatrix(vu, vf[1], m1, vf[17]);
    for (int r = 18; r <= 20; ++r)
        lq(vu, vf[r], vi[2]), vi[2] = vi16(vi[2] + 1);
    lq(vu, vf[25], vi[1]), vi[1] = vi16(vi[1] + 1);
    // 0x048-0x060: vf2 = M1 * vf18; lowers vf26-vf28 (vi1++)
    mulMatrix(vu, vf[2], m1, vf[18]);
    for (int r = 26; r <= 28; ++r)
        lq(vu, vf[r], vi[1]), vi[1] = vi16(vi[1] + 1);
    mulMatrix(vu, vf[3], m1, vf[19]);   // 0x068-0x080
    mulMatrix(vu, vf[4], m1, vf[20]);   // 0x088-0x0a0
    float m2[4][4];
    std::memcpy(m2, &vf[25], sizeof m2);
    mulMatrix(vu, vf[5], m2, vf[17]);   // 0x0a8-0x0c0
    // 0x0c8-0x0e0: vf6 = M2 * vf18; lowers: vi11 = 32767, vf9 = qword 16, vi8 = its x (16 bits)
    mulMatrix(vu, vf[6], m2, vf[18]);
    vi[11] = 32767;
    lq(vu, vf[9], 16);
    vi[8] = ilw(vu, 16, 0);
    // 0x0e8-0x100: vf7 = M2 * vf19; lowers: vi8 &= vi11, vi9 = vi11 + 1 (16 bits)
    mulMatrix(vu, vf[7], m2, vf[19]);
    vi[8] = vi16(vi[8] & vi[11]);
    vi[9] = vi16(vi[11] + 1);
    // 0x108-0x120: vf8 = M2 * vf20 (vf28 is read before 0x128 reloads it); lowers vf31 / vf29 / vf30 = qwords 15 / 13 / 14
    mulMatrix(vu, vf[8], m2, vf[20]);
    lq(vu, vf[31], 15);
    lq(vu, vf[29], 13);
    lq(vu, vf[30], 14);
    vf[18][Z] = 1.0f;                   // 0x128 ADDw.z vf18, vf0, vf0w (0 + 1)
    lq(vu, vf[28], 12);                 //       LQ vf28, 12(vi0)
    vf[19][Z] = 1.0f;                   // 0x130 ADDw.z vf19, vf0, vf0w
    vu.pc = uint32_t(vi[15]) * 8;       // 0x138 JR vi15; 0x140 NOP (delay slot)
}
