// a_g2_in3a: Qwen's round15 (night 23-24 Oct) with VU operand/result normalisation added by hand (denormal -> +-0,
// as the runtime's VU1 interpreter does); passes its judge (tools/vu1port/port.py p6_a_g2_in3a --judge).
#include "vu_task.h"

#include <algorithm>
#include <cstring>

namespace {
    enum { X = 0, Y = 1, Z = 2, W = 3 };

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

    void store_qword_masked(VuState &vu, int32_t qword, const float *src, uint8_t mask) {
        uint32_t addr = uint32_t(qword) & 0x3FF;
        uint8_t *dst = vu.mem + 16 * addr;
        if (mask & 1) std::memcpy(dst + 0, &src[X], 4);
        if (mask & 2) std::memcpy(dst + 4, &src[Y], 4);
        if (mask & 4) std::memcpy(dst + 8, &src[Z], 4);
        if (mask & 8) std::memcpy(dst + 12, &src[W], 4);
    }

    int32_t ftoi4(float f) {
        f *= 16.0f;
        if (f > 2147483647.0f) return 2147483647;
        if (f < -2147483648.0f) return -2147483648;
        return static_cast<int32_t>(f);
    }
}

void stage(VuState &vu) {
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;

    // Q pipeline delay: instructions reading Q in this stage see the value at entry
    float q_entry = vu.q;

    // 0bd8
    vf[8][X] = vu.acc[X] + vf[19][X] * q_entry;
    vf[8][Y] = vu.acc[Y] + vf[19][Y] * q_entry;
    update_mac(vu, vf[8], 0x3);
    vi[7] = vi[9] + 12;

    // 0be0
    vf[7][X] = vf[7][X] * vf[7][Z];
    vf[7][Y] = vf[7][Y] * vf[7][Z];
    update_mac(vu, vf[7], 0x3);
    vi[2] = vi[2] + vi[6];

    // 0be8
    vf[19][X] = vf[19][X] * q_entry;
    vf[19][Y] = vf[19][Y] * q_entry;
    vf[19][Z] = vf[19][Z] * q_entry;
    update_mac(vu, vf[19], 0x7);
    vi[7] = vi[7] + vi[6];

    // 0bf0
    {
        int32_t tmp = ftoi4(vf[18][X]); std::memcpy(&vf[18][X], &tmp, 4);
        tmp = ftoi4(vf[18][Y]); std::memcpy(&vf[18][Y], &tmp, 4);
        tmp = ftoi4(vf[18][Z]); std::memcpy(&vf[18][Z], &tmp, 4);
    }
    vu.i = 0.15f;

    // 0bf8
    vf[8][X] = vf[8][X] * vf[12][X];
    vf[8][Y] = vf[8][Y] * vf[12][Y];
    update_mac(vu, vf[8], 0x3);
    vi[5] = vi[5] + 1;

    // 0c00
    vf[8][Z] = q_entry;
    update_mac(vu, vf[8], 0x4);
    store_qword_masked(vu, vi[8] + 6, vf[7], 0x7);

    // 0c08
    for(int c=0;c<4;c++){ uint32_t b; std::memcpy(&b,&vf[20][c],4); if(!(b&0x7f800000)) b&=0x80000000u; float v; std::memcpy(&v,&b,4); vf[13][c]=v>0.0f?v:0.0f; vf[27][c]=v<vu.i?v:vu.i; }
    
    
    
    update_mac(vu, vf[13], 0xF);
    store_qword_masked(vu, vi[9] + 6, vf[7], 0x7);

    // 0c10
    
    
    
    
    update_mac(vu, vf[27], 0xF);
    store_qword_masked(vu, vi[8] + 8, vf[18], 0x7);

    // 0c18
    vf[14][X] = 0.0f;
    vf[14][Y] = 0.0f;
    vf[14][Z] = 0.0f;
    vf[14][W] = 0.0f;
    update_mac(vu, vf[14], 0xF);
    store_qword_masked(vu, vi[9] + 8, vf[18], 0x7);

    // 0c20
    {
        int32_t tmp = ftoi4(vf[19][X]); std::memcpy(&vf[19][X], &tmp, 4);
        tmp = ftoi4(vf[19][Y]); std::memcpy(&vf[19][Y], &tmp, 4);
        tmp = ftoi4(vf[19][Z]); std::memcpy(&vf[19][Z], &tmp, 4);
    }
    vu.i = -0.15f;

    // 0c28
    vf[13][X] = vf[13][X] + vf[13][X];
    vf[13][Y] = vf[13][Y] + vf[13][Y];
    vf[13][Z] = vf[13][Z] + vf[13][Z];
    vf[13][W] = vf[13][W] + vf[13][W];
    update_mac(vu, vf[13], 0xF);
    vi[14] = vi[13] + 1;

    vu.pc = 0xc30;
}
