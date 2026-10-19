// Known-good answer for the judge (not a model's): P6 (the water VU1 program, ELF 0x4DAC8C) stage
// "setup", pc 0x000 to the branch at 0x258 (exits 0x268 / 0x1648), written by hand from the
// disassembly and the replayer's hints (Q at 0x0A8-0x120 = 1 / vf11.x from the DIV at 0x070,
// from 0x128 = 1 / vf11.y from the DIV at 0x0F0). Proves the stage cases, the judge and its tolerances.
#include "vu_task.h"

#include <cmath>
#include <cstring>

namespace
{
    enum { X = 0, Y = 1, Z = 2, W = 3 };

    int32_t ftoi0(float f)
    {
        if (!(f == f))
            return 0;
        if (f >= 2147483647.0f)
            return 0x7FFFFFFF;
        if (f <= -2147483648.0f)
            return int32_t(0x80000000u);
        return int32_t(f); // truncation, as FTOI0
    }
    void setInt(float &lane, int32_t v) { std::memcpy(&lane, &v, 4); }
    int32_t getInt(float lane)
    {
        int32_t v;
        std::memcpy(&v, &lane, 4);
        return v;
    }
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }

    void lq(VuState &vu, float *dst, int32_t qword)
    {
        std::memcpy(dst, vu.mem + 16 * (uint32_t(qword) & 0x3FF), 16);
    }
    void isw(VuState &vu, int32_t qword, int lane, int32_t value)
    {
        const uint32_t v = uint32_t(value) & 0xFFFF;
        std::memcpy(vu.mem + 16 * (uint32_t(qword) & 0x3FF) + 4 * lane, &v, 4);
    }
    int32_t ilw(const VuState &vu, int32_t qword, int lane)
    {
        uint32_t v;
        std::memcpy(&v, vu.mem + 16 * (uint32_t(qword) & 0x3FF) + 4 * lane, 4);
        return vi16(int32_t(v & 0xFFFF));
    }
    // CLIPw.xyz a, b: judge a.xyz against |b.w|; the clip register keeps the last four results
    void clipw(VuState &vu, const float *a, const float *b)
    {
        const float w = std::fabs(b[W]);
        const uint32_t f = (a[X] > w ? 1u : 0u) | (a[X] < -w ? 2u : 0u) | (a[Y] > w ? 4u : 0u) | (a[Y] < -w ? 8u : 0u) |
                           (a[Z] > w ? 16u : 0u) | (a[Z] < -w ? 32u : 0u);
        vu.clip = ((vu.clip << 6) | f) & 0xFFFFFF;
    }
    // ACC = vf5 * v.x + vf6 * v.y + vf7 * v.z; out = ACC + vf8 * 1  (the matrix in vf5-vf8)
    void transform(VuState &vu, float *out, const float *v)
    {
        float (*vf)[4] = vu.vf;
        for (int c = 0; c < 4; ++c)
        {
            float acc = vf[5][c] * v[X];
            acc = acc + vf[6][c] * v[Y];
            acc = acc + vf[7][c] * v[Z];
            vu.acc[c] = acc;
            out[c] = acc + vf[8][c] * vf[0][W];
        }
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    vu.i = 10.0f;                                    // 0x000 LOI 10
    vf[17][Z] = vf[0][Z] + vu.i;                     // 0x008
    vi[1] = vi16(int32_t(vu.top));                   //       XTOP
    vf[16][Z] = vf[0][Z] - vu.i;                     // 0x010
    lq(vu, vf[15], vi[1] + 6);
    for (int c = 0; c < 4; ++c)                      // 0x018 / 0x020
        vf[30][c] = vf[0][c] - vf[0][c], vf[27][c] = vf[0][c] - vf[0][c];
    vi[2] = vi16(vi[1] + 51);
    lq(vu, vf[9], vi[1] + 0);
    vf[17][Z] = vf[17][Z] + vu.i;                    // 0x028
    lq(vu, vf[10], vi[1] + 1);
    vf[18][Z] = vf[16][Z] + vf[15][X];               // 0x030 .. 0x048
    vf[19][Z] = vf[16][Z] + vf[15][W];
    vf[21][Z] = vf[16][Z] + vf[15][Y];
    vf[20][Z] = vf[16][Z] + vf[15][Z];
    lq(vu, vf[5], 5);
    lq(vu, vf[6], 6);
    lq(vu, vf[7], 7);
    lq(vu, vf[8], 8);
    vf[26][X] = vf[10][X] - vf[9][X];                // 0x050
    vf[26][Y] = vf[10][Y] - vf[9][Y];
    lq(vu, vf[11], vi[1] + 2);
    // 0x058-0x070: vf22 = matrix * (vf9.x, vf9.y, vf18.z); MOVEs fill vf19 / vf20 / vf21 / vf18 (x, y)
    {
        const float corner[3] = {vf[9][X], vf[9][Y], vf[18][Z]};
        vf[19][X] = vf[10][X], vf[19][Y] = vf[10][Y];
        vf[20][Y] = vf[9][Y];
        vf[20][X] = vf[10][X];
        transform(vu, vf[22], corner);
    }
    const float q1 = vf[0][W] / vf[11][X];           // 0x070 DIV (visible from 0x0A8)
    vf[21][X] = vf[9][X];                            // 0x078 .. 0x090: vf23 = matrix * vf19
    vf[21][Y] = vf[10][Y];
    {
        const float v[3] = {vf[19][X], vf[19][Y], vf[19][Z]};
        vf[18][X] = vf[9][X], vf[18][Y] = vf[9][Y];
        transform(vu, vf[23], v);
    }
    vu.clip = 0;                                     // 0x090 FCSET 0
    vf[30][W] = vf[15][W] - vf[15][Y];               // 0x098
    lq(vu, vf[12], vi[1] + 3);
    vf[30][Z] = vf[15][Z] - vf[15][X];               // 0x0A0
    lq(vu, vf[13], vi[1] + 4);
    vu.q = q1;
    vf[31][X] = vf[26][X] * vu.q;                    // 0x0A8
    lq(vu, vf[14], vi[1] + 5);
    setInt(vf[9][Z], ftoi0(vf[9][Z]));               // 0x0B0 FTOI0.zw vf9
    setInt(vf[9][W], ftoi0(vf[9][W]));
    vi[15] = 16384;
    setInt(vf[10][W], ftoi0(vf[10][W]));             // 0x0B8
    vi[15] = vi16(vi[15] + vi[15]);
    setInt(vf[11][Z], ftoi0(vf[11][Z]));             // 0x0C0
    setInt(vf[12][Z], ftoi0(vf[12][Z]));             // 0x0C8
    vf[30][Z] = vf[30][Z] * vu.q;                    // 0x0D0
    vf[30][W] = vf[30][W] * vu.q;
    vi[14] = vi16(getInt(vf[9][Z]));
    vi[5] = vi16(getInt(vf[10][W]));                 // 0x0D8 .. 0x0E8
    vi[11] = vi16(getInt(vf[11][Z]));
    vi[9] = vi16(getInt(vf[9][W]));
    {
        const float v[3] = {vf[20][X], vf[20][Y], vf[20][Z]};
        transform(vu, vf[24], v);                    // 0x0D8 .. 0x0F0: vf24 = matrix * vf20
    }
    const float q2 = vf[0][W] / vf[11][Y];           // 0x0F0 DIV (visible from 0x128)
    vi[7] = 0;                                       // 0x0F8 .. 0x110: vf25 = matrix * vf21
    vi[6] = vi16(vi[5] - 2);
    vi[8] = vi16(vi[11] + vi[11]);
    vi[8] = vi16(vi[8] + vi[11]);
    {
        const float v[3] = {vf[21][X], vf[21][Y], vf[21][Z]};
        transform(vu, vf[25], v);
    }
    vf[30][Y] = vf[15][Y] - vf[15][X];               // 0x118
    isw(vu, 1023, X, vi[15]);
    vf[30][W] = vf[30][W] - vf[30][Z];               // 0x120
    isw(vu, 1023, Y, vi[6]);
    vu.q = q2;
    vf[31][Y] = vf[26][Y] * vu.q;                    // 0x128
    isw(vu, 1023, Z, vi[8]);
    clipw(vu, vf[22], vf[22]);                       // 0x130 .. 0x148
    isw(vu, 1023, W, vi[7]);
    clipw(vu, vf[23], vf[23]);
    vi[3] = vi16(vi[1] + 8);
    clipw(vu, vf[24], vf[24]);
    vi[4] = vi16(vi[3] + vi[9]);
    clipw(vu, vf[25], vf[25]);
    vi[10] = vi16(vi[9] - 1);
    vf[30][Y] = vf[30][Y] * vu.q;                    // 0x150
    vf[30][W] = vf[30][W] * vu.q;
    vf[18][Z] = vf[17][Z] + vf[18][Z];               // 0x158 .. 0x170
    vf[19][Z] = vf[17][Z] + vf[19][Z];
    vf[21][Z] = vf[17][Z] + vf[21][Z];
    vf[20][Z] = vf[17][Z] + vf[20][Z];
    // 0x178 .. 0x1F0: the four corners again, raised by vf17.z
    vi[1] = (vu.clip & 0xFFFFFF) ? 1 : 0;            // 0x178 FCAND 0xFFFFFF
    vi[12] = vi[1];
    vi[1] = (vu.clip & 0x820820) ? 1 : 0;            // 0x188 FCAND 0x820820
    vi[13] = vi[1];
    vi[2] = vi16(vi[2] + 3);
    vi[15] = ilw(vu, 0, X);                          // 0x1A8
    {
        const float a[3] = {vf[18][X], vf[18][Y], vf[18][Z]};
        transform(vu, vf[22], a);
        const float b[3] = {vf[19][X], vf[19][Y], vf[19][Z]};
        transform(vu, vf[23], b);
        const float c[3] = {vf[20][X], vf[20][Y], vf[20][Z]};
        transform(vu, vf[24], c);
    }
    setInt(vf[15][Z], vi[10]);                       // 0x1D8 / 0x1E0 MFIR
    setInt(vf[15][Y], vi[10]);
    vi[7] = vi16(vi[2] + vi[8]);
    {
        const float d[3] = {vf[21][X], vf[21][Y], vf[21][Z]};
        transform(vu, vf[25], d);
    }
    clipw(vu, vf[22], vf[22]);                       // 0x1F8 .. 0x210
    clipw(vu, vf[23], vf[23]);
    clipw(vu, vf[24], vf[24]);
    clipw(vu, vf[25], vf[25]);
    for (int c = 0; c < 4; ++c)
        vf[16][c] = vf[27][c], vf[17][c] = vf[27][c], vf[18][c] = vf[27][c], vf[19][c] = vf[27][c];
    vf[9][Z] = vf[0][Z] + vf[15][X];                 // 0x218
    vu.i = -1.0f;
    vf[31][W] = vf[0][W] * vu.i;                     // 0x220
    for (int c = 0; c < 4; ++c)
        vf[20][c] = vf[27][c];
    vu.i = 2048.0f;                                  // 0x228
    vi[1] = (vu.clip & 0xFFFFFF) ? 1 : 0;            // 0x230
    for (int c = 0; c < 3; ++c)                      // 0x238 / 0x240
        vf[21][c] = vf[9][c] + vf[0][X], vf[22][c] = vf[9][c] + vf[0][X];
    vi[12] = vi16(vi[12] + vi[1]);
    vi[1] = (vu.clip & 0x820820) ? 1 : 0;
    vf[29][X] = vf[0][X] + vf[13][W];                // 0x248
    vi[13] = vi16(vi[13] + vi[1]);
    vf[29][Y] = vf[0][X] + vf[14][W];                // 0x250 (ADDw.y: vf0.y + vf14.w)
    vi[1] = vi16(int32_t(vu.top));
    vf[15][Y] = float(getInt(vf[15][Y]));            // 0x258 ITOF0.yz
    vf[15][Z] = float(getInt(vf[15][Z]));
    const bool other = vi[15] != 0;                  //       IBNE vi15, vi0, 0x1648
    vf[11][X] = vf[0][X] + vf[13][Z];                // 0x260 (delay slot)
    isw(vu, vi[1], X, vi[13]);
    vu.pc = other ? 0x1648 : 0x268;
}
