# Build-time scheduling for compiled VU1 (and VU0) code: plan

Written 19 Oct 2026. Claims marked (estimate) are judgement calls; (unchecked) means not yet read in the code.

## Why

After the 18 Oct night, 59 of 77 saves hold 60 fps. In every slow save left (the sand-monster fights, `092426`,
some 21:39-21:46 saves) the **VU1 thread is 88-90% busy in compiled VU1 code** while the game thread waits for it
in `sceGsSyncPath` (59%). Our `vu1_recomp` output still does at run time what the hardware does for free: it
counts cycles, models stalls and runs the 4-deep MAC/status/clip flag pipeline (`cyc`, `commitFlags`,
`foldStatus`/`makeStatusRoom` ~7%; `fastBlock` most of the rest; `processVIF1Data` ~8% is separate).

Road Trip Adventure's VU1 recompiler (catsandsoup, GPL-3.0, `tools/vu1recomp/`) does this work once, at build
time: VU1 went from 82 ms to 1.3 ms per frame (`docs/reference.md`, CPU-side survey).

## What does not change

- **Results.** The compiled code must stay bit-exact against the interpreter. `PS2X_VU1_VERIFY=1` /
  `PS2X_VU0_VERIFY=1` are the judge, and nothing goes in that adds a mismatch.
- **Timing model.** The runtime still needs the VU1 cycle count of each run for `vu1Busy`
  (`PS2X_VU1_TIMING`: 1.0 at 30 fps, 0.5 at 60). The cycle counts become constants per path instead of being
  counted at run time. 30 fps stays at console speed.
- **Boot and play for a new user.** Nothing new runs at boot. Only the build changes (see "Build cost").

## Steps

0. **Survey first** (survey-before-fixing rule). Read Road Trip's `tools/vu1recomp/` and how it verifies
   (`PS2X_VU1_DIFF`, replayed and fuzzed snapshots). Also check black-recomp's VU1 blocks and reo's
   translator. GPL-3.0 is compatible with PS2Recomp; credit anything we take. Check Road Trip's assumption that
   every MSCAL starts with an empty pipeline against our runtime and against how PoP chains its programs (if the
   previous program ended with the E bit, the pipeline has drained; unchecked for PoP).
1. **Fix the known `vf13` mismatch first**, so the verifier starts clean.
   - Facts: image `9d41512cd9917433`, entries 0x0D30/0x1068 and 0x16F0, `vf13` differs at the end of the run.
     It happens in 0.7% of runs in the fight and 0.04% in `gameplay1`. GIF packets and VU memory match, so the
     drawn output is the same.
   - The loops involved: 0x0F78-0x1040 and 0x1468-0x1540 (MULq, MINIx.w/MAXy.w on vf13, FTOI4, XGKICK).
   - Approach: make the verifier dump the input state of one mismatching run, replay it offline through both
     paths, and find the first instruction where they differ. Then check whether the base runtime (without
     patches 31-34) shows it too.
2. **Baseline.** Record VU1 thread busy %, VU1 time per frame and fps for the 18 saves under 60, the build time
   of the VU runner files (~30 min with `-Vu1` today), and the size of the generated VU code.
3. **Static pipeline scheduler in `vu1_recomp`.**
   - Walk each entry point over `(pc, pipeline state)` nodes. Stalls, flag commits and cycle counts become
     constants in the generated code.
   - Detect jump tables (Road Trip found an 8-slot `B` table) so every slot is compiled.
   - Anything the walk cannot resolve (an unknown indirect jump, too many pipeline states at one pc) keeps
     today's run-time model for that block. It is a fallback, not an error.
4. **Flag liveness.** Remove flag computations that no later instruction reads (FSAND/FSEQ/FMAND/FCAND/...,
   branches on flags, and the state left when the program ends). Road Trip removed all 111 of its FMAC flag
   producers. A program end counts as a reader unless we prove the next program does not look.
5. **Verify.**
   - `PS2X_VU1_VERIFY=1` over the fight saves, `gameplay1` and then all 77 saves: 0 new mismatches.
   - Replay the dumped snapshots from step 1.
   - Clean fps pass over all saves (`oct18.ps1 -Final` style, environment logged next to each number).
   - Nightly checks and the scripted cold boot (`tools/windows/fps60-check.ps1`).
6. **VU0.** Apply the same passes to the one VU0 image (`PS2X_VU0_VERIFY=1`).
7. **Report.** Report fps before and after, VU1 time per frame, build time and code size, and any blocks left on
   the fallback, plainly.

Done when the 18 saves under 60 hold 60, or a profile shows plainly what limits them next. That could be
`processVIF1Data`, the GS, or the game thread.

## Build cost (for a new user building from their own disc)

- The scheduling pass itself should take seconds to a few minutes (estimate).
- The unknown is the C++ compiler. If the scheduled code comes out larger, the VU files could take longer than
  today's ~30 min. It could also come out smaller, since the flag and cycle bookkeeping goes away. Measured in
  step 2 and step 7.

## Progress (19 Oct 2026)

- **Step 0 (survey), done.** Road Trip's `tools/vu1recomp/vu1recomp.py` (cloned to `~/tools/surveys/roadtrip`)
  walks `(pc, abstract pipeline state)` nodes: remaining latency per VF lane / VI / ACC, a fixed set of flag
  slots, FDIV, the branch in flight and the E bit; an XGKICK's data-dependent wait becomes a switch over the
  few states it can lead to; JR targets are the link labels (pc+16 after BAL/JALR); flag liveness is a backward
  pass. Our runtime meets its entry assumption: MSCAL -> `execute()` -> `resetScheduler()` (empty pipeline),
  MSCNT -> `resume()` after an E-bit end. The generated code still checks it (pipelines empty, nothing pending)
  and uses the per-pair code otherwise.
- **PoP's flag use** (all 10 programs): no status reader at all (FS*), FMAND in a few places, clip readers
  (FCAND/FCOR/FCGET) heavily, FCSET only in 0x4DAC8C, EFU (ESIN/MFP) in two programs, RINIT/RGET/RNEXT in one.
- **Built: runtime patch 37** (`vu1_recomp`, `VU1R_SCHED=1` at build time; `PS2X_VU1_SCHED=0` turns it off at
  run time for A/B in one build).
  - One function per program over all nodes from its entry points (`<capture dir>/entries.txt`, written by
    `tools/vu1progs.py --check`, plus every E-bit continuation). Registers live in locals for the whole run.
  - Stalls, cycle counts and flag commits are constants. MAC/status flags are only computed where a later
    FMAND or the program end can see them: **315 of 4,808 FMAC flag producers are kept**. Status sticky bits
    are still accumulated cheaply (the end state stays exact). FDIV results are computed inline and committed
    at their cycle.
  - Hand-overs to the per-pair code with the exact pipeline state ("deopt"): pairs it does not handle (EFU,
    generic lower ops, padding), unknown JR targets, and the budget stop (checked at every node so it stops at
    the same pair as the per-pair code). Over the 11 programs: 15,030 nodes, 6 hand-over nodes.
  - Verifier change in the same patch: ready cycles at or before the unit's cycle compare as equal (they stall
    nothing; the scheduled code writes only the ones still in the future).
  - Generated code: 0.2-2.3 MB per program (largest: 42 s at -O2 in clang on the Mac).
- Results of the first PC build (verify, fps on/off): below when measured.

## Where a new user's VU images come from (solved 19 Oct 2026)

PoP keeps every VU microprogram in the ELF as a VIF MPG packet that loads at VU address 0
(0x4CD50C-0x4E4DEC in SLUS_207.43). The 16 KB image the runtime sees is the current program plus whatever an
earlier, longer program left behind, which is why whole-image hashes kept producing "new" images (the death
scene of 19 Oct ran on the interpreter at 39-43 fps that way). `tools/vu1progs.py` writes each program
zero-padded from the user's own ELF; the runtime matches compiled programs by zero-padded prefix (upstream
patch 33, our 35 sends pairs past a program's end to the interpreter), so 11 compiled programs cover every
combination. Nothing to ship but the tool. Two small uploads at other addresses (0x4E4A6C: 0x20 bytes at VU
0x400; 0x4E50EC: 0xA0 bytes at 0x200) were never seen in a capture; an image with one would fall back to the
interpreter.
