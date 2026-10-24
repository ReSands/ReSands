// b_g1_in1b: Qwen's round17 (night 23-24 Oct) with VU operand/result normalisation added by hand (denormal -> +-0,
// as the runtime's VU1 interpreter does); passes its judge (tools/vu1port/port.py p6_b_g1_in1b --judge).
#include "vu_task.h"
#include <cstring>
#include <cstdint>
static inline float vu_norm_(float v){uint32_t b;std::memcpy(&b,&v,4);uint32_t e=b&0x7f800000u;if(!e)b&=0x80000000u;else if(e==0x7f800000u)b=(b&0x80000000u)|0x7f7fffffu;std::memcpy(&v,&b,4);return v;}
static inline float vu_max_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a>b?a:b;}
static inline float vu_min_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a<b?a:b;}
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {
    constexpr int X = 0, Y = 1, Z = 2, W = 3;

    inline int32_t i16(int32_t v) { return int32_t(int16_t(v)); }

    inline void isw(VuState& vu, int32_t viT, int32_t viS, int32_t imm, int lane) {
        uint32_t addr = (vu.vi[viS] + imm) & 0x3FF;
        uint8_t* p = vu.mem + 16 * addr + 4 * lane;
        uint16_t val = static_cast<uint16_t>(viT);
        uint32_t word = static_cast<uint32_t>(val);
        std::memcpy(p, &word, 4);
    }

    inline void sq(VuState& vu, const float* src, int32_t viS, int32_t imm, int mask) {
        uint32_t addr = (vu.vi[viS] + imm) & 0x3FF;
        uint8_t* p = vu.mem + 16 * addr;
        if (mask == 0x0F) {
            std::memcpy(p, src, 16);
        } else if (mask == 0x07) {
            std::memcpy(p, src, 12);
        }
    }

    inline float ftoi4(float v) {
        float scaled = v * 16.0f;
        int32_t val;
        if (scaled > 2147483647.0f) val = 0x7FFFFFFF;
        else if (scaled < -2147483648.0f) val = 0x80000000;
        else val = static_cast<int32_t>(scaled);
        return *reinterpret_cast<float*>(&val);
    }
}

void stage(VuState& vu) {
    float (*vf)[4] = vu.vf;
    int32_t* vi = vu.vi;

    // 17f0: MULq.xyz vf10, vf10, Q | SQ.xyzw vf17, 200(vi9)
    vf[10][X] *= vu.q;
    vf[10][Y] *= vu.q;
    vf[10][Z] *= vu.q;
    sq(vu, vf[17], 9, 200, 0x0F);

    // 17f8: SUB.z vf0, vf23, vf15 | MOVE.xy vf19, vf21
    {
        float sub_res = vf[23][Z] - vf[15][Z];
        vu.mac &= ~(2 | 0x20);
        if (sub_res == 0.0f) vu.mac |= 2;
        if (sub_res < 0.0f) vu.mac |= 0x20;
    }
    vf[19][X] = vf[21][X];
    vf[19][Y] = vf[21][Y];

    // 1800: ADDw.z vf19, vf21, vf27w | ISW.w vi0, 4(vi8)
    vf[19][Z] = vf[21][Z] + vf[27][W];
    isw(vu, vi[0], 8, 4, W);

    // 1808: MULAx.xyzw ACC, vf5, vf18x | DIV Q, vf0w, vf11w
    for (int c = 0; c < 4; ++c) vu.acc[c] = vf[5][c] * vf[18][X];
    vu.q = vf[0][W] / vf[11][W];

    // 1810: FTOI4.xyz vf10, vf10 | LQI.xyzw vf20, (vi4++)
    vf[10][X] = ftoi4(vf[10][X]);
    vf[10][Y] = ftoi4(vf[10][Y]);
    vf[10][Z] = ftoi4(vf[10][Z]);
    {
        uint32_t addr = vu.vi[4] & 0x3FF;
        std::memcpy(vf[20], vu.mem + 16 * addr, 16);
        vu.vi[4] += 1;
    }

    // 1818: MADDAy.xyzw ACC, vf6, vf18y | FMAND vi6, vi12
    for (int c = 0; c < 4; ++c) vu.acc[c] += vf[6][c] * vf[18][Y];
    vi[6] = vu.mac & vi[12];

    // 1820: MADDAz.xyzw ACC, vf7, vf18z | ISW.w vi0, 4(vi9)
    for (int c = 0; c < 4; ++c) vu.acc[c] += vf[7][c] * vf[18][Z];
    isw(vu, vi[0], 9, 4, W);

    // 1828: MADDw.xyzw vf18, vf8, vf0w | LOI 0.15
    for (int c = 0; c < 4; ++c) vf[18][c] = vu.acc[c] + vf[8][c] * vf[0][W];
    vu.i = 0.15f;

    // 1830: ADD.xz vf21, vf21, vf31 | SQ.xyz vf10, 0(vi8)
    vf[21][X] += vf[31][X];
    vf[21][Z] += vf[31][Z];
    sq(vu, vf[10], 8, 0, 0x07);

    // 1838: MINIi.xyzw vf27, vf20, I | LOI -0.15
    for (int c = 0; c < 4; ++c) vf[27][c] = vu_min_(vf[20][c], vu.i);
    vu.i = -0.15f;

    vu.pc = 0x1840;
}
