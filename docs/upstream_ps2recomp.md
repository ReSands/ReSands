# PS2Recomp bugs found by the Prince of Persia port: draft upstream report

Draft, 16 Oct 2026. Not posted anywhere yet (the user decides). Each item was found while bringing
up Prince of Persia: The Sands of Time (SLUS-20743); "upstream" = ran-j/PS2Recomp `origin/main` at
**eb8688b** (16 Oct, after #272 "Emulate PS2 float semantics"), checked by reading the code and, for
instructions, by running our instruction tests (`tools/mmitest/run.sh <checkout>`) against a build of
it. Our fixes are in `tools/patches/` (upstream-based `pop` branch) and `tools/patches/pop-sotc/`
(TaylorNAlbarnaz `sotc-port` based branch); patch numbers below refer to those.

## Translation (recompiler)

| # | Bug | Effect seen | Upstream eb8688b | Our fix |
|---|---|---|---|---|
| 1 | **LQ/SQ (and LQC2/SQC2) use the address as given.** The EE ignores the low 4 bits of a quadword load/store address. | Unaligned LQ + QFSRV copies shifted data; the game's bigfile loader read garbage. | Still present: `instruction_translator.cpp` passes `rs + imm` to `READ128`/`WRITE128`, which do not mask. | `ps2recomp-lq-sq-align.patch` |
| 2 | **BC2T/BC2F test `vu0_status & 1`** (VU0's Z flag). On the EE, COP2 condition = VU1 running (CPCOND). | The game's VU1-idle wait spun forever once VU0 maths left Z set. | Still present: `control_flow_emitter.cpp:442`. | Override (returns at once); proper fix: CPCOND = VU1 busy. |
| 3 | **VCALLMSR indexes `vi[rd]`** with the instruction's register field (VI27 = CMSAR0); `vi` has 16 entries. | Out-of-bounds read (undefined behaviour) when a VU0 microprogram is started this way. | `translateVU_VCALLMSR` still reads `ctx->vi[{rd}]`. (#268 addressed it on our branch; re-check whether it reached main.) | `ps2recomp-vcallmsr-cmsar0.patch` / pop-sotc 02 |
| 4 | **VU0 macro VLQI/VLQD/VSQI/VSQD/VILWR/VISWR address EE RAM at vi*16** instead of VU0 data memory (0x11004000); VSQI/VSQD swap VF/VI fields; VILWR/VISWR use word addressing. | A VU0-computed count smashed a stack buffer (crash). | VLQI still `READ128(vi << 4)` (EE RAM). | `ps2recomp-vu0-macro-memory.patch` |
| 5 | **MMI instructions** (instruction test, 20,000 random + edge cases each, against a plain reference from the EE manual's semantics): **38 of 110 wrong** upstream: PMFHL (all 5 formats), PMTHL.LW, PABSW, PABSH (read rs, not rt), PADDUH/PSUBUH (no saturation), PMADDW, PMSUBW, PMADDUW, PMULTW (one sum of four unsigned products instead of two signed 64-bit products), PMULTUW, PMULTH (one sum instead of eight halfword products), PDIVW, PDIVUW, PDIVBW (one lane), PMADDH, PMSUBH, PHMADH, PHMSBH, PSLLVW/PSRLVW/PSRAVW (value/count swapped, no sign extension), PMFHI/PMFLO and PMTHI/PMTLO (64 of 128 bits), PINTH, PINTEH, PEXEH, PEXEW, PEXCH, PEXCW, PREVH, PROT3W. | The game's MMI-vectorised ADPCM decoder (`adpcm_decoder`) produced noise: all streamed music/voice garbled. | All 38 still wrong. | pop-sotc 21, 22, 24 (the 20 PoP uses; 18 unused ones listed for upstream) |
| 6 | **FPU (COP1)** against an EE FPU model (PCSX2 defaults: round toward zero for ADD/SUB/MUL/accumulator forms and CVT.S.W, nearest for DIV/SQRT/RSQRT; denormals as zero in and out; exponent 255 = ordinary large number, no inf/NaN): upstream differs on **18 of 24** instructions. Notably **SQRT.S reads fs instead of ft** (EE encoding: `sqrt.s fd, ft`); results with exponent 255 inputs come out as host inf/NaN; C.EQ/C.LT/C.LE wrong around denormals; MAX.S/MIN.S use host float ordering; CVT.W.S does not saturate exponent-255 inputs. | Last-bit drift in game maths, NaN propagation from data the EE treats as numbers. | Present (this report's numbers are from eb8688b). | pop-sotc 25 (EE thread runs with round toward zero + FTZ/DAZ; MAX/MIN by EE ordering); with it our model test passes 24/24 on the sotc base. |
| 7 | **`rand` is replaced by `std::rand() & 0x7FFF`** when the analyzer names it. newlib's `rand` returns 31 bits (RAND_MAX 0x7FFFFFFF); code that scales by 1/2^31 always got ~0. | A cutscene's random choice always picked the first option and the scene spun forever. | Still present: `Kernel/Stubs/LibC.cpp:1085`. | Override running newlib's algorithm on the game's own state; better: do not stub `rand`. |

## Runtime

| # | Bug | Effect seen | Upstream eb8688b | Our fix |
|---|---|---|---|---|
| 8 | **COP0 Count never advances** (`mfc0 Count` reads a field nothing updates). | Game time stood still: fades never progressed, text drawn with alpha 0. | `cop0_count` is only written by MTC0; nothing advances it. | `ps2recomp-cop0-count.patch` (Count from host time at 294.912 MHz) |
| 9 | **`sceGsExecLoadImage` / `StoreImage` stand-ins multiply the block address by 8** (`vram_addr * 2048 / 256`): the address is already in 256-byte block units. | Images landed in the wrong GS memory (our loading screen went into the Z buffer). | Still present: `Kernel/Stubs/GS.cpp` (`dbp = vram_addr * 2048u / 256u`). | pop-sotc 04 |
| 10 | **libvu0 stand-ins** (`sceVu0MulMatrix` operand order; `InnerProduct`/`Normalize` include w). | Projection matrix wrong, scene vertices garbage. | Upstream has since revised `sceVu0MulMatrix` (comment says first operand on the left); **not re-verified**: run a self-test that calls the game's own libvu0 on known inputs (our `[pop:vu0test]`). | Rebuild scripts rename `sceVu0*` so the game's own libvu0 is recompiled. |
| 11 | **VIF1: data after a DIRECT ending in a GIF IMAGE tag is treated as image data**, so the next DMA tag's VIF codes became pixels. | Dot grid / 2-pixel shift in uploaded images. | Not re-checked. | `ps2recomp-vif1-direct-image-continuation.patch` |
| 12 | **CPU GS rasterizer: sprites truncate positions/UVs to whole pixels.** | Half-resolution glow passes bled Z-buffer data (orange/green wash). | Not re-checked. | `ps2recomp-gs-sprite-subpixel.patch` |
| 13 | HLE heap (`guestMalloc`) placed in the game's own heap (from SetupHeap); `sceCdCallback` is a no-op; `sceSifLoadModuleBuffer` reads EE memory at the IOP address; no HLE for `sceCdLayerSearchFile` / `sceCdDiskReady`. | Boot blockers (corrupted objects, CD worker waiting forever, IOP modules not loaded). | Not re-checked. | Game override (`tools/overrides/pop_slus20743.cpp`). |

## How to reproduce the instruction results
`tools/mmitest/run.sh <PS2Recomp checkout with build-mac>`: generates each instruction's translation
with the checkout's own decoder and code generator and compares it with the reference (MMI:
`harness.cpp`, FPU: `fpuharness.cpp`; `FPU_MODE=ee` runs the translations with round toward zero and
denormals flushed). Builds on macOS via sse2neon in about a minute; on x86 it uses real SSE.

## Before posting
- Re-check items 3, 10-13 against the then-current main.
- Split into separate issues (translation, FPU, MMI, runtime stand-ins), each with the test output.
- Our patches are GPL-3.0 like PS2Recomp; offer them as PRs where they apply to main.
