# Draft issue for ran-j/PS2Recomp

**Title:** 38 of 110 MMI / pipeline-1 instruction translations differ from EE semantics (instruction-level test attached)

**Body:**

I wrote an instruction-level test for the recompiler's MMI translations: it encodes each MMI and pipeline-1 instruction, asks `R5900Decoder` + `CodeGenerator::translateInstruction` for the generated C++, compiles those snippets against `ps2_runtime_macros.h`, and compares them on 20,000 random and edge-case register values (rd/HI/LO, all 128 bits) with a plain per-lane reference written from the EE instruction semantics. Against `main` at 2c5fbb9, 38 of 110 differ:

| Instruction(s) | What is wrong |
|---|---|
| PMULTW, PMULTUW | Sums four unsigned products into one value; should be two 32x32->64 products (words 0 and 2) into rd, LO/HI and LO1/HI1. PMULTW is signed. |
| PMULTH | Sums the products into one value; should be eight signed 16x16->32 products: rd = {p0, p2, p4, p6}, LO = {p0, p1, p4, p5}, HI = {p2, p3, p6, p7}. |
| PMADDW, PMSUBW, PMADDUW | Same summing problem, plus the accumulate uses a single 64-bit HI:LO instead of words 0 and 2. |
| PMADDH, PMSUBH, PHMADH, PHMSBH | Lane layout of LO/HI and rd does not follow the EE (see PMULTH). |
| PMFHI, PMFLO | Copy only the low 64 bits; HI/LO are 128 bits (upper halves in hi1/lo1). |
| PMTHI, PMTLO | Write only 32 bits of HI/LO. |
| PMFHL (all five formats), PMTHL.LW | Treat HI/LO as single 64-bit values, so the word/halfword selection is wrong in every format. |
| PDIVW, PDIVUW, PDIVBW | Only one lane is computed. |
| PSLLVW, PSRLVW, PSRAVW | Value and shift count come from swapped registers (rd = rt shifted by rs), and the 32-bit results are not sign-extended to each doubleword (only words 0 and 2 are used). |
| PEXEH, PEXCH, PREVH | Wrong halfword shuffles: per doubleword PEXEH = {h2,h1,h0,h3}, PEXCH = {h0,h2,h1,h3}, PREVH = {h3,h2,h1,h0} (not all eight reversed). |
| PEXEW, PEXCW, PROT3W | Wrong word shuffles: PEXEW = {w2,w1,w0,w3}, PEXCW = {w0,w2,w1,w3}, PROT3W = {w1,w2,w0,w3}. |
| PINTH, PINTEH | PINTH: rd.h[2i] = rt.h[i], rd.h[2i+1] = rs.h[4+i]. PINTEH: rd.h[2i] = rt.h[2i], rd.h[2i+1] = rs.h[2i]. |
| PABSW, PABSH | Read rs instead of rt (the operand is rt; rs is normally $zero, so the result is 0). |
| PADDUH, PSUBUH | Wrapping instead of unsigned saturation. |

Found while getting a game's IMA ADPCM stream decoder (PMULTW, PEXCW, PEXCH, PSLLVW) and mixer (PMULTH, PMFLO/PMFHI) to work: before the fixes the decoded music was noise; after them the recompiled decoder matches a reference decoder on every sample.

I have fixes for PMULTW, PMULTH, PMFHI/PMFLO, PMFHL (all formats), PINTH, PINTEH, the variable word shifts and the exchange/rotate shuffles, and can send them as a PR together with the test (it builds on macOS through sse2neon and should build with MSVC). The remaining ones (PMADD*/PMSUB*/PHM*, PDIV*, PMTHI/PMTLO, PMTHL, PABS*, PADDUH/PSUBUH) are not fixed yet.

Not covered by the test yet: QFSRV, the VU0 macro (COP2) instructions, and the rest of the R5900 set.
