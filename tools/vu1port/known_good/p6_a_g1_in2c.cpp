// a_g1_in2c: Qwen's round24 (night 23-24 Oct) with VU operand/result normalisation added by hand (denormal -> +-0,
// as the runtime's VU1 interpreter does), SQ address registers and FTOI4 bit stores fixed; passes its judge (tools/vu1port/port.py p6_a_g1_in2c --judge).
#include "vu_task.h"
#include <cstring>
#include <cstdint>
#include <climits>
static inline float vu_norm_(float v){uint32_t b;std::memcpy(&b,&v,4);uint32_t e=b&0x7f800000u;if(!e)b&=0x80000000u;else if(e==0x7f800000u)b=(b&0x80000000u)|0x7f7fffffu;std::memcpy(&v,&b,4);return v;}
static inline float vu_max_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a>b?a:b;}
static inline float vu_min_(float a,float b){a=vu_norm_(a);b=vu_norm_(b);return a<b?a:b;}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>

namespace {
    enum { X = 0, Y = 1, Z = 2, W = 3 };

    inline void update_mac(uint32_t &mac, int mask, const float *vals) {
        mac &= ~(0xFF);
        if (mask & 1) { // x
            mac |= (vals[X] == 0.0f ? 8 : 0) | (vals[X] < 0.0f ? 0x80 : 0);
        }
        if (mask & 2) { // y
            mac |= (vals[Y] == 0.0f ? 4 : 0) | (vals[Y] < 0.0f ? 0x40 : 0);
        }
        if (mask & 4) { // z
            mac |= (vals[Z] == 0.0f ? 2 : 0) | (vals[Z] < 0.0f ? 0x20 : 0);
        }
        if (mask & 8) { // w
            mac |= (vals[W] == 0.0f ? 1 : 0) | (vals[W] < 0.0f ? 0x10 : 0);
        }
    }

    inline void sq_xyz(VuState &vu, int s, int t, int addr_imm) {
        uint32_t qaddr = (vu.vi[t] + addr_imm) & 0x3FF;
        std::memcpy(vu.mem + 16 * qaddr, vu.vf[s], 12);
    }
}

void stage(VuState &vu) {
    float (*vf)[4] = vu.vf;
    int32_t *vi = vu.vi;
    uint32_t &mac = vu.mac;
    float &acc0 = vu.acc[0];
    float &acc1 = vu.acc[1];
    float &acc2 = vu.acc[2];
    float &acc3 = vu.acc[3];

    // 0x4f8
    // MADDq.xy vf8, vf19, Q
    vf[8][X] = acc0 + vf[19][X] * vu.q;
    vf[8][Y] = acc1 + vf[19][Y] * vu.q;
    update_mac(mac, 1|2, vf[8]);
    // IADDIU vi14, vi13, 1
    vi[14] = int32_t(int16_t(vi[13] + 1));

    // 0x500
    // MAXx.xyzw vf13, vf20, vf0x
    { float b = 0.0f; vf[13][X] = vu_max_(vf[20][X], b); vf[13][Y] = vu_max_(vf[20][Y], b); vf[13][Z] = vu_max_(vf[20][Z], b); vf[13][W] = vu_max_(vf[20][W], b); }
    update_mac(mac, 1|2|4|8, vf[13]);
    // SQ.xyz vf7, 6(vi8)
    sq_xyz(vu, 7, 8, 6);

    // 0x508
    // MINIi.xyzw vf27, vf20, I
    { float b = vu.i; vf[27][X] = vu_min_(vf[20][X], b); vf[27][Y] = vu_min_(vf[20][Y], b); vf[27][Z] = vu_min_(vf[20][Z], b); vf[27][W] = vu_min_(vf[20][W], b); }
    update_mac(mac, 1|2|4|8, vf[27]);
    // SQ.xyz vf7, 6(vi9)
    sq_xyz(vu, 7, 9, 6);

    // 0x510
    // ADDq.z vf8, vf0, Q
    vf[8][Z] = 0.0f + vu.q;
    update_mac(mac, 4, vf[8]);
    // LOI -0.15
    vu.i = -0.15f;

    // 0x518
    // MUL.xy vf8, vf8, vf12
    vf[8][X] *= vf[12][X];
    vf[8][Y] *= vf[12][Y];
    update_mac(mac, 1|2, vf[8]);
    // IADDIU vi5, vi5, 1
    vi[5] = int32_t(int16_t(vi[5] + 1));

    // 0x520
    // ADD.xyzw vf13, vf13, vf13
    vf[13][X] += vf[13][X]; vf[13][Y] += vf[13][Y]; vf[13][Z] += vf[13][Z]; vf[13][W] += vf[13][W];
    update_mac(mac, 1|2|4|8, vf[13]);
    // IADD vi2, vi2, vi6
    vi[2] = int32_t(int16_t(vi[2] + vi[6]));

    // 0x528
    // MAXi.xyzw vf27, vf27, I
    { float b = vu.i; vf[27][X] = vu_max_(vf[27][X], b); vf[27][Y] = vu_max_(vf[27][Y], b); vf[27][Z] = vu_max_(vf[27][Z], b); vf[27][W] = vu_max_(vf[27][W], b); }
    update_mac(mac, 1|2|4|8, vf[27]);
    // IADD vi7, vi7, vi6
    vi[7] = int32_t(int16_t(vi[7] + vi[6]));

    // 0x530
    // SUB.xyzw vf14, vf14, vf14
    vf[14][X] = 0.0f; vf[14][Y] = 0.0f; vf[14][Z] = 0.0f; vf[14][W] = 0.0f;
    update_mac(mac, 1|2|4|8, vf[14]);
    // IADDIU vi6, vi0, 6
    vi[6] = int32_t(int16_t(vi[0] + 6));

    // 0x538
    // SUB.xyzw vf28, vf20, vf26
    for (int c = 0; c < 4; c++) vf[28][c] = vu_norm_(vu_norm_(vf[20][c]) - vu_norm_(vf[26][c]));
    update_mac(mac, 1|2|4|8, vf[28]);
    // SQ.xyz vf8, 12(vi8)
    sq_xyz(vu, 8, 8, 12);

    // 0x540
    // MINIw.xyzw vf13, vf13, vf0w
    { float b = 1.0f; vf[13][X] = vu_min_(vf[13][X], b); vf[13][Y] = vu_min_(vf[13][Y], b); vf[13][Z] = vu_min_(vf[13][Z], b); vf[13][W] = vu_min_(vf[13][W], b); }
    update_mac(mac, 1|2|4|8, vf[13]);
    // SQ.xyz vf8, 12(vi9)
    sq_xyz(vu, 8, 9, 12);

    // 0x548
    // SUBw.y vf5, vf20, vf24w
    vf[5][Y] = vu_norm_(vu_norm_(vf[20][Y]) - vu_norm_(vf[24][W]));
    update_mac(mac, 2, vf[5]);
    // MOVE.xy vf16, vf21
    vf[16][X] = vf[21][X]; vf[16][Y] = vf[21][Y];
    update_mac(mac, 1|2, vf[16]);

    // 0x550
    // ADDx.z vf16, vf21, vf27x
    vf[16][Z] = vf[21][Z] + vf[27][X];
    update_mac(mac, 4, vf[16]);
    // LOI 255
    vu.i = 255.0f;

    // 0x558
    // ADDAi.xyzw ACC, vf14, I
    acc0 = vf[14][X] + vu.i;
    acc1 = vf[14][Y] + vu.i;
    acc2 = vf[14][Z] + vu.i;
    acc3 = vf[14][W] + vu.i;
    update_mac(mac, 1|2|4|8, vu.acc);
    // LOI 60
    vu.i = 60.0f;

    // 0x560
    // MSUBi.xyzw vf13, vf13, I
    vf[13][X] = acc0 - vf[13][X] * vu.i;
    vf[13][Y] = acc1 - vf[13][Y] * vu.i;
    vf[13][Z] = acc2 - vf[13][Z] * vu.i;
    vf[13][W] = acc3 - vf[13][W] * vu.i;
    update_mac(mac, 1|2|4|8, vf[13]);
    // NOP

    // 0x568
    // ADD.xz vf21, vf21, vf31
    vf[21][X] += vf[31][X];
    vf[21][Z] += vf[31][Z];
    update_mac(mac, 1|4, vf[21]);
    // NOP

    // 0x570
    // ADDy.x vf5, vf0, vf5y
    vf[5][X] = 0.0f + vf[5][Y];
    update_mac(mac, 1, vf[5]);
    // NOP

    // 0x578
    // SUBx.z vf6, vf20, vf20x
    vf[6][Z] = vu_norm_(vu_norm_(vf[20][Z]) - vu_norm_(vf[20][X]));
    update_mac(mac, 4, vf[6]);
    // NOP

    // 0x580
    // ADDx.y vf5, vf0, vf28x
    vf[5][Y] = 0.0f + vf[28][X];
    update_mac(mac, 2, vf[5]);
    // IBNE vi5, vi14, 0x338
    if (vi[5] != vi[14]) {
        vu.pc = 0x338;
    } else {
        vu.pc = 0x590;
    }

    // 0x588
    // FTOI4.xyzw vf13, vf13
    {
        float temp;
        int32_t val;
        temp = vf[13][X] * 16.0f; val = temp >= 2147483648.0f ? INT32_MAX : temp <= -2147483648.0f ? INT32_MIN : (int32_t)temp; std::memcpy(&vf[13][X], &val, 4);
        temp = vf[13][Y] * 16.0f; val = temp >= 2147483648.0f ? INT32_MAX : temp <= -2147483648.0f ? INT32_MIN : (int32_t)temp; std::memcpy(&vf[13][Y], &val, 4);
        temp = vf[13][Z] * 16.0f; val = temp >= 2147483648.0f ? INT32_MAX : temp <= -2147483648.0f ? INT32_MIN : (int32_t)temp; std::memcpy(&vf[13][Z], &val, 4);
        temp = vf[13][W] * 16.0f; val = temp >= 2147483648.0f ? INT32_MAX : temp <= -2147483648.0f ? INT32_MIN : (int32_t)temp; std::memcpy(&vf[13][W], &val, 4);
    }
    // NOP
}
