// Writes PS2Recomp's own translation of every MMI / pipeline-1 instruction as test functions
// (mmi_cases.inc) for tools/mmitest/harness.cpp, which compares them with a plain reference.
// Build: see tools/mmitest/run.sh. Usage: mmigen <out.inc>
#include "ps2recomp/code_generator.h"
#include "ps2recomp/r5900_decoder.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ps2recomp;

namespace
{
    struct Op
    {
        const char *name;
        uint32_t funct;
        uint32_t sa;
    };

    constexpr uint32_t kRs = 5, kRt = 6, kRd = 7;

    // MMI (opcode 0x1C): direct functs, and the MMI0-3 groups (funct 0x08/0x28/0x09/0x29, sub-op in sa).
    const Op kOps[] = {
        {"MADD", 0x00, 0}, {"MADDU", 0x01, 0}, {"PLZCW", 0x04, 0}, {"MFHI1", 0x10, 0}, {"MTHI1", 0x11, 0},
        {"MFLO1", 0x12, 0}, {"MTLO1", 0x13, 0}, {"MULT1", 0x18, 0}, {"MULTU1", 0x19, 0}, {"DIV1", 0x1A, 0},
        {"DIVU1", 0x1B, 0}, {"MADD1", 0x20, 0}, {"MADDU1", 0x21, 0},
        {"PMFHL_LW", 0x30, 0}, {"PMFHL_UW", 0x30, 1}, {"PMFHL_SLW", 0x30, 2}, {"PMFHL_LH", 0x30, 3}, {"PMFHL_SH", 0x30, 4},
        {"PMTHL_LW", 0x31, 0},
        {"PSLLH_0", 0x34, 0}, {"PSLLH_5", 0x34, 5}, {"PSLLH_15", 0x34, 15}, {"PSRLH_5", 0x36, 5}, {"PSRLH_15", 0x36, 15},
        {"PSRAH_5", 0x37, 5}, {"PSRAH_15", 0x37, 15}, {"PSLLW_0", 0x3C, 0}, {"PSLLW_5", 0x3C, 5}, {"PSLLW_31", 0x3C, 31},
        {"PSRLW_5", 0x3E, 5}, {"PSRLW_31", 0x3E, 31}, {"PSRAW_5", 0x3F, 5}, {"PSRAW_31", 0x3F, 31},
        // MMI0
        {"PADDW", 0x08, 0}, {"PSUBW", 0x08, 1}, {"PCGTW", 0x08, 2}, {"PMAXW", 0x08, 3}, {"PADDH", 0x08, 4},
        {"PSUBH", 0x08, 5}, {"PCGTH", 0x08, 6}, {"PMAXH", 0x08, 7}, {"PADDB", 0x08, 8}, {"PSUBB", 0x08, 9},
        {"PCGTB", 0x08, 10}, {"PADDSW", 0x08, 16}, {"PSUBSW", 0x08, 17}, {"PEXTLW", 0x08, 18}, {"PPACW", 0x08, 19},
        {"PADDSH", 0x08, 20}, {"PSUBSH", 0x08, 21}, {"PEXTLH", 0x08, 22}, {"PPACH", 0x08, 23}, {"PADDSB", 0x08, 24},
        {"PSUBSB", 0x08, 25}, {"PEXTLB", 0x08, 26}, {"PPACB", 0x08, 27}, {"PEXT5", 0x08, 30}, {"PPAC5", 0x08, 31},
        // MMI1
        {"PABSW", 0x28, 1}, {"PCEQW", 0x28, 2}, {"PMINW", 0x28, 3}, {"PADSBH", 0x28, 4}, {"PABSH", 0x28, 5},
        {"PCEQH", 0x28, 6}, {"PMINH", 0x28, 7}, {"PCEQB", 0x28, 10}, {"PADDUW", 0x28, 16}, {"PSUBUW", 0x28, 17},
        {"PEXTUW", 0x28, 18}, {"PADDUH", 0x28, 20}, {"PSUBUH", 0x28, 21}, {"PEXTUH", 0x28, 22}, {"PADDUB", 0x28, 24},
        {"PSUBUB", 0x28, 25}, {"PEXTUB", 0x28, 26},
        // MMI2
        {"PMADDW", 0x09, 0}, {"PSLLVW", 0x09, 2}, {"PSRLVW", 0x09, 3}, {"PMSUBW", 0x09, 4}, {"PMFHI", 0x09, 8},
        {"PMFLO", 0x09, 9}, {"PINTH", 0x09, 10}, {"PMULTW", 0x09, 12}, {"PDIVW", 0x09, 13}, {"PCPYLD", 0x09, 14},
        {"PMADDH", 0x09, 16}, {"PHMADH", 0x09, 17}, {"PAND", 0x09, 18}, {"PXOR", 0x09, 19}, {"PMSUBH", 0x09, 20},
        {"PHMSBH", 0x09, 21}, {"PEXEH", 0x09, 26}, {"PREVH", 0x09, 27}, {"PMULTH", 0x09, 28}, {"PDIVBW", 0x09, 29},
        {"PEXEW", 0x09, 30}, {"PROT3W", 0x09, 31},
        // MMI3
        {"PMADDUW", 0x29, 0}, {"PSRAVW", 0x29, 3}, {"PMTHI", 0x29, 8}, {"PMTLO", 0x29, 9}, {"PINTEH", 0x29, 10},
        {"PMULTUW", 0x29, 12}, {"PDIVUW", 0x29, 13}, {"PCPYUD", 0x29, 14}, {"POR", 0x29, 18}, {"PNOR", 0x29, 19},
        {"PEXCH", 0x29, 26}, {"PCPYH", 0x29, 27}, {"PEXCW", 0x29, 30},
    };
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: mmigen <out.inc>\n");
        return 2;
    }
    R5900Decoder decoder;
    CodeGenerator gen({}, {});
    FILE *out = std::fopen(argv[1], "w");
    std::string table;
    for (const Op &op : kOps)
    {
        // Instructions without a destination register are encoded with rd = 0, as compilers do.
        static const char *const kNoRd[] = {"DIV1", "DIVU1", "MTHI1", "MTLO1", "PMTHL_LW", "PDIVW", "PDIVUW", "PDIVBW", "PMTHI", "PMTLO"};
        bool noRd = false;
        for (const char *name : kNoRd)
            noRd = noRd || std::string(name) == op.name;
        const uint32_t rd = noRd ? 0u : kRd;
        const uint32_t raw = (0x1Cu << 26) | (kRs << 21) | (kRt << 16) | (rd << 11) | (op.sa << 6) | op.funct;
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
