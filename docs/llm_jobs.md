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
