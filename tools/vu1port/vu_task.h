// VU1 program port API (tools/vu1port/): a stage of a VU1 microprogram as plain C++. The judge
// (judge.cpp) loads cases recorded by vu1replay --stage (the runtime's VU1 interpreter replaying
// PS2X_VU1_TRACE runs), calls stage() on each case's input and compares with the interpreter's output.
#pragma once
#include <cstdint>
#include <vector>

// The VU1 state a stage reads and writes, in architectural terms: every earlier write has landed
// (stages start and end where no write is pending).
struct VuState
{
    float vf[32][4];    // vf00 is always (0, 0, 0, 1); x, y, z, w = [0], [1], [2], [3]
    int32_t vi[16];     // 16-bit integer registers, kept sign-extended to 32 bits; vi00 is always 0
    float acc[4];       // ACC
    float q, p, i;      // Q (DIV/SQRT/RSQRT), P (EFU), I (LOI)
    uint32_t mac;       // MAC flags as last visible (bits 0-3 zero w,z,y,x; 4-7 sign; 8-11 underflow; 12-15 overflow)
    uint32_t clip;      // clip flags (24 bits: the last four CLIP results, 6 bits each, newest in bits 0-5)
    uint32_t status;    // status flags
    uint32_t top;       // what XTOP returns
    uint32_t pc;        // byte address: the stage's entry on input; set it to the exit taken on return
    uint8_t mem[0x4000]; // VU1 data memory: 1024 qwords of 16 bytes; LQ/SQ address qword n at mem[16 n]
};

// XGKICK: called with the qword address the program kicks (the vi register's value); the harness copies
// the GIF packet that starts there.
void xgkick(VuState &vu, uint32_t qwordAddress);

// The port: runs the stage from vu.pc (its entry) until it reaches one of the stage's exits, sets vu.pc.
void stage(VuState &vu);
