// Writes PS2Recomp's own translation of every EE FPU (COP1) instruction as test functions
// (fpu_cases.inc) for tools/mmitest/fpuharness.cpp, which compares them with a reference model.
// Build: see tools/mmitest/run.sh. Usage: fpugen <out.inc>
#include "ps2recomp/code_generator.h"
#include "ps2recomp/r5900_decoder.h"

#include <cstdio>
#include <string>

using namespace ps2recomp;

namespace
{
    struct Op
    {
        const char *name;
        uint32_t fmt; // 0x10 = S, 0x14 = W
        uint32_t funct;
    };

    // fs = f1, ft = f2, fd = f3 (as the harness sets them up).
    constexpr uint32_t kFs = 1, kFt = 2, kFd = 3;

    // The EE's COP1 instruction set (single precision only, plus CVT.S.W).
    const Op kOps[] = {
        {"ADD_S", 0x10, 0x00}, {"SUB_S", 0x10, 0x01}, {"MUL_S", 0x10, 0x02}, {"DIV_S", 0x10, 0x03},
        {"SQRT_S", 0x10, 0x04}, {"ABS_S", 0x10, 0x05}, {"MOV_S", 0x10, 0x06}, {"NEG_S", 0x10, 0x07},
        {"RSQRT_S", 0x10, 0x16}, {"ADDA_S", 0x10, 0x18}, {"SUBA_S", 0x10, 0x19}, {"MULA_S", 0x10, 0x1A},
        {"MADD_S", 0x10, 0x1C}, {"MSUB_S", 0x10, 0x1D}, {"MADDA_S", 0x10, 0x1E}, {"MSUBA_S", 0x10, 0x1F},
        {"CVT_W_S", 0x10, 0x24}, {"MAX_S", 0x10, 0x28}, {"MIN_S", 0x10, 0x29},
        {"C_F_S", 0x10, 0x30}, {"C_EQ_S", 0x10, 0x32}, {"C_LT_S", 0x10, 0x34}, {"C_LE_S", 0x10, 0x36},
        {"CVT_S_W", 0x14, 0x20},
    };
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: fpugen <out.inc>\n");
        return 2;
    }
    R5900Decoder decoder;
    CodeGenerator gen({}, {});
    FILE *out = std::fopen(argv[1], "w");
    std::string table;
    for (const Op &op : kOps)
    {
        const uint32_t raw = (0x11u << 26) | (op.fmt << 21) | (kFt << 16) | (kFs << 11) | (kFd << 6) | op.funct;
        const Instruction inst = decoder.decodeInstruction(0x100000u, raw, false);
        const std::string code = gen.translateInstruction(inst);
        std::fprintf(out, "// %s (%08x)\nstatic void t_%s(R5900Context *ctx, uint8_t *rdram, PS2Runtime *runtime)\n{\n    (void)rdram; (void)runtime;\n    %s\n}\n\n",
                     op.name, raw, op.name, code.c_str());
        table += std::string("    {\"") + op.name + "\", t_" + op.name + "},\n";
    }
    std::fprintf(out, "static const TestCase kCases[] = {\n%s};\n", table.c_str());
    std::fclose(out);
    std::printf("%zu instructions written to %s\n", std::size(kOps), argv[1]);
    return 0;
}
