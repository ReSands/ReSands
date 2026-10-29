// Known-good answer: Qwen's passing port of P6 stage "a_g1_row" (pc 0x288 to 0x338) with the LQ vf4, 4(vi0) at 0x310
// it dropped, found 29 Oct when the colour pass ran without the mask pass before it (offline, pop_state_frame_vu1).
#include "vu_task.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    enum { X = 0, Y = 1, Z = 2, W = 3 };

    int32_t ftoi0(float f)
    {
        if (!(f == f)) return 0;
        if (f >= 2147483647.0f) return 0x7FFFFFFF;
        if (f <= -2147483648.0f) return int32_t(0x80000000u);
        return int32_t(f);
    }
    int32_t ftoi4(float f) { return ftoi0(f * 16.0f); }
    void setInt(float &lane, int32_t v) { std::memcpy(&lane, &v, 4); }
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }

    void lq(VuState &vu, float *dst, int32_t qword)
    {
        std::memcpy(dst, vu.mem + 16 * (uint32_t(qword) & 0x3FF), 16);
    }
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

    // 0x288
    vf[21][Z] = vf[9][Z] + vf[0][Z];
    for (int c = 0; c < 4; ++c) vf[24][c] = vf[20][c];

    // 0x290
    vf[9][Z] = vf[9][Z] + vf[30][Y];
    lq(vu, vf[20], vi[4]);
    vi[4] = vi16(vi[4] + 1);

    // 0x298
    vf[31][Z] = vf[30][Z] + vf[30][W];
    vi[5] = vi16(vi[0] + vi[0]);

    // 0x2a0
    vf[30][Z] = vf[30][Z] + vf[30][W];
    lq(vu, vf[25], vi[4]);

    // 0x2a8
    vf[23][Y] = vf[0][Y];
    vf[23][Z] = vf[0][Z];
    vi[6] = vi16(vi[0] + vi[0]);

    // 0x2b0
    for (int c = 0; c < 4; ++c) vf[13][c] = std::max(vf[20][c], vf[0][X]);
    vu.i = 0.15f;

    // 0x2b8
    for (int c = 0; c < 4; ++c) vf[27][c] = std::min(vf[20][c], vu.i);
    lq(vu, vf[26], vi[3]);
    vi[3] = vi16(vi[3] + 1);

    // 0x2c0
    vf[21][X] = vf[21][X] - vf[31][X];
    vf[21][Z] = vf[21][Z] - vf[31][Z];
    vi[12] = ilw(vu, 1023, X);

    // 0x2c8
    for (int c = 0; c < 4; ++c) vf[14][c] = 0;
    vi[1] = vi16(int32_t(vu.top));

    // 0x2d0
    for (int c = 0; c < 4; ++c) vf[13][c] = vf[13][c] * 2;
    vu.i = -0.15f;

    // 0x2d8
    for (int c = 0; c < 4; ++c) vf[27][c] = std::max(vf[27][c], vu.i);
    lq(vu, vf[10], vi[1] + 4);

    // 0x2e0
    for (int c = 0; c < 4; ++c) vf[28][c] = vf[20][c] - vf[26][c];
    vf[16][X] = vf[21][X];
    vf[16][Y] = vf[21][Y];

    // 0x2e8
    vf[5][Y] = vf[20][Y] - vf[24][W];
    lq(vu, vf[1], 1);

    // 0x2f0
    for (int c = 0; c < 4; ++c) vf[13][c] = std::min(vf[13][c], vf[0][W]);
    lq(vu, vf[2], 2);

    // 0x2f8
    vf[16][Z] = vf[21][Z] + vf[27][X];
    vu.i = 255.0f;

    // 0x300
    vf[21][X] = vf[21][X] + vf[31][X];
    vf[21][Z] = vf[21][Z] + vf[31][Z];
    lq(vu, vf[3], 3);

    // 0x308
    // ADDAi.xyzw ACC, vf14, I
    // Reads I=255.0f (from 0x2f8). vf14 is 0.
    // ACC = vf14 + I = 0 + 255 = 255.
    for (int c = 0; c < 4; ++c) vu.acc[c] = vf[14][c] + 255.0f;
    vu.i = 60.0f;

    // 0x310
    // MSUBi.xyzw vf13, vf13, I
    // Reads I=60.0f.
    // vf13 = ACC - vf13 * I.
    for (int c = 0; c < 4; ++c) vf[13][c] = vu.acc[c] - vf[13][c] * 60.0f;
    lq(vu, vf[4], 4); // LQ.xyzw vf4, 4(vi0): missing in Qwen's port (the traces always carried vf4 in from the mask pass)

    // 0x318
    vf[5][X] = vf[0][X] + vf[5][Y];

    // 0x320
    vf[5][Y] = vf[0][Y] + vf[28][X];

    // 0x328
    // SUBx.z vf6, vf20, vf20x
    // Only writes vf6.z.
    vf[6][Z] = vf[20][Z] - vf[20][X];

    // 0x330
    for (int c = 0; c < 4; ++c) setInt(vf[13][c], ftoi4(vf[13][c]));

    vu.pc = 0x338;
}
