// a_g2_in3b: Qwen's round15 (night 23-24 Oct) with VU operand/result normalisation added by hand (denormal -> +-0,
// as the runtime's VU1 interpreter does); passes its judge (tools/vu1port/port.py p6_a_g2_in3b --judge).
#include "vu_task.h"
#include <cstring>
#include <cstdint>
static inline float vu_norm_(float v){uint32_t b;std::memcpy(&b,&v,4);uint32_t e=b&0x7f800000u;if(!e)b&=0x80000000u;else if(e==0x7f800000u)b=(b&0x80000000u)|0x7f7fffffu;std::memcpy(&v,&b,4);return v;}
static inline float vu_max_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a>b?a:b;}
static inline float vu_min_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a<b?a:b;}
#include <algorithm>
#include <cstring>

namespace {
    enum { X = 0, Y = 1, Z = 2, W = 3 };

    void update_mac(VuState &vu, const float *dst, int mask) {
        for (int c = 0; c < 4; ++c) {
            if (mask & (1 << c)) {
                float v = dst[c];
                vu.mac &= ~(1 << (3 - c));
                vu.mac &= ~(0x80 >> c);
                if (v == 0.0f) vu.mac |= (1 << (3 - c));
                if (v < 0.0f) vu.mac |= (0x80 >> c);
            } else {
                vu.mac &= ~(1 << (3 - c));
                vu.mac &= ~(0x80 >> c);
            }
        }
    }

    void store_qword_masked(VuState &vu, int32_t qword, const float *src, uint8_t mask) {
        uint32_t addr = uint32_t(qword) & 0x3FF;
        uint8_t *dst = vu.mem + 16 * addr;
        if (mask & 1) std::memcpy(dst + 0, &src[X], 4);
        if (mask & 2) std::memcpy(dst + 4, &src[Y], 4);
        if (mask & 4) std::memcpy(dst + 8, &src[Z], 4);
        if (mask & 8) std::memcpy(dst + 12, &src[W], 4);
    }
}

void stage(VuState &vu) {
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    float I = vu.i;

    // 0c30
    // MAXi.xyzw vf27, vf27, I
    for (int c = 0; c < 4; ++c) vf[27][c] = vu_max_(vf[27][c], I);
    update_mac(vu, vf[27], 0xF);
    // IADDIU vi6, vi0, 6
    vi[6] = vi[0] + 6;

    // 0c38
    // MULz.xy vf8, vf8, vf8z
    vf[8][X] *= vf[8][Z];
    vf[8][Y] *= vf[8][Z];
    update_mac(vu, vf[8], 0xC);
    // MOVE.xy vf16, vf21
    vf[16][X] = vf[21][X];
    vf[16][Y] = vf[21][Y];

    // 0c40
    // SUBw.y vf5, vf20, vf24w
    vf[5][Y] = vu_norm_(vu_norm_(vf[20][Y]) - vu_norm_(vf[24][W]));
    update_mac(vu, vf[5], 0x4);
    // SQ.xyz vf19, 14(vi8)
    store_qword_masked(vu, vi[8] + 14, vf[19], 0x7);

    // 0c48
    // MINIw.xyzw vf13, vf13, vf0w
    for (int c = 0; c < 4; ++c) vf[13][c] = vu_min_(vf[13][c], 1.0f);
    update_mac(vu, vf[13], 0xF);
    // SQ.xyz vf19, 14(vi9)
    store_qword_masked(vu, vi[9] + 14, vf[19], 0x7);

    // 0c50
    // SUB.xyzw vf28, vf20, vf26
    for (int c = 0; c < 4; ++c) vf[28][c] = vu_norm_(vu_norm_(vf[20][c]) - vu_norm_(vf[26][c]));
    update_mac(vu, vf[28], 0xF);
    // LOI 255
    I = 255.0f;

    // 0c58
    // ADDx.z vf16, vf21, vf27x
    vf[16][Z] = vf[21][Z] + vf[27][X];
    update_mac(vu, vf[16], 0x2);
    // SQ.xyz vf8, 12(vi8)
    store_qword_masked(vu, vi[8] + 12, vf[8], 0x7);

    // 0c60
    // ADD.xz vf21, vf21, vf31
    vf[21][X] += vf[31][X];
    vf[21][Z] += vf[31][Z];
    update_mac(vu, vf[21], 0xA);
    // SQ.xyz vf8, 12(vi9)
    store_qword_masked(vu, vi[9] + 12, vf[8], 0x7);

    // 0c68
    // ADDAi.xyzw ACC, vf14, I  [reads old I]
    float I_old = I;
    for (int c = 0; c < 4; ++c) vu.acc[c] = vf[14][c] + I_old;
    update_mac(vu, vu.acc, 0xF);
    // LOI 60
    I = 60.0f;

    // 0c70
    // MSUBi.xyzw vf13, vf13, I
    for (int c = 0; c < 4; ++c) vf[13][c] = vu.acc[c] - vf[13][c] * I;
    update_mac(vu, vf[13], 0xF);
    // NOP

    // 0c78
    // ADDy.x vf5, vf0, vf5y
    vf[5][X] = vf[5][Y];
    update_mac(vu, vf[5], 0x8);
    // NOP

    // 0c80
    // ADDx.y vf5, vf0, vf28x
    vf[5][Y] = vf[28][X];
    update_mac(vu, vf[5], 0x4);
    // NOP

    // 0c88
    // SUBx.z vf6, vf20, vf20x
    vf[6][Z] = vu_norm_(vu_norm_(vf[20][Z]) - vu_norm_(vf[20][X]));
    update_mac(vu, vf[6], 0x2);
    // IBNE vi5, vi14, 0x9e0
    bool branch_taken = (vi[5] != vi[14]);

    // 0c90 (delay slot)
    // FTOI4.xyzw vf13, vf13
    for (int c = 0; c < 4; ++c) {
        float f = vf[13][c] * 16.0f;
        int32_t i;
        if (f > 2147483647.0f) i = 2147483647;
        else if (f < -2147483648.0f) i = -2147483648;
        else i = static_cast<int32_t>(f);
        std::memcpy(&vf[13][c], &i, 4);
    }
    // NOP

    vu.i = I;
    vu.pc = branch_taken ? 0x9e0 : 0xc98;
}
