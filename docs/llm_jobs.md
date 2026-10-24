# Jobs for the local model (overnight, unattended)

Rule: a job is only worth running if a script can judge each answer. Without a judge the model
produces plausible nonsense (naming test, 14 Oct: ~1/3 right meaning, 0 exact). Sorted by judge quality.

| # | Job | Input | Judge (automatic) | Value | Ready? |
|---|-----|-------|-------------------|-------|--------|
| 1 | **Fix the remaining wrong MMI translations** (22: PMADD*/PMSUB*/PHM*, PDIV*, PMTHI/PMTLO, PMTHL, PABS*, PADDUH/PSUBUH) | PS2Recomp translator source + the instruction's reference in tools/mmitest/harness.cpp | `tools/mmitest/run.sh`: the instruction passes 20k random cases, no other instruction regresses | Upstream PR; any game using them | Needs a driver that applies a patch, rebuilds, runs the test (Mac, ~1 min per try) |
| 2 | **Reference semantics for VU0 macro (COP2) ops** to extend the instruction test | Public instruction descriptions | Agreement with an independent oracle (PCSX2 interpreter run locally as a test oracle only, never shipped) | Finds the next class of recompiler bugs (VU0 maths: matrices, medallion drift) | Needs the oracle harness |
| 3 | **Other bigfile record formats** (materials, game objects/placement, cameras, animation) | Record bytes + record size + any decompiled loader | Parse consumes exactly the record's size, for every record of that type across all packs; values in sane ranges | Route D: materials and world placement | Split: discovering what fields mean is NOT a model job (tests: weak at discovery); once a layout is known, writing parsers for all variants against the exact-size judge is |
| 4 | **World placement of meshes** | Candidate matrices from game-object records | Placed objects form connected buildings (shared edges/vertices), match the GS screenshot of a save-state spot | Route D stage 1 | After #3 finds the game-object format |
| 5 | **Decompile -> Python ports** (c2py gaps, hand-written functions like `_LOA_ReadMatrix`) | Ghidra decompile | Record/replay: recorded inputs/outputs from the running game (as `POP_CAPTURE_ADPCM`) | Faster porting of loaders/mixers | Capture wrappers per function |
| 6 | **Texture assignment per element** | Element UVs, material records, texture keys | UVs fit the texture size; rendered element matches the GS frame | Route D textures | After #3 |
| 7 | **Function naming** (running now) | Decompiles | Calibration against hidden prototype names (meaning, not exact text) | Navigation; weak | Running; verdict pending rescore |
| 8 | **Log triage** | Long boot logs | Diff against a known-good baseline log (new warnings, new missing targets) | Spot regressions after rebuilds | Easy; mostly a script, model only to summarise |

Test results (14 Oct 2026, gemma4:26b on the PC's 5070 Ti)
- Naming (#7): 60 calibration functions: ~10 right meaning, ~13 partly, ~29 wrong, 8 empty (JSON repetition loop; fixed with num_predict 400, repeat_penalty 1.15, temperature 0.2, name salvage). ~100 functions/hour. Not trustworthy without review.
- MMI (#1), name only (model must recall the instruction): 1 of 5 pass; it does not know these instructions (PADDUH as plain word add, PMULTUW as halfword products).
- MMI (#1), with a one-paragraph spec per instruction: 13 of 18 first round (12 on the first attempt), 18 of 18 after making 5 specs explicit (lanes "words 0 and 2 only", sign extension spelled out as 0xFFFFFFFF/0). 8-35 s per passing instruction. Vague wording ("sign extension of q") fails consistently; the diff feedback rarely rescues a misunderstanding.
- Lesson: the model is a reliable *spec-to-code translator* under a strict judge, not a source of knowledge. The valuable human/Claude work is writing precise specs; the model does the volume.
- PC-only run validated: tools/windows/llm-job.ps1 via start-job.ps1 (Ollama local, MSVC + real SSE), Mac button tools/overnight-pc.command asks which job.

Notes
- The PC's GPU is only free when no build or game runs; one job at a time.
- Each job writes one result file per item and resumes after interruption (like name_functions.py).
- Results are suggestions until the judge passes; never applied to the build automatically.

## Candidates noted 16 Oct (Qwen3.6-35B via llama.cpp, generic harness `tools/llm_port/generic/`)
Ranked by value for 1.0. Each has a precise spec (written by us) and a scripted judge, which is
what Qwen needs (staged, medium-sized functions; 1-4 rounds each in the 15 Oct runs).
1. **Live packet decoders -> standalone C++ with a judge**: `livePacketBatches` / `packetPositions`
   in `tools/overrides/pop_native_live.cpp` are hand-written and untested outside the game. Task:
   inputs = DMA chains captured from frames (`POP_DMACAP` captures already on disk), spec = the
   docstrings, judge = the Python `dmacmp.vif_unpacks` + `jade2gltf` decode of the same chain. Gives
   a tested decoder shared by the live path and the viewer.
2. **`tools/chainfar.py` (sky) and the character lighting bake in `tools/chainskin.py` -> C++**,
   judged against the Python output (noted 16 Oct morning; still valid).
3. **The 18 remaining wrong MMI instructions** (spec harness exists, 18/18 with specs on 14 Oct).
4. **GPU ports of the post effects** (`tools/native/src/postfx.h`: ColorDiffusion now, SmartBlur
   next): the CPU code is the spec, judge = image difference of the GPU output against the CPU
   reference on captured frames (PNF5 + renders). Needs a small image-judge driver (the generic
   harness compares text); worth building once SmartBlur is in.
5. **GS blend/state table unit test**: from `docs/native_renderer.md` "Blend state" (the game's
   SetBlending table), write a table-driven test that feeds every flags combination through
   `pop_native::materialFromGsFlags` and checks the GS ALPHA value it implies against the table
   (judge = the table transcribed from the decompile by us). Small; catches regressions.
Not a model job:
- **Audio quality comparison across versions** (user's question): the measurements (sample rate,
  channels, codec, bandwidth from an FFT, noise floor, clipping) are deterministic and best done by
  a script with vgmstream + numpy, run overnight on the PC as a nightly-style job. The model would
  add nothing to the numbers; at most it could word the report, as Gemma does for the nightly. The
  hard part is matching the same stream across archives (PS2 `SOUNDPS2.BIG` vs GC/PC/Xbox
  `.big`/folders): the bigfile keys match across versions for most assets, so try key matching
  first; the listening judgement stays with the user.
- Reverse-engineering new formats/paths (GFX particles `FUN_004584C0`, after-effect halos, UV
  animation matrices): discovery is where the model is weak (14 Oct tests); do it ourselves, then
  hand the volume work over with a spec.

## Status 16 Oct evening
- Candidate 4 (GPU ports of the post effects) is done by Claude, not Qwen: the kernels were small
  (`tools/native/shaders/gs_sprite.comp`, `color_diffusion.comp`), and the judge
  (`pop_postfx_gpu_judge`) needed a known-good shader to be validated anyway. The judge is reusable
  for further GPU post effects.
- Candidate 1 (live packet decoders) proved its point the same day: the hand-written decoder had a
  batch-order bug (colours after positions -> wrong colours, white fronds). Fixed by hand; a
  standalone tested decoder is still worth having, judged on recorded packets.
- No Qwen job of real value was ready for the night of 16 Oct: the remaining 1.0 work is mostly
  discovery. Next big Qwen candidate: **VU1 driver programs (0x4CDE80, 0x4D5480, 0x4D9480,
  0x4DD300, 0x4E0F00) to readable C++**, judged by recorded inputs/outputs: a capture hook at each
  VU1 kick (VU1 data memory + registers before, XGKICK GIF output after), a standalone harness, then
  staged ports per entry point/path. Gives exact lighting (point lights), shading-pass UVs, sky,
  water and particles for the native renderer. Needs the capture hook first (Claude, ~half a day).


## Harness² (Google Research) looked at, 19 Oct 2026: not adopted; one idea taken

What it is (README, github.com/google-research/harness2, Apache 2.0): it refines the *harness* around an
LLM agent (system prompt, guardrails, memory, tool guidance, subagents, skills, scripts, plugins) without
touching the weights. Per task a Propose-Probe-Compose step proposes harness edits, re-runs the same task
with them and composes a refined harness from the behavioural differences, **without ground-truth
feedback**; across tasks it keeps adopted harnesses and "evidence-supported lessons". The agents run in
OpenCode or Codex; it is benchmarked on office/legal/work agent suites (GLM-5, Gemini 3.5 Flash; up to
+11.5 points on WorkBuddy-Bench, +4.5 on JobBench, +11.8 pp partial pass on LAB).

Why not for our Qwen jobs:
- Not offline as published: models are configured through Vertex AI (gcloud auth). Local or
  OpenAI-compatible endpoints (our llama-server) are not mentioned; OpenCode can talk to such an
  endpoint, but whether Harness² works through it is unchecked. The proposer step presumably wants a
  strong model, i.e. the cloud or Qwen refining itself (estimate).
- It solves the no-judge case. Our jobs have exact judges (equivalence tests, exact-size parsing,
  `tools/mmitest`); a strict judge is a better signal than behavioural differences between runs.
- It drives long agent loops with tools and subagents; Qwen does best on small staged
  spec -> code tasks under a judge (above), long loops are where a 35B-A3B model drifts (estimate).
- Linux + Python 3.12 + Node 20 (+ Docker for some benchmarks); the PC is Windows (would need WSL).
  Its executor has unrestricted network access and Codex runs with the sandbox off.

Idea taken (not built yet): **lessons per task type from our own judged runs.** Each failed round
already leaves the spec, the model's answer, the judge's reason and (eventually) the passing answer.
A script collects these into short, evidence-backed lessons per task type (e.g. "Ghidra's CONCAT44
means ...", "lh sign-extends: spell it out", "words 0 and 2 only") and prepends the relevant ones
to later specs. This matches our own finding (14 Oct) that precise wording decides success. A lesson
stays only if it measurably helps: re-run a fixed set of past tasks with and without it and compare
first-round pass rates. Fully offline; the judge keeps it honest.

## VU1 water program P6 for Qwen, 19-20 Oct 2026 night (unfinished: 13 of 24 stages pass)
Harness `tools/vu1port/`: `replay.cpp` (vu1replay: the runtime's VU1 interpreter replaying the `PS2X_VU1_TRACE`
water traces, x86_64; writes per-stage cases and hints), `cases.sh <stage> <entry>:<exits>`, `judge.cpp` (exact
pc/vi/clip, 4 ULP on floats, ASan, round toward zero, no FMA contraction), `port.py <stage>` (spec = API +
VU primer + annotated disassembly + the hand-written setup stage as the example; up to N rounds with the
judge's report), `port.py --chain` (all passing stages in one program behind a pc dispatcher, judged on the
600 whole runs; not run yet: stages missing). Work files `game/analysis/vu1port/`, results
`game/analysis/qwen/vu1_<stage>/` (spec.md, attempts/, best.cpp, PASS). Hand ports: `tools/vu1port/known_good/`.

**Passing (re-judged 22 Oct on the current cases):** setup (by hand, 600 cases), a_pre (Qwen round 1),
a_g1_row (Qwen r5), a_g1_tail / a_g2_tail / b_g1_tail / b_g2_tail (by hand; the four loop tails, Qwen failed
their delay slots), a_g2_head (r1), tail (r1), b_pre (r2), b_g1_row (r2), b_g1_in2 (r6), b_tail (r1).
**Not passing** (best attempt, failing / total cases): a_g1_in1 348/2484, a_g1_in2 all, a_check all 174,
a_g2_in1 all, a_g2_in2 all, a_g2_in3 90/1620, b_g1_in1 810/2484, b_check 60/174, b_g2_head all 666,
b_g2_in1 all, b_g2_in2 all (its rounds 4-6 were lost: the PC's llama-server went away during the night).

What we learned about the model on this task:
- **Size limit ~25-35 pairs.** Whole grids (110+ pairs) and inner loops (51-75) used the whole 32k-token
  budget reasoning and produced no code, twice in a row each; "reason briefly" in the retry rarely helps.
  Stages were split at loop heads, then inner loops at points where no DIV result is pending and no FMAND is
  separated from its MAC source (`replay.cpp` reports what is in flight at every candidate border). About a
  third of all rounds still overran.
- **Spec fixes that mattered:** the ACC forms spelled out (it read ADDAi as ACC += a * I), broadcast letter vs
  destination mask with examples (SUBx.z writes only .z), delay slots marked on their lines, and an explicit
  rule for an exit that is a loop branch's delay slot (taken: run the slot here, not taken: stop before it).
  After each, the next small stages passed in 1-2 rounds.
- **Feedback loop is weak:** rounds after a failure often repeat the same mistakes; the stages that passed late
  (r5, r6) improved slowly. A fresh start with a fixed primer beat continuing the old conversation.
- **Harness fix found by the hand ports:** cases now record Q as it is once a pending DIV lands (P6's inner
  loops end with a DIV in flight; nothing reads Q before it lands), else a correct port fails on Q.
- Throughput: ~68 tokens/s, 3-10 min a round, one request at a time; the 11 unfinished stages are the big
  arithmetic ones (vertex transform, perspective divide, colour, packet stores).

Next (to decide with the user): hand-port the 11 remaining stages (they are mostly repeats of one vertex
pattern: the passing a_g1_row / b_g1_in2 show it), or split them further for Qwen; then `port.py --chain` and
the whole-run judge; then wire P6 into the native renderer (water, renderer step 3).

**22 Oct: split again and moved to the PC alone.** The nine failing inner-loop stages were cut into 19 pieces
of 9-20 pairs (`port.py` STAGES `*_in1a` etc.; cut points from scanning `vu1replay --stage <entry>:<cut>`:
no DIV pending, no FMAND apart from its MAC source); the two clip-check stages stay whole and run last.
`port.py` now takes as the example the nearest earlier stage that passed (the same loop's code, not the
setup stage), starts a fresh conversation every N rounds (`--fresh 3`) and stops at a deadline (`--until`);
the judge builds with MSVC on the PC (`judge.cpp`: watchdog thread instead of `alarm`). Night job
`tools/windows/oct22.ps1` (desktop shortcut "Qwen overnight (then shuts down)"): two passes of 6 rounds,
`--chain` when everything passes, then the PC shuts down; log `game\jobs\qwen-night.log`.

**22-23 Oct night: 9 of 21 pieces passed** (job 21:56 -> 07:30 cut-off, then shut down as designed; the 18:52
entries at the top of the log are a test start with `-Until 18:56`). Passed: a_g1_in1a, a_g1_in1b, a_g1_in2a,
a_g1_in2b, a_g2_in1b (pass 2), b_g1_in1a, b_g2_head_a, b_g2_head_b, b_g2_in2b. Not passed: a_g1_in2c,
a_g2_in1a/in2a/in2b/in3a/in3b, b_g1_in1b, b_g2_in1a/in1b/in2a, a_check, b_check. Pass 2 reached only 4 stages
before 07:30 (~30-50 min per failing stage); `--chain` did not run. Leads, not yet examined: **b_check
"not passed" in under a minute** (04:54 -> 04:54: setup or judge failure, not the model); the a_g2 group fails
almost entirely (5 of 6); my `dir` for `vu1_p6_*PASS*` found nothing (check the actual result file names).
Next (user, 23 Oct): dig into the per-stage logs/transcripts under `game\analysis\qwen\` for why each failed,
then improve tonight's run (more time per stage? smaller cuts? better examples? the checks fixed?).

**Post-mortem (23 Oct lunch).** Most of the night was lost to the harness, not to hard stages:
- **71 of 208 rounds so far (34%) wrote no code**: all 32,768 tokens went on thinking (~8 min each, the
  longest rounds; llama-server ran with `--reasoning-budget -1`). Fix: this llama.cpp build (b11643) has a real
  budget, `--reasoning-budget N` + `--reasoning-budget-message`; tested at N = 300: thinking is cut, the
  message injected, the answer follows. Now 16000 (`llama-serve.ps1`; `oct23.ps1` rewrites
  `game\jobs\llama-serve.cmd`, which the LlamaServe task runs, so no re-registering). port.py also judges a
  file drafted inside the reasoning when the answer has none, and saves `roundNN.nocode.txt` (end of the
  reasoning) for later post-mortems.
- **`--rounds` was the round number to stop at, not a count**: b_check already had rounds 1-6 from an earlier
  night, so it got none ("not passed" in under a minute: not a setup failure); a_check got one. No-code rounds
  were not counted either (resume looked at `.cpp` files only), so a no-code round06 was redone each run. Now
  `--rounds N` = N more rounds; `oct23.ps1` passes 6 per pass.
- **The PASS marker** is a file `PASS` inside `vu1_p6_<stage>\` (holds the round name), not a file name.
- **Example code with a MAC bug, labelled bit-exact**: a_g2_head and a_g2_in1b (the example for every later
  a_g2 stage) set sign bits with `0x80 >> (3 - c)` (x -> 0x10, w's bit). The judge does not compare MAC
  (`judge.cpp`), so it passed. a_g2_in2a reads MAC (`FMAND` at 0xb38). Fixed in both `best.cpp` (`0x80 >> c`).
- **Still open, a_g2**: in1a/in2b/in3a/in3b read no MAC and fail all 1620 cases with the same shape (a_g2_in1a
  round10: vf11 expected 0x00000080 in all four lanes (an integer), got floats; vf16, Q and one data qword wrong),
  so something they share (the vf11 input/conversion, or the loop's start) is mistranslated. Not examined
  further; first thing to look at if a_g2 fails again tonight.

**Night 23-24 Oct (`oct23.ps1`, 12 stages, 2 passes of 6 more rounds): 4 passed, 13 of 21 in all.** Passed:
a_g2_in1a, a_g2_in2b, b_g2_in1b, b_g2_in2a. Left (8): a_g1_in2c, a_g2_in2a, a_g2_in3a, a_g2_in3b, b_g1_in1b,
b_g2_in1a, a_check, b_check. The harness fixes held: 2 no-code rounds of ~130 (both a_g1_in2c, 32,768 tokens
despite the 16k thinking budget), rounds counted right. Failure shapes from the last `attempts\roundNN.report.txt`:
- **Stuck on a few cases, "expected 0, got small integer bits"**: a_g2_in3a and in3b at exactly 90 of 1620
  (cases 13, 27, ... = one case in ~14; vf13/vf27 resp. vf05/vf28 expected 0x0, got e.g. 0x1fe0, 0x6bff, 0x100),
  a_g2_in2a down to 54/90 at times, b_g1_in1b at 48 of 2484 (vf27 expected 0, got 0xff/0xa7/0xa7/0x34). The
  same class across stages, so most likely one shared cause in the trace or spec (a path or load that leaves
  zeros in the real run), not four separate model errors. First thing to examine.
- **a_g1_in2c, all 2484 fail**: vf13 expected raw integers (0x00000fe4 = 4068) but got the float 4068.0, and
  data qwords 0x49/0xa0 wrong: the port converts (ITOF) where the program keeps integer bits, or the spec
  suggests it. Probably a quick hand fix.
- **b_g2_in1a, all 1620 fail every round**: vf18 far off (expected ~70815, got ~-500430) and stray .w writes to
  data qwords: a real mistranslation; hand-port or smaller cuts.
- **a_check / b_check, 174 of 174 every round, both nights**: exit pc wrong (0x6f0 vs 0xcf0), ~560 differences,
  several compile failures and one endless loop: too big for the model as one piece. Cut them up or hand-port.
