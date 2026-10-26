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

## ReAgent + ghidra-bridge (Dryxio) looked at, 24 Oct 2026: not adopted; two ideas for the next Qwen job

Decision (user, 24 Oct): finish the P6 water job first; build the ideas below into the **next** Qwen
project, not into the running P6 harness.

What it is (github.com/Dryxio/reagent + github.com/Dryxio/ghidra-bridge, both MIT; read, not run):
ReAgent is a reverser -> checker -> fix loop that turns Ghidra decompiles into C/C++ source (profiles
for gta-reversed, OpenRCT2, windows-x64; backends Claude API/CLI, Codex CLI, any OpenAI-compatible
endpoint). A candidate passes on: LLM checker PASS + a structural heuristic + the project's build/tests
+ 11 heuristic "parity" signals; its README says this is not semantic equivalence. ghidra-bridge is a
pyghidra headless exporter (`exporters/runner.py`, ~650 lines) writing JSON once per project: per
function decompile, signature, callers, callees, assembly, normalized high P-code, basic-block CFG;
program-wide structs/enums/typedefs, vtables, strings and named globals with their referencing
functions. `ghidra-bridge context <addr>` bundles one function's callers/callees/strings/globals/CFG;
ReAgent puts that in the prompt next to the decompile (each item capped at 8k characters).

Why not as a tool:
- No need for whole-game source: PS2Recomp already gives working C++ for every function.
- Its judge is weaker than ours (LLM verdict + heuristics vs execution against recorded cases).
- The hard ports (P6, character VU1 program) are VU1 microcode, which Ghidra does not decompile.
- On our R5900 ELF the exporter loses much of its value: callees come only from direct call refs
  (`jal`), so Jade's virtual calls through `jalr` are missing; vtables are found only by MSVC symbol
  names (`??_7...@@6B`) or names containing "vtable" (we have none); globals named `DAT_`/`BYTE_` are
  skipped (most of ours). Callers/callees/strings we already export with
  `tools/ghidra/PoPNamingContext.java`. Asm, P-code and CFG would work (architecture-independent).

Ideas to build into the next Qwen job:
1. **Bounded evidence requests** (ReAgent `agents/reverser.py:176-257`, `prompts/reverser_system.md`):
   instead of code the model may reply with only `{"actions":[{"tool":"decompile","target":"0x..."}]}`;
   the harness answers from pre-exported data (read-only), the model continues, within a fixed budget
   (theirs: 8). For Qwen keep it small (2-3 requests; tools limited to decompile / asm / xrefs / a
   global's value of a named address): discovery and long loops are where it is weakest (14 Oct tests),
   and 34% of the 22-23 Oct rounds used up their thinking budget without code. Measure it like the
   lessons idea above: first-round pass rate on a fixed task set with and without requests.
   Fixes the "spec forgot what callee X does" failure without losing a night.
2. **One bulk export of the Ghidra project to JSON** (our own script, an extension of
   `PoPNamingContext.java`, not ghidra-bridge): per function decompile, asm, callers, callees, **plus
   `jalr` sites and constant-address loads/stores** (gp-relative included). Specs and evidence answers
   are then assembled on the Mac without a headless Ghidra run per job.
3. Our advantage over ReAgent, to use: **global values from a save** (`tools/popstate.py` reads RAM,
   so a `DAT_` address gets its real value and, via a type guess, its meaning) and **cases recorded
   from the running game** (capture wrapper per function; job #5 in the table above). ReAgent's
   `verification/differential.py` is the same reference-vs-candidate JSON shape but on hand-supplied
   cases.

Shape of the next job (proposal): a generic "game function -> standalone C++" Qwen job. Claude picks
the function and writes the spec; the export (2) + save values (3) give the evidence; a capture
wrapper records input/output cases from a normal boot; `tools/llm_port/generic` runs it with the
evidence-request loop (1) and the recorded cases as the judge.

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

**24 Oct morning: 6 of the 8 fixed by hand from Qwen's own attempts, the checks split; 9 pieces for tonight.**
- **The "expected 0" class was the VU's float normalisation**, missing from the primer: the runtime's VU1
  interpreter (`ps2_vu1_impl.inl`: `vnorm`, `normalizeOperand`, `normalizeFmacExactResult`) reads every upper
  operand with exponent 0 as +-0 and exponent 0xff as +-0x7f7fffff, and flushes FMAC results the same way.
  These stages run MAX/MINI/SUB on lanes holding integer bits (colours, counters: 0x000000ff is a denormal).
  Adding that to Qwen's best attempts (round numbers in each file's header) passed a_g2_in3a, a_g2_in3b,
  a_g2_in2a and b_g1_in1b. The primer (`port.py` PRIMER) now states the rule.
- **a_g1_in2c**: round24 also stored through the wrong address register (SQ used vi[source vf]) and wrote
  FTOI4 results as floats; fixed. **b_g2_in1a**: round16 wrote ISW as one byte (a 32-bit word) and computed
  MADDw as vf18 += (ACC + ...); fixed. All six are in `tools/vu1port/known_good/` (counts as passed).
- **a_check / b_check split** (`port.py` STAGES, cases recorded with `cases.sh`): head (setup), loop (vertex
  loop to the clip test), next (loop end / exit to the tail), cull (clip-flag tests), sub (triangle test
  subroutine 0x1568 / 0x2550, JR vi15 back) and ret (2 pairs). a_ck_next, a_ck_ret and b_ck_ret hand-ported
  (they pass: the end-to-end check of the new cases); 9 left for Qwen: a_ck_head/loop/cull/sub,
  b_ck_head/loop/next/cull/sub. Night job `tools/windows/oct24.ps1` (passes of 6 rounds until 07:30, then
  `--chain` if all pass).
- Lesson: when several stages fail the same few cases the same way for many rounds, judge a patched attempt
  by hand before giving Qwen more rounds: here one missing rule in the primer cost ~60 rounds over two nights.

## VU1 sprite program P9, 24 Oct 2026 evening (set up; Qwen runs it after the water pieces)

Why: the sprites (GFX particles drawn as strips along a curve) run P9 on the runtime's VU1 interpreter in
`spriteRecord` (`tools/overrides/pop_native_live.cpp`), which is slow in busy scenes; a C++ port removes it.
P9 = ELF MPG packet 0x4E338C, 0x13A0 bytes (628 pairs, 563 run in the recorded scenes), image hash 04994ae7ba41b01f.

- **Cases without booting the game.** `pop_state_frame_vu1` with `POP_P9_TRACE=<file>` (`tools/native/src/p9trace.cpp`,
  through a hook in `spriteRun` that is null in the game) records every P9 run of a save in the runtime's
  `PS2X_VU1_TRACE` format. ~130 saves (shadow/ab/in, saves1024b-f, play1021) give 2,984 runs (1,492 MSCALF setups at
  0x340 + 1,492 MSCNT runs at 0x390), 13.6 MB, replay deterministic. `tools/vu1port/cases_p9.sh [--record <dirs>]`
  records and cuts; cases spread evenly over the trace with `vu1replay --stride`, at most 600 per stage
  (`game/analysis/vu1port/p9_*.cases`, 739 MB; also copied to the PC), whole runs in `p9_whole.cases` for `--chain p9`.
- **37 stages** (`port.py` STAGES_P9), cut at subroutine entries/returns and branch joins: a caller stage executes its
  BAL and delay slot and stops at the subroutine entry; JR stages return to `vi[N] * 8`. Exits must be pairs the
  program arrives at, never delay slots (a delay-slot exit gave "ended inside").
- **What may be in flight at a boundary** (`vu1replay --allow`): xgkick (the judge takes a kick's packet at the
  XGKICK), MAC and status (P9 has no status reader; `cases_p9.sh` checks that no FMAND reads MAC flags from before
  its stage's entry: 0), and Q (four boundaries have a DIV in flight; every Q reader after them reads that DIV's
  result, so the snapshot's landed Q is right). P6 keeps the strict default.
- **The R register** (RINIT/RNEXT/RGET/RXOR, used by P9 only): `VuState.r` behind `VU_TASK_R` (P6's binary cases
  and prompts stay byte-identical, checked on all 44 P6 prompts), the replayer `vu1replay_r`, the judge
  `/DVU_TASK_R=1`; the primer gets the R semantics (LFSR step) for P9 only. P9 prompts show LOI constants as exact
  bits (vudis rounds them).
- **Hand ports as the end-to-end check:** `known_good/p9_setup`, `p9_init` (the two 4x4 matrix products), `p9_main`
  (RINIT): all pass 498 cases, on the Mac and with MSVC on the PC; a port with one wrong RINIT bit fails all 498.
- **Qwen test (24 Oct 21:24-21:46, `--rounds 3`):** 3 of 3 stages passed, 600 of 600 cases each: `p9_d_head` (FMAND +
  JR) in round 1 (252 s); `p9_loop` in round 2 (round 1's only error: the BAL return value, vi15 = 84 instead of
  0x420 / 8 = 132, so prompts now state each BAL's value); `p9_c_a` (36 pairs: the curve maths, RNEXT, FTOI, integer
  stores) in round 3 (round 1 maths ~1% off, round 2 maths right but ISW lanes wrong). ~70 tokens/s, 1-6 min a round.
  These passes count for the night job. Stop test: "Stop Qwen" mid-round left no python / llama-server, GPU idle.
- **Night job** `tools/windows/oct24.ps1` (shortcut "Qwen overnight (then shuts down)"): the water's 4 passes as
  planned, then passes **without a deadline**: each unpassed P9 stage in program order (3 rounds), then the P6
  leftovers (6), `--chain` per program once it passes; llama-server checked before every stage (the LlamaServe task
  has a 24-hour limit). Shuts down only if everything passes. **Cancel at any time with the desktop shortcut
  "Stop Qwen"** (`tools/windows/qwen-stop.ps1`: ends the job and llama-server, never shuts down); every round is on
  disk as it finishes, so the next run resumes.

## Night of 24-25 Oct results; P6 and P9 finished by hand (25 Oct 2026)

Night job `oct24.ps1`, 21:47-10:19 (stopped by the user with "Stop Qwen"). Qwen passed 32 of the 37 P9 stages and
1 more P6 piece (`a_ck_cull`); 6 rounds without progress on the rest.
- **Two P6 pieces could never pass (harness bug).** In variant B's clip check a CLIP sits 1-3 pairs before the
  borders 0x19c8 / 0x19f0 in every case, and the case snapshot took the *visible* clip flags (before that CLIP
  lands) while Q was already taken as landed: `b_ck_loop` expected a CLIP result its code never makes, `b_ck_next`
  one fewer than its code makes (each "got" was the next case's "expected"). 26-28 rounds each over two nights.
  `b_ck_head` had "passed" by computing the CLIP and then setting `vu.clip = 0` before returning (judge gaming).
  Fix (`replay.cpp` snapshot): clip flags as landed (`m_workingClip`), like Q; the cutter counts clip reads that
  would see older flags than the landed ones (`clip flags read before a CLIP in flight at the entry landed`; none
  in P6). Recut, then **every earlier attempt re-judged: b_ck_head 13 of 14 attempts pass (only the cheat fails),
  b_ck_next passed in round 1, b_ck_loop in round 2.** Lesson: when a stage fails for many rounds on one value,
  check what is in flight at its borders before blaming the model.
- **Hand ports** (`known_good/`, all first try): `p6_a_ck_head` (174 cases), `p9_c_b` (600), `p9_d_save` (596),
  `p9_d_loop_b` (598), `p9_f_a` (599), `p9_f_b` (599). Qwen's misses were ordinary: wrong MAC lane bits for the
  FMAND tests, ITOF applied to float bits, ISW lanes packed.
- **Whole programs:** `port.py --chain p9` **PASS 598 whole runs**, `--chain p6` **PASS 600**.
- **Speed** (`judge --time N`, built with `VU1_TIMING=1 build.sh`: -O2, no sanitizer; x86 on the Mac under
  Rosetta, both sides alike): P9 ~27 us per run vs ~5 ms in the interpreter (`vu1replay_r` over the 2,984 runs:
  15 s), ~190x; P6 ~3 us vs ~1.8 ms. To be measured in the game.
- A scan of every passed port for constant writes to clip / MAC / Q at the end found only the `b_ck_head` cheat.

## VU1 character program P4 for Qwen, 25 Oct 2026 (set up; the night job `oct25.ps1`)

Why: the next candidate after the water (CLAUDE.md); P4 = image 31829861c88dc716, the skinned, lit character meshes
(`docs/native_renderer.md` "Point lights decoded"). The judge harness is the P6/P9 one; new:
- **Stage borders by tool, not by hand.** `vu1replay --cutscan <file> --hints <file>` records per pc, over all runs:
  the kinds in flight before it, whether it is ever a delay slot, and whether it lies between a MAC flag write and
  its FM* reader, or after a later clip write than the one an FC* reader sees (clip flags are landed in the
  snapshot, so only that case is wrong). `tools/vu1port/cutscan.py <image> <scans>` builds basic blocks, makes every
  run start (0, the pair after each E pair's delay slot), subroutine entry, return point and branch target from
  another stage an entry, joins fall-through blocks and blocks that may not start a stage into their predecessor,
  and cuts at most 16 pairs: **79 stages, 695 pairs, every entry safe** (the first proposal had two loops of 33-34
  pairs only because the clip rule was as strict as the MAC one).
- **Cases:** `tools/vu1port/cases_p4.sh` from the three play traces that ran P4 (091049, 091734, 192521: 4,836
  runs), at most 600 per stage spread over the traces; `p4_whole.cases` from each run start to the program end;
  the script ends with a border check (no "(!)" kind, no early clip read).
- **Primer:** WAITQ added (a no-op for a port). JR / BAL notes now for every program after P6.
- **Hand port** `known_good/p4_0000` (the MSCAL entry) as the first example and end-to-end check.
- **Branches never taken in the traces** (e.g. 0x38 IBGTZ to 0x58, the dispatcher's 0x2ae0) are not exits; a port
  returns with that pc, so the game integration must fall back to the interpreter when the chain stops at a pc
  without a stage (true of P9 and P6 too).
- **Night job** `tools/windows/oct25.ps1` (desktop shortcut "Qwen overnight (until Stop Qwen)", now pointing at
  it): passes over the unpassed P4 stages, 3 rounds each, `--chain p4` once all pass; ends by itself after two
  passes in a row with no new pass, then shuts the PC down. "Stop Qwen" cancels as before.

## Night of 25-26 Oct results; P4 finished by hand (26 Oct 2026)

Night job `oct25.ps1`, 22:22-10:43 (stopped with "Stop Qwen" during pass 3: retries had become byte-identical,
e.g. `p4_0c80` rounds 7-9 the same file). Qwen passed **67 of 79** P4 stages (pass 1: 78 -> 23 left, pass 2: 14,
pass 3: 12). The 12 left, all hand-ported first try (`known_good/p4_*`): three 4-instruction ILW/IOR/ISW stages
(`0ee8`, `3408`, `39e8`: Qwen read `-1(vi6)` as a byte offset every round), the bone-matrix stages (`0c80`, `35b0`,
`3630`, `3730`, `06a8`), `1598` / `15f0` (DIV, CLIP, FTOI4), the light loops `3dc8` / `3ee0` (RSQRT).
- **Sampled cases hid two bugs in "passed" ports.** `--chain p4` then failed 18 of 673 whole runs, all near the
  float limit. To find the stage: `vu1replay --records <the report's trace records>` (new, 26 Oct) records every
  case of just those runs, then each passing port is judged on them.
  1. **The largest float written as an integer**: `return sign ? -0x7f7fffff : 0x7f7fffff;` (= ~2.1e9) in 8
     ports, and `*(float*)(sign ? 0xff7fffff : 0x7f7fffff)` (a read from that address: a crash on the first
     overflow) in 6, among them **`p9_e_a`, in the shipped `ported/p9.cpp`**, and `p6_b_ck_sub`. 14 ports in all
     (12 P4); fixed in place (`best.cpp`, originals kept as `best.cpp.orig`), every one re-judged PASS.
  2. **Product-sums judged on the exact value.** The reference (`ps2_vu1_impl.inl` `normalizeFmacExactResult`)
     clamps MADD/MSUB on the long-double sum, not on a clamped product plus ACC, and flushes a denormal Q to 0.
     The primer said "product first (rounded), then the add", so Qwen clamped the product: `p4_0cd8` (passed 600
     sampled cases) failed 3 whole runs; hand-ported.
  After both: **`--chain p4` PASS 673 whole runs; `--chain p6` PASS 600 and `--chain p9` PASS 598** (P6 and P9 again
  with the fixed ports). `ported/p9.cpp` changed: the game picks the fix up at its next build.
- **Harness changes for the next program (P5):** the primer now gives `norm()` / `fix()` verbatim (exact product-sum,
  DIV/RSQRT through `norm`, the largest float as `3.40282347e+38f`); the judge rejects the integer-as-float and
  pointer-read forms before compiling (catches all 14 originals, no false hits on the passing ports).
- Still possible: other overflow-only differences in Qwen ports that the three traces never reach. When the game
  runs these ports, a whole-run check against the interpreter on new traces (as above) is the safety net.

## VU1 particle program P5 for Qwen, 26 Oct 2026 (done: Qwen 82 of 87 stages, 5 by hand)

Why: after P4 the next VU1 program by value for 0.13 (particles, tied to the `-NativeLate` glow pop). P5 = ELF MPG
0x4D948C, code 0x0-0x17c0 (760 pairs, 716 run in the traces); no R register, no EFU; 22 BAL / 8 JR subroutine calls,
13 CLIPs with FCOR / FCAND tests, 3 E pairs.
- **Several image hashes.** P5 shares VU1 micro memory with other programs, so its runs carry 7 hashes (every
  recorded image holding P5's code unchanged: `port.P5_HASHES`); `vu1replay --only` takes a list now. ~930 runs over
  the same three play traces as P4 (no new recording needed).
- **Stages:** `cutscan.py --prefix p5` (new option): **87 stages, 716 pairs, longest 16**, 32 required entries, no
  unsafe entry, no orphan block. Cases `tools/vu1port/cases_p5.sh`: 40-600 per stage, border check clean;
  **`p5_whole.cases` = every recorded run (930)**, not 100 per run start: whole runs are what caught P4's overflow bugs.
- **Hand port** `known_good/p5_0000` (the MSCAL entry, PASS 465): the end-to-end check and the first example, with the
  exact `norm()` / `fix()` helpers.
- **Harness changes from the P4 morning:** primer helpers and the reject check (above), and a **fresh seed per request**
  (`tools/llm_port/meshdecode/port.py` `ask`): the 25-26 Oct retries with an unchanged conversation came back
  byte-identical, so rounds after the first in a pass were wasted.
- **Night job** `tools/windows/oct26.ps1` (shortcut "Qwen overnight (until Stop Qwen)" repointed): as oct25.ps1 for
  P5; ends by itself after two passes with no new pass, then shuts down. Morning: `--chain p5` on the 930 runs, and a
  look at any stage that passed only on its sampled cases (`vu1replay --records` on failing whole runs).
- **Expected run time (estimate, from P4):** P4 went 79 -> 23 stages left in pass 1 (6 h 50), -> 14 in pass 2 (3 h 20),
  -> 12 by mid pass 3. P5 is about the same size (87 stages), so pass 1 ~7 h, pass 2 ~3 h; the job usually stops on
  the two-idle-passes rule ~12-14 h after the start, or "Stop Qwen" in the morning with ~10-15 stages left by hand.
- **Run (26 Oct, started 11:38, daytime):** by 15:24, pass 1 had 54 of the first 64 stages passing (10 failing,
  mostly 0x04b8-0x06b8). The user stops it with "Stop Qwen" when they want the TV and restarts it for the night
  after checking whether it is still worth it.
- **Result (stopped 20:22, "Stop Qwen"):** **83 of 87 pass** (82 Qwen + the hand-ported `p5_0000`). Pass 1 (to
  16:43) left 13, pass 2 left 9, pass 3 left 4 (5 new passes there: the fresh seed per request works), pass 4 nothing
  new. Left for hand-porting: `p5_02c0`, `p5_04b8`, `p5_1550`, `p5_1700` (each failed 12+ rounds). Results copied
  to the Mac (`game/analysis/qwen/vu1_p5_*`). Not run overnight: 4 stages that failed 4 passes are cheaper by hand.
- **The last four by hand (26 Oct evening), `--chain p5` PASS 930 whole runs** (`tools/vu1port/ported/p5.cpp`, not
  in the game yet). P5 is done.
  - `p5_02c0` could not pass: **its exits were wrong.** The subroutine at 0x240 is called with `BAL vi15` (0x8f8,
    0xf20) and copies the link into vi1 (`IADD vi1, vi0, vi15`), so its `JR vi1` at 0x330 returns to 0x908 / 0xf30.
    `cutscan.py` takes JR targets from the BALs that write the same register, so it gave the returns of the
    `BAL vi1` calls (0x1378, 0x1468, 0x14c0); the recorded cases then ran on past the stage. Exits fixed by hand in
    `port.py`, cases re-recorded (`cases_p5.sh p5_02c0`). Lesson: a JR through a register a subroutine copies the
    link into needs its exits checked by hand (cutscan does not follow the copy).
  - `p5_04b8`, `p5_1700`: `MULAz.w ACC, vf0, vf31z` reads vf0's **w** lane (1), so ACC.w = vf31.z, not 0 (the same
    lane rule the primer states; my first hand port got it wrong too, Qwen in every round).
  - `p5_1550`: a counted MR32 loop; Qwen rotated the wrong way or stopped after one iteration.
- **P5 is the last VU1 program to port:** P0/P2 are VU0, P3 unreachable, P10 dead (26 Oct,
  `docs/native_renderer.md` "VU1 program inventory"). The next Qwen batch, if any, is not VU1 microcode.
