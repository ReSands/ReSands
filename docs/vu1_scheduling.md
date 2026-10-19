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

## Related gap before publishing 1.0: where a new user's VU images come from

Compiled programs are matched by a hash of the whole code image: 16 KB for VU1, 4 KB for VU0. Our 35 VU1 images
were recorded from our own playthrough saves. A user building from a fresh ISO has none of them, so every scene
falls back to the interpreter: correct but slow, like the fights before 18 Oct.

We cannot ship the images, because they are game code. The plan is to ship a recipe instead:

- the list of image hashes;
- for each image, where its pieces come from on the disc (ELF or bigfile offsets and the upload layout);
- a build step that puts the images together from the user's own ISO and checks each hash before compiling.

This needs finding where PoP keeps its microprograms and how it uploads them (unchecked). It is not part of the
scheduling work, but it is a blocker for 1.0's "users build it themselves" model.
