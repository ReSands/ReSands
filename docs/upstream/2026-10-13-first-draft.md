# PS2Recomp upstream reports (drafts, 13 Oct 2026)

Found while bringing up Prince of Persia: The Sands of Time (SLUS-20743). Checked against
upstream `main` 2c5fbb9 and the open issues/PRs (up to #287). Already covered upstream, so not
reported: sprite fractional coordinates (#281), VIF1 DIRECT image continuation (#269).

---

## 1. `rand` HLE returns 15 bits; newlib's `rand` returns 31

**Where:** `ps2xRuntime/src/lib/Kernel/Stubs/LibC.cpp:1085`: `setReturnS32(ctx, std::rand() & 0x7FFF);`

`rand` is on the exporter's stub list, so any game whose `rand` gets named (e.g. by `ps2_analyzer`) calls this stand-in. PS2 games link newlib, whose `rand` returns 0..0x7FFFFFFF (`RAND_MAX` = 2^31-1). Game code that scales the result by 1/2^31 to get a float in [0,1) then always gets ~0.

**Effect (SLUS-20743):** the game's random-range helper at 0x1B9A40 computes `a + (b-a) * rand() / 2^31`, so it always returned `a`. A scripted cutscene waited for a random value that never came and spun forever.

**Fix options:** implement newlib's algorithm on the guest's own state (newlib: `_REENT->_new._reent._rand_next = _rand_next * 6364136223846793005 + 1; return (_rand_next >> 32) & 0x7FFFFFFF;`), or stop stubbing `rand`/`srand` so the game's own newlib code is recompiled. We did the former in a game override (the state is at `*(0x4E8768) + 0xA8` in this build).

---

## 2. `bc2t`/`bc2f` test the VU0 status Z flag instead of "VU1 running"

**Where:** `ps2xRecomp/src/lib/control_flow_emitter.cpp:443`: the COP2 branch condition is `(ctx->vu0_status & 0x1)`.

On the EE, BC2T/BC2F test the COP2 condition line, which the PS2 wires to VU1's busy state (it is how games wait for a VU1 microprogram to finish). Bit 0 of the VU0 status register is the Z flag, which ordinary VU0 macro maths sets and clears.

**Effect (SLUS-20743):** the game's only `bc2t` is a VU1-idle wait loop (0x4A4D00) in its VIF1 kick routine. Once VU0 maths left Z set, the loop never exited (hard freeze in gameplay).

**Fix:** test the runtime's VU1-running state (or, in a runtime where VU1 microprograms complete synchronously, treat VU1 as idle: `bc2t` not taken, `bc2f` taken). We patched it with a game override.

---

## 3. `sceGsExecLoadImage`/`sceGsExecStoreImage` multiply the VRAM address by 8

**Where:** `ps2xRuntime/src/lib/Kernel/Stubs/GS.cpp:634` and `:699`:
`uint32_t dbp = (static_cast<uint32_t>(img.vram_addr) * 2048u) / 256u;`

`sceGsSetDefLoadImage`/`sceGsSetDefStoreImage` take the address in 64-word (256-byte) blocks, the same unit as BITBLTBUF's DBP/SBP. The stand-ins treat it as 2 KB units, so every upload/download goes to 8x the intended block address.

**Effect (SLUS-20743):** loading-screen images (256x256 PSMCT32 at blocks 0x3480/0x3880) landed in the Z buffer, showing garbage instead of the loading screen.

**Fix:** `uint32_t dbp = img.vram_addr;` (and the same for `sbp`).

---

## 4. LQ/SQ/LQC2/SQC2 do not ignore the low 4 address bits

**Where:** `ps2xRecomp/src/lib/instruction_translator.cpp:198` (LQ), `:200` (SQ), `:213` (LQC2), `:215` (SQC2).

The EE forces quadword alignment for these: the low 4 bits of the effective address are ignored, so no address error occurs. Optimised copy routines rely on this: `lq` from a misaligned pointer, then `qfsrv` by the misalignment. The generated code reads from the exact unaligned address, so those copies come out shifted.

**Effect (SLUS-20743):** data copied with such a routine after LZO1X decompression was shifted; the bigfile loader then failed.

**Fix:** mask the effective address: `(ADD32(GPR_U32(ctx, rs), imm) & ~0xFu)` for all four.

---

## 5. `mfc0 Count` never advances

**Where:** `ps2xRecomp/src/lib/cop0_translator.cpp:47`: `SET_GPR_S32(ctx, rt, (int32_t)ctx->cop0_count);`. Nothing in the runtime updates `cop0_count`.

Games use Count (EE clock, 294.912 MHz) to measure frame time. With Count frozen, game time never advances.

**Effect (SLUS-20743):** fades stayed at alpha 0 (title text invisible), and timed logic stood still.

**Fix:** derive Count from a host clock: `count = base + (now - t0) * 294.912 MHz`, with `mtc0 Count` setting the base. Our patch adds `ps2ReadCop0Count`/`ps2WriteCop0Count` helpers in `ps2_runtime.h` and calls them from the translator.

---

## 6. VU0 macro VLQI/VLQD/VSQI/VSQD/VILWR/VISWR: wrong memory, swapped fields, wrong units

**Where:** `ps2xRecomp/src/lib/vu_translation_helpers.cpp:114` (VILWR), `:119` (VISWR), `:768` (VLQI), `:784` (VSQI), `:800` (VLQD), `:816` (VSQD).

1. They access EE main RAM at `vi * 16`. In macro mode these instructions address **VU0 data memory** (4 KB, mapped at 0x11004000 on the EE bus): address = `0x11004000 + (vi & 0xFF) * 16`.
2. VSQI/VSQD swap their operands. The encoding is `VSQI.dest vfs, (vit++)` with fs in bits 15..11 and it in bits 20..16, but the code uses `rd` as the address register and `rt` as the source VF.
3. VILWR/VISWR treat VI as a word address (`vi << 2`). VI is a quadword address, and the field (.x/.y/.z/.w) selects the word within it.
4. VI00 is hard-wired to zero, so the post-increment/pre-decrement forms must not modify it.

**Effect (SLUS-20743):** a VU0 routine (at 0x475380) writes a result count to VU0 memory and the EE reads it back; the EE got garbage, copied that many quadwords into a 512-byte stack buffer, and smashed saved registers.

**Fix:** our patch, ~60 lines in `vu_translation_helpers.cpp` (shared helpers for the VU0 quadword address, the VI step and the field offset). Happy to open a PR.

---

## 7. libvu0 HLE stand-ins: `sceVu0MulMatrix` order reversed; `InnerProduct`/`Normalize` include w

**Where:** `ps2xRuntime/src/lib/Kernel/Stubs/VU.cpp:694` (MulMatrix), `:548` (InnerProduct), `:728` (Normalize); added in #183.

We called the game's own compiled libvu0 functions on known inputs from a game override and compared:

- `sceVu0MulMatrix(m0, m1, m2)` gives `m0 = m1 x m2` in Sony's column-vector convention, i.e. with rows as stored, each row of `m2` transformed by `m1` (m2 applied first). The stand-in computes `m1 . m2` in row-major order: the opposite order.
- `sceVu0InnerProduct` returns the dot product of **xyz** only. The stand-in includes w.
- `sceVu0Normalize` normalises **xyz** (length from xyz). The stand-in includes w in the length.

`ApplyMatrix`, `TransposeMatrix`, `DivVector`, `UnitMatrix` and `RotMatrixZ` matched.

**Effect (SLUS-20743):** the projection matrix came out wrong, so all 3D vertices were garbage.

**Suggestion:** fix the three stand-ins. More robust: drop `sceVu0*` from the stub list so each game's real libvu0 is recompiled (it is plain VU0 macro code). We rename them in our export for that reason.

---

## 8. HLE gaps hit while booting SLUS-20743

All handled with a game override. Reporting so the runtime can cover them generally:

- **`sceCdLayerSearchFile`** has no HLE handler (the runtime has `sceCdSearchFile`). The game calls it directly; the original busy-waits on an RPC that never answers. Mapping it to the `sceCdSearchFile` handler works.
- **`sceCdDiskReady`** (not named by `ps2_analyzer` in this build; binds cdvdfsv RPC 0x8000059A): same busy-wait.
- **`sceCdCallback`** is a no-op. Games that wait for the callback after `sceCdRead` (here: a worker thread blocked on a semaphore the callback signals) hang. The general fix is to call the registered callback when an HLE read completes; our override signals the game's semaphore directly.
- **`sceSifLoadModuleBuffer`** takes an **IOP** address (the game first DMAs the IRX image to IOP RAM with `sceSifSetDma`). The stand-in reads EE memory at that address, so it loads garbage. Tracking SIF DMAs and using the EE source buffer works.
- **Runtime heap vs the game's allocator:** `runtime->guestMalloc` (used by several HLEs, e.g. the 48 KB GS packet in `sceGsExecLoadImage`, thread stacks, SIF) takes its region from the game's `SetupHeap` range. This game runs its own dlmalloc in that same range, so HLE allocations corrupted game objects. Possibly related to #216. We moved the runtime heap below the ELF (0x80000-0x100000).

---

## 9. `sceSdRemote*` stand-ins never reach the IOP, so games using the real sound driver stay silent

**Where:** `ps2xRuntime/src/lib/Kernel/Stubs/Audio.cpp`: `sceSdRemoteInit` only resets local state, `sceSdRemote` records voice/block transfers and returns plausible status values, `sceSdTransToIOP` is a TODO. None of them sends the RPC (sdr server 0x80000701) to the IOP.

That suits a runtime that synthesises audio from these calls itself. But when the game's own IOP modules run on the emulated IOP (here libsd, `sdr_driver` and the game's sound driver, loaded with `sceSifLoadModuleBuffer`), the driver never receives `sceSdInit` or any command, so it never touches the SPU2.

**Effect (SLUS-20743):** with the SPU2 model running at full rate (48,000 samples/s, no underruns), the output was all zeros, and the SPU2 saw **zero register writes** from boot onward.

**Fix:** when the game's sdr driver is running on the emulated IOP, don't substitute these functions; let the game's EE-side client make its real RPC calls. We do this by renaming them in the export (`pop_sceSd…`) so the original code is recompiled. A runtime switch (stand-in vs real RPC) would cover both cases.

Not sufficient on its own for SLUS-20743 (13 Oct 2026, still being verified): the game's sound thread is created at priority 0, which the EE scheduler of the SotC fork rejected (`priority < 1`; 0 is valid on the EE kernel), and the SPU2 model dropped AutoDMA (streamed PCM) transfers. Check whether upstream's scheduler and SPU2 have the same limits before reporting those.

---

Also seen, already known upstream: game overrides registered from the static library are silently dropped (#264).
