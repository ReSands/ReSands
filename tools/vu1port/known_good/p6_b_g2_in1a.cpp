// b_g2_in1a: Qwen's round16 (night 23-24 Oct) with VU operand/result normalisation, ISW as a 32-bit word store and
// MADDw reading ACC fixed by hand; passes its judge (tools/vu1port/port.py p6_b_g2_in1a --judge).
#include "vu_task.h"
#include <cstring>
#include <cstdint>
static inline float vu_norm_(float v){uint32_t b;std::memcpy(&b,&v,4);uint32_t e=b&0x7f800000u;if(!e)b&=0x80000000u;else if(e==0x7f800000u)b=(b&0x80000000u)|0x7f7fffffu;std::memcpy(&v,&b,4);return v;}
static inline float vu_max_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a>b?a:b;}
static inline float vu_min_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a<b?a:b;}
#include <cstring>
#include <cmath>
#include <cstdint>

namespace {
    void isw_w(VuState &vu, int32_t addr, int32_t v) { uint32_t w = uint32_t(v) & 0xFFFFu; std::memcpy(vu.mem + (addr & 0x3FF) * 16 + 12, &w, 4); }
    enum { X = 0, Y = 1, Z = 2, W = 3 };
    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }
    
    int32_t ftoi4_sat(float f) {
        // 2^31 / 16 = 134217728.0f. Clamp before multiplication to avoid overflow to infinity.
        if (f > 134217728.0f) return INT32_MAX;
        if (f < -134217728.0f) return INT32_MIN;
        return static_cast<int32_t>(f * 16.0f);
    }
}

void stage(VuState &vu)
{
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;

    // 0x1c40 ADDz.z vf18, vf21, vf27z | IADD vi8, vi2, vi6
    vf[18][Z] = vu_norm_(vu_norm_(vf[21][Z]) + vu_norm_(vf[27][Z]));
    vi[8] = vi16(vi[2] + vi[6]);

    // 0x1c48 ADD.xz vf21, vf21, vf31 | IADD vi9, vi7, vi6
    vf[21][X] += vf[31][X];
    vf[21][Z] += vf[31][Z];
    vi[9] = vi16(vi[7] + vi[6]);

    // 0x1c50 MULq.xyz vf16, vf16, Q | IADDIU vi5, vi5, 1   [Q = result of the DIV at 0x1c18 or 0x1d40]
    vf[16][X] *= vu.q;
    vf[16][Y] *= vu.q;
    vf[16][Z] *= vu.q;
    vi[5] = vi16(vi[5] + 1);

    // 0x1c58 MINIx.xyzw vf28, vf20, vf10x | DIV Q, vf0w, vf17w
    vf[28][X] = vu_min_(vf[20][X], vf[10][X]);
    vf[28][Y] = vu_min_(vf[20][Y], vf[10][X]);
    vf[28][Z] = vu_min_(vf[20][Z], vf[10][X]);
    vf[28][W] = vu_min_(vf[20][W], vf[10][X]);
    vu.q = vf[0][W] / vf[17][W];

    // 0x1c60 MULAx.xyzw ACC, vf1, vf18x | ISW.w vi12, 0(vi8)
    for (int c = 0; c < 4; ++c) vu.acc[c] = vf[1][c] * vf[18][X];
    isw_w(vu, vi[8], vi[12]);

    // 0x1c68 MADDAy.xyzw ACC, vf2, vf18y | ISW.w vi12, 0(vi9)
    for (int c = 0; c < 4; ++c) vu.acc[c] += vf[2][c] * vf[18][Y];
    isw_w(vu, vi[9], vi[12]);

    // 0x1c70 FTOI4.xyz vf16, vf16 | MOVE.xy vf19, vf21
    for (int c = 0; c < 3; ++c) {
        int32_t val = ftoi4_sat(vf[16][c]);
        std::memcpy(&vf[16][c], &val, 4);
    }
    vf[19][X] = vf[21][X];
    vf[19][Y] = vf[21][Y];

    // 0x1c78 MADDAz.xyzw ACC, vf3, vf18z | IADDIU vi12, vi0, 2
    for (int c = 0; c < 4; ++c) vu.acc[c] += vf[3][c] * vf[18][Z];
    vi[12] = vi16(vi[0] + 2);

    // 0x1c80 MADDw.xyzw vf18, vf4, vf0w | ISW.w vi0, 0(vi2)
    for (int c = 0; c < 4; ++c) vf[18][c] = vu_norm_(vu.acc[c] + vu_norm_(vf[4][c]) * 1.0f);
    isw_w(vu, vi[2], vi[0]);

    // 0x1c88 MAX.y vf28, vf28, vf10 | ISW.w vi0, 0(vi7)
    vf[28][Y] = vu_max_(vf[28][Y], vf[10][Y]);
    isw_w(vu, vi[7], vi[0]);

    // 0x1c90 ADDw.z vf19, vf21, vf27w | SQ.xyz vf16, 0(vi2)
    vf[19][Z] = vu_norm_(vu_norm_(vf[21][Z]) + vu_norm_(vf[27][W]));
    std::memcpy(vu.mem + (vi[2] & 0x3FF) * 16, vf[16], 12);

    vu.pc = 0x1c98;
}
