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
- **Split into parts** (MSVC: one function per program had 0.4-2.6 MB stack frames, overflowed the VU1 thread's
  stack = crash in boot; with `/d2OptimizeHugeFunctions` the frame is 1.6 KB but the largest files took > 1 h).
  Now: parts of <= 200 nodes, one file each (`vu1_<h>_sched_<k>.cpp`), loops kept whole where they fit
  (recursive SCC decomposition), a driver `fastBlock<h, 0x80000000>` passes a state struct (`ctx.user`, new field
  in `Vu1FastCtx`) between them. 84 parts; vu1_recomp ~9 min + runtime ~15 min on the PC. The CMake flag stays
  for `*_sched*.cpp`. (Tried and dropped: tracking return addresses through BAL/JALR: 15k -> 50-200k nodes.)
- **Results (19 Oct, PC, 30 s boots, `oct19.ps1 -Sched`, `fps_sched_on/off.txt`):** verify identical to the exact
  build (death save 0 mismatches, fight 575, gameplay1 65: only the known vf13 one). fps off -> on: `093051`
  43-45 -> 51-52, `202311` 42-43 -> 46-47, `093011` 34-52 -> 41-53, `093100` 42-51 -> 53-54, `213954`/`214002`
  55-56 -> 57-59, `092959` 39-55 -> 44-55, death save 60 both.
- **Gate statistics** (`PS2X_VU1_SCHED_STATS=1`, every 65536 runs per program): in the fights all programs run
  100% scheduled except **`eea60abc4b18ca8e`: 62% of its runs are not scheduled** (fight: 1.83 M runs, 691 k
  ended scheduled, 572 k hand over, 572 k start with the pipeline not empty). Cause: `EATAN P, vf22z` at 0x0D00
  (EFU, latency 54, not supported -> hand-over; first written down as ESIN, corrected 19 Oct evening), and the next run (MSCNT continuation) finds `m_efuResourceReady > m_cycle`
  (EFU still busy; the runtime's cycle does not advance between runs). `MFP.z vf22, P` at 0x0270/0x04C8 reads it
  in the next run. Same code in `9e37579481796a65`. Profile of the fight (scheduled build): VU thread 88% busy,
  runFast (per-pair) 16% inclusive, processVIF1Data 7.8% self, clipBits 2.7% (not inlined; fixed since).

### EFU in the scheduler (19 Oct evening: built, Mac check clean; PC verify + stats pending)

Done as planned below, with two corrections: the op at 0x0D00 in `eea60abc`/`9e375794` is **EATAN**
(special 0x7C, latency 54), the only EFU op in all 11 programs (plus MFP at 0x0270/0x04C8), so the scheduler
takes the single-lane functions ESIN/EATAN/EEXP (and WAITP); and residual entries are added per End node for
its own continuation (`n.endPc`, `n.after.efuRes`; an MSCAL resets the EFU, so only the MSCNT continuation
matters). Mac check: 0 EFU hand-overs (left: 4 generic lower, 1 fallback pair), 75 -> 86 entries (EFU busy
1, 2 or 7 cycles), 15,030 -> 16,008 nodes, 84 -> 89 parts, FMAC flag producers kept 342 of 4,935. Driver
picks the entry by `(pc << 8) | efuBusy` (efuBusy > 255 or unknown -> per-pair code). Budget exit from the
scheduled code now also runs `commitEfu(ctx.cycle)` like runFast. Next: PC run `game\jobs\sched.ps1`, check
verify (no new mismatches) and the gate stats of `eea60abc` (expect ~100% scheduled), then fps on/off.

Original plan:

Interpreter semantics (`ps2_vu1_impl.inl` queueP ~590, decode `ps2_vu1_core.cpp` ~860-935, commitEfu ~2840):
two result slots `m_efu[2]` {valid, readyCycle, value}; an EFU op takes the first free slot, ready = cycle +
latency (ESIN 29), value = normalizeResult(vuEsin(normalizeOperand(vf[fs][fsf]))) (vuEsin is in impl.inl,
~1614); `m_efuResourceReady = cycle + latency - 1`; an EFU op stalls until `m_efuResourceReady`; WAITP (0x7B)
stalls until both slots are ready; commits by ready cycle (ties: lower index first) set `m_state.p`. MFP
(lowerEmit case 0x64) reads P after `commitEfu`. resetScheduler (MSCAL) clears m_efu and the resource; runFast
commits all EFU at Ended (commitEfu kAll). The verifier compares `m_efuResourceReady` exactly, `p` always.

Plan:
1. `St`: `efu[2]` remaining latency, `efuRes`. `advance`: decrement, `Op::CommitEfu{slot}` at 0 (emit
   `pcur = pv<slot>;`, always needed). `maxRem` includes the slots. staticStall: EFU pipe -> `efuRes`, waitP ->
   max slot rem.
2. `supportedLower`: allow ESIN (special 0x79) and WAITP (0x7B); other EFU ops still hand over. Slot full ->
   hand over.
3. lowerEmit in g_sched mode: ESIN -> `{ uint32_t ig = 0u; pv<slot> = vu.normalizeResult(vuEsin(vu.normalizeOperand(<lane>)), ig); vu.m_efuResourceReady = cyc + 28u; }`
   (slot via a global set by pairBody); WAITP -> nothing; MFP -> `_mm_set1_ps(pcur)` instead of commitEfu/vu.m_state.p.
4. Locals/state struct: `pcur` (from/to `vu.m_state.p` in the driver), `pv0`, `pv1` (add to `kScalars` and
   `stateStruct`). materialize: pending slots -> `vu.m_efu[i] = {true, cyc + rem, pv<i>}` + noteReadyCycle.
5. End nodes: flush commits EFU; nothing else (m_efuResourceReady was already written at issue).
6. **Entries with the EFU busy:** the gate drops `m_efuResourceReady <= m_cycle` from the "pipeline" test; the
   driver computes `r = efuResourceReady - cycle` (0 if past) and picks the entry node by (pc, r). Graph entries
   become a map (pc, r) -> node; after building, collect the residual `efuRes` of all End nodes (state before
   the flush, `n.after.efuRes`) and add entries (each E-bit continuation pc, r) for them; repeat until no new
   residuals. An unknown (pc, r) returns Fallback (per-pair code, as today).
7. Check on the Mac (`tools/vu1sched/check.sh`), amend patch 37 in `~/tools/PS2Recomp-sotc` (branch up1017,
   `git commit --amend`, `git format-patch -1 --stdout > tools/patches/pop-sotc/37-...patch`), commit, bundle,
   run `game\jobs\sched.ps1` on the PC (drops the old 37, re-applies, builds with VU1R_SCHED=1, verifies, then
   gate stats on 3 fight saves). Then the fps on/off measurement (`oct19.ps1 -Sched -NoBuild`).

After that, in order of expected win: per-pair code still used (check stats again), `processVIF1Data` (7.8%),
part-transition cost (count transitions), then the death-save program `04994ae7` (not seen in fight stats).

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
