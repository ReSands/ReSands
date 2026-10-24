// a_g2_in2a: Qwen's round20 (night 23-24 Oct) with VU operand/result normalisation added by hand (denormal -> +-0,
// as the runtime's VU1 interpreter does); passes its judge (tools/vu1port/port.py p6_a_g2_in2a --judge).
#include "vu_task.h"
#include <cstring>
#include <cstdint>
static inline float vu_norm_(float v){uint32_t b;std::memcpy(&b,&v,4);uint32_t e=b&0x7f800000u;if(!e)b&=0x80000000u;else if(e==0x7f800000u)b=(b&0x80000000u)|0x7f7fffffu;std::memcpy(&v,&b,4);return v;}
static inline float vu_max_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a>b?a:b;}
static inline float vu_min_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a<b?a:b;}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
    enum { X = 0, Y = 1, Z = 2, W = 3 };

    int32_t vi16(int32_t v) { return int32_t(int16_t(v)); }
    
    int32_t ftoi4(float f) {
        if (!(f == f)) return 0;
        f *= 16.0f;
        if (f >= 2147483647.0f) return 0x7FFFFFFF;
        if (f <= -2147483648.0f) return int32_t(0x80000000u);
        return int32_t(f);
    }
    
    void setInt(float &lane, int32_t v) { std::memcpy(&lane, &v, 4); }
    
    int32_t low16(float f) {
        int32_t v;
        std::memcpy(&v, &f, 4);
        return v & 0xFFFF;
    }

    void store_qword(VuState &vu, int32_t qword, const float *src) {
        uint32_t addr = uint32_t(qword) & 0x3FF;
        std::memcpy(vu.mem + 16 * addr, src, 16);
    }

    void store_qword_masked(VuState &vu, int32_t qword, const float *src, uint8_t mask) {
        uint32_t addr = uint32_t(qword) & 0x3FF;
        uint8_t *dst = vu.mem + 16 * addr;
        if (mask & 1) std::memcpy(dst + 0, &src[X], 4);
        if (mask & 2) std::memcpy(dst + 4, &src[Y], 4);
        if (mask & 4) std::memcpy(dst + 8, &src[Z], 4);
        if (mask & 8) std::memcpy(dst + 12, &src[W], 4);
    }

    void store_word(VuState &vu, int32_t qword, int lane, int32_t val) {
        uint32_t addr = uint32_t(qword) & 0x3FF;
        uint8_t *dst = vu.mem + 16 * addr + 4 * lane;
        uint32_t v32 = val & 0xFFFF;
        std::memcpy(dst, &v32, 4);
    }

    void update_mac(VuState &vu, const float *dst, int mask) {
        for (int c = 0; c < 4; ++c) {
            if (mask & (1 << c)) {
                float v = dst[c];
                if (v == 0.0f) vu.mac |= (1 << (3 - c));
                else vu.mac &= ~(1 << (3 - c));
                if (v < 0.0f) vu.mac |= (0x80 >> (3 - c));
                else vu.mac &= ~(0x80 >> (3 - c));
            } else {
                vu.mac &= ~(1 << (3 - c));
                vu.mac &= ~(0x80 >> (3 - c));
            }
        }
    }
}

void stage(VuState &vu) {
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;

    // 0xae8
    // MADDw.xyzw vf18, vf4, vf0w
    vf[18][X] = vu.acc[X] + vf[4][X] * vf[0][W];
    vf[18][Y] = vu.acc[Y] + vf[4][Y] * vf[0][W];
    vf[18][Z] = vu.acc[Z] + vf[4][Z] * vf[0][W];
    vf[18][W] = vu.acc[W] + vf[4][W] * vf[0][W];
    update_mac(vu, vf[18], 0xF);

    // ISW.w vi10, 8(vi9)
    store_word(vu, vi[9] + 8, W, vi[10]);

    // 0xaf0
    // MULAx.xy ACC, vf6, vf10x
    vu.acc[X] = vf[6][X] * vf[10][X];
    vu.acc[Y] = vf[6][Y] * vf[10][X];
    update_mac(vu, vu.acc, 0x3);

    // SQ.xyz vf5, 0(vi2)
    store_qword_masked(vu, vi[2] + 0, vf[5], 0x7);

    // 0xaf8
    // MADDAq.xy ACC, vf17, Q
    vu.acc[X] += vf[17][X] * vu.q;
    vu.acc[Y] += vf[17][Y] * vu.q;
    update_mac(vu, vu.acc, 0x3);

    // SQ.xyz vf5, 0(vi7)
    store_qword_masked(vu, vi[7] + 0, vf[5], 0x7);

    // 0xb00
    // MADDw.xy vf6, vf29, vf0w
    vf[6][X] = vu.acc[X] + vf[29][X] * vf[0][W];
    vf[6][Y] = vu.acc[Y] + vf[29][Y] * vf[0][W];
    update_mac(vu, vf[6], 0x3);

    // MOVE.xy vf19, vf21
    vf[19][X] = vf[21][X];
    vf[19][Y] = vf[21][Y];
    update_mac(vu, vf[19], 0x3);

    // 0xb08
    // MULq.xyz vf17, vf17, Q
    vf[17][X] *= vu.q;
    vf[17][Y] *= vu.q;
    vf[17][Z] *= vu.q;
    update_mac(vu, vf[17], 0x7);

    // NOP

    // 0xb10
    // ADDq.z vf6, vf0, Q
    vf[6][Z] = vu.q;
    update_mac(vu, vf[6], 0x4);

    // NOP

    // 0xb18
    // SUB.yz vf0, vf23, vf15
    {
        float sub_y = vf[23][Y] - vf[15][Y];
        float sub_z = vf[23][Z] - vf[15][Z];
        float temp_vf0[4] = {0.0f, sub_y, sub_z, 0.0f};
        update_mac(vu, temp_vf0, 0x6); // y and z
    }
    uint32_t mac_b18 = vu.mac; // Save MAC flags for FMAND

    // IADDIU vi12, vi0, 6
    vi[12] = vi[0] + 6;

    // 0xb20
    // ADDw.z vf19, vf21, vf27w
    vf[19][Z] = vf[21][Z] + vf[27][W];
    update_mac(vu, vf[19], 0x4);

    // NOP

    // 0xb28
    // MAXw.xy vf7, vf7, vf31w
    vf[7][X] = vu_max_(vf[7][X], vf[31][W]);
    vf[7][Y] = vu_max_(vf[7][Y], vf[31][W]);
    update_mac(vu, vf[7], 0x3);

    // DIV Q, vf0w, vf18w
    vu.q = vf[0][W] / vf[18][W];

    // 0xb30
    // ADD.xz vf21, vf21, vf31
    vf[21][X] += vf[31][X];
    vf[21][Z] += vf[31][Z];
    update_mac(vu, vf[21], 0x5);

    // MTIR vi10, vf13w
    vi[10] = low16(vf[13][W]);

    // 0xb38
    // FTOI4.xyz vf17, vf17
    setInt(vf[17][X], ftoi4(vf[17][X]));
    setInt(vf[17][Y], ftoi4(vf[17][Y]));
    setInt(vf[17][Z], ftoi4(vf[17][Z]));
    update_mac(vu, vf[17], 0x7);

    // FMAND vi6, vi12 [MAC flags of the upper instruction at 0xb18]
    vi[6] = mac_b18 & vi[12];

    // 0xb40
    // MUL.xy vf6, vf6, vf12
    vf[6][X] *= vf[12][X];
    vf[6][Y] *= vf[12][Y];
    update_mac(vu, vf[6], 0x3);

    // ISUB vi6, vi12, vi6
    vi[6] = vi[12] - vi[6];

    // 0xb48
    // SUBz.x vf8, vf25, vf20z
    vf[8][X] = vu_norm_(vu_norm_(vf[25][X]) - vu_norm_(vf[20][Z]));
    update_mac(vu, vf[8], 0x1);

    // NOP

    // 0xb50
    // ADDw.y vf8, vf0, vf28w
    vf[8][Y] = vu_norm_(vf[28][W]); // vf[0][Y] is always 0
    update_mac(vu, vf[8], 0x2);

    // ISW.w vi10, 14(vi8)
    store_word(vu, vi[8] + 14, W, vi[10]);

    // 0xb58
    // MULAx.xyzw ACC, vf1, vf19x
    vu.acc[X] = vf[1][X] * vf[19][X];
    vu.acc[Y] = vf[1][Y] * vf[19][X];
    vu.acc[Z] = vf[1][Z] * vf[19][X];
    vu.acc[W] = vf[1][W] * vf[19][X];
    update_mac(vu, vu.acc, 0xF);

    // ISW.w vi10, 14(vi9)
    store_word(vu, vi[9] + 14, W, vi[10]);

    vu.pc = 0xb60;
}
