# Lessons from recompiling a PS2 game

What this project (Prince of Persia: The Sands of Time, PS2, with PS2Recomp) has learned, written for anyone
starting their own decomp/recomp: the mistakes we made early, what worked, and what we would do from day one.
Kept up to date as we go (started 18 Oct 2026). Our own details live in `CLAUDE.md` and `docs/`; this file
keeps only what carries over to other projects.

## Before you start

- **Know what you own and what you may use.** Work from your own disc and keep the ISO, ELF, game data and
  generated code out of git (`.gitignore` them from the first commit). Decide provenance rules early: no
  leaked SDK source, headers or samples, and do not write SDK code from memory either. Another project had to
  revert a whole movie player for this. Leaked engine source is off limits too.
- **Look for symbols before anything else.** Preview and prototype discs are often unstripped. Our prototype
  ELF gave names to most of the engine through function matching, which turned weeks of guessing into lookups.
  Check every prototype you can find (other games on the same engine help less than you would hope: their code
  has drifted).
- **Survey other projects first, and again whenever you narrow down a problem.** Dozens of PS2 recompilations
  exist (unbaked.games, recomp.fyi). Read them for every layer, not only the one you are working on: we read
  them for renderers and missed, for a while, that several had already compiled VU0 microcode, the likely fix
  for our slowdown. A two-minute grep of their READMEs saves days. Keep the READMEs locally and grep them.
- **Pick the runtime base by what it already does, not by stars.** Fetching all forks of the recompiler and
  comparing them (fixes, renderers, VU1 compilation, save states) moved us from 2 fps to playable in a day.

## Getting it to boot

- **Expect hangs, not crashes.** Most early blockers were the game waiting forever on something the runtime
  only pretended to do: an IOP module that never started, an RPC server never registered, a CD callback never
  fired. Log every kernel call, semaphore and RPC bind; when the frame counter stops, dump the last calls.
- **Distrust the recompiler's built-in stand-ins.** PS2Recomp replaces any function its analyser names
  (libvu0, `rand`, the remote sound library) with its own C++. Several were wrong, and one left the game
  silent. Rename those functions before recompiling so the game's own code runs, and check every stand-in.
- **Overrides: hook by address, and never trust code after calling the original.** On a scheduler that can
  unwind a guest call and resume later, code after the original may run at the wrong time. Read what you need
  first. A handler bound to an address must advance the PC itself.
- **Write an instruction test early.** A small harness comparing the recompiler's translation of each
  MMI/FPU/VU0-macro instruction against a reference model found many wrong translations at once (and the FPU's
  round-toward-zero and denormal behaviour). Silent arithmetic errors otherwise surface as odd gameplay weeks
  later.
- **Save states early.** They turn a 6-minute route into a 2-second load and make every bug reproducible. Know
  their limits (ours: no sound after a load, no load-time code runs).

## Debugging and measuring

- **Make everything switchable by environment variable and scriptable.** Scripted pad input with timings,
  screenshots at given frame numbers, frame/DMA captures on request, per-subsystem logs. Unattended runs then
  test real game paths overnight.
- **Measure before guessing.** A sampling profiler inside the process (suspend each thread, record the stack,
  symbolize offline) costs a hundred lines and answers "where does the time go" for any build. Our first guess
  for the slowdown (graphics) was wrong: the frames were not bigger.
- **A save state is a complete snapshot of the frame.** Its memory holds the draw lists, the DMA chain, the
  texture table, the camera and the light sets. Reading it offline answers many questions without running the
  game, and lets you replay the renderer's code against any saved scene in a second.
- **Judge pictures with numbers.** Mean pixel difference against the emulated output of the same frame, over a
  fixed set of scenes, catches regressions that eyes miss and shows real progress.
- **Separate "game too slow" from "sound broken".** Our audio judder followed the frame rate. Fix speed first.
- **Check that a measurement ran with the settings you think.** A launcher that sets debug switches in its own
  process leaked them into every later run of the same job: a night's fps comparison ran with the VU capture
  hook and a "check every VU0 run against the interpreter" mode still on. The profile gave it away (the
  verifier showed up in the hot functions). Restore the environment after each run, and log the effective
  switches next to every number.
- **"Waiting" in a profile is a lead, not a dead end.** After the VU microcode was compiled, the game thread
  spent most of a slow scene waiting for VU1 while the VU1 thread was mostly idle: the runtime was modelling
  the console's VU1 speed (a real PS2 cannot run those scenes at 60 either). Ask what the wait is for.
- **A sampler that lumps everything outside your executable together hides where threads are.** "Outside the
  image" can be kernel waits, the graphics driver or the C runtime's memcpy, and the report cannot tell them
  apart; name the module (or read the inclusive view: which of your functions called out).

## Replacing hardware with native code

- **Replace at the engine's level, not the hardware's.** Hook the engine's draw calls, sound calls and movie
  player instead of emulating GS, SPU2 and IPU. Keep the emulated path as a reference you can switch to and
  compare against; it is the judge for everything native.
- **Port the game's own logic, from its own code.** Blend state from the engine's own blend table, UV scales
  from the VU1 microcode's constants, lightmap meaning from the game's own CLUT in GS memory. Every time we
  inferred instead of reading the code, we got it subtly wrong (UV scale 1/4096 instead of 1/512; a lightmap
  read as brightness when it encodes darkness).
- **Decode from memory as the hardware receives it**, not only from files: after load, data is patched (block
  indices become pointers), and save-state scenes never run the loaders at all.
- **Keep a slow, simple reference next to every fast path.** CPU reference implementations of post effects, then
  GPU ports judged bit-identical against them; a Python converter as the reference for the C++ one.
- **Some surfaces are generated on VU1.** Water in our game is tessellated by a VU1 program from control
  points: no mesh exists anywhere in memory. Check the VU program before assuming a mesh decoder will do.

## Working with AI agents (this project is built with Claude Code)

- **Start sessions from a short status file**, and move session logs out of it into `docs/`. A 150 KB status
  file made every session slow and expensive.
- **Give long jobs time limits and watch them**: a build sat half an hour at a hidden "press any key" prompt.
- **Local models are good at narrow ports with a strict judge** (exact byte comparison, small functions, staged
  tasks); not at open-ended discovery.
- **Write down the why**, with addresses and evidence. The next session (or person) needs the reason, not just
  the result.
- **Find where code comes from before keying anything on a captured image.** PoP's VU "images" were really 10
  ELF programs plus stale tails from earlier uploads; keying compiled code on the whole image missed every new
  combination (the 19 Oct death scene at 40 fps). Keying on the source program covers them all, and also
  removes the need to record or ship images.
- **A model's "already happened" values are not state.** Verifiers that compare bookkeeping (ready cycles in
  the past, flag slots) byte for byte reject code that is equivalent but keeps less bookkeeping; normalize
  what cannot influence the future (a ready cycle at or before now) and keep comparing everything else.
- **A correct change can make generated code slower through layout alone.** Adding EFU support to the VU1
  scheduler (19 Oct) gave identical results and no hand-overs, yet every save lost 8-15 fps: the new nodes moved
  where the program was cut into parts, and hot loops now crossed parts on every iteration (each crossing saves
  and reloads ~140 values). Check performance after every generator change, not only exactness, and profile
  before guessing (the profile pointed at one program's parts within minutes).
- **Stopping a PC job mid-boot leaves the game running** (19 Oct): ending the scheduled task kills only the
  wrapper, the `ps2EntryRunner.exe` it started stays and locks the exe, so the next build fails with LNK1104.
  `tools/overnight-pc.command --stop` now stops the game too; a play session left open has the same effect.
- **The PC needs an active display for game boots** (19 Oct): with the TV off (or in standby) the GPU has no
  active display (`nvidia-smi --query-gpu=display_active` = Disabled), the GS thread's OpenGL `Present` stalls
  and every boot runs at ~4 fps (game thread waiting in sceGsSyncPath). Keeping Windows awake
  (SetThreadExecutionState) does not help. A whole evening's skip measurements were invalid because of it.
  Check `display_active` before measuring frame rates (or use an HDMI dummy plug).
- **A hook that skips a game function skips all of its side effects** (19 Oct): `CJMeshElement::Display` and
  `CJMeshElementRef::Display` also switch the VU1 driver (MPG upload) before skinned draws; skipping them for a
  measurement left the wrong program loaded and every save fell to ~7 fps. Decompile before skipping.

## start-job.ps1 arguments over plain SSH (19 Oct 2026)

`ssh pc "... start-job.ps1 -Script x.ps1 -Arguments '-World'"` goes through cmd.exe, which keeps the single
quotes: the wrapper then runs `& x.ps1 '-World'`, a quoted string that PowerShell binds to the first
positional string parameter (oct19.ps1's `$Pattern`), so the script's default mode ran, twice. Start jobs
from PowerShell (`powershell -EncodedCommand`, as tools/overnight-pc.command does). Scripts whose default
mode boots the game check for an active display first, so a mistake like this cannot reach a boot.

## Night of 19-20 Oct 2026
- **Measure the per-frame hand-over before trusting an in-process design.** The native world first re-sent
  every batch's geometry each frame (8.2 MB): reading and uploading it took 7 of the worker's 8.3 ms, and the
  hook waits for the worker on the VU thread. Keeping meshes on the GPU by key (PNF7) cut it to 79 KB and
  ~3 ms. Readback from device-local memory without HOST_CACHED cost 33 ms on its own.
- **Our VU disassembler was wrong for the flag instructions** (fixed in `tools/vudis.py`): FCAND/FCOR/FCEQ
  always write vi1 (it printed a garbage register from the immediate bits), FMAND/FMOR/FMEQ take two
  registers (`FMAND vit, vis` = MAC & vis; it printed an immediate). A port written from the old listing
  would have been wrong; check a disassembler against the interpreter's decoder before giving its output to a
  model.
- **Hand-port one stage before starting a model on a program** (as for the C ports): the known-good P6 setup
  stage found the judge's issues (rounding mode, `-ffp-contract=off`, pending Q/MAC at stage borders) before
  any model time was spent. Stage borders must be where only harmless writes (vf/vi/ACC) are in flight;
  the replayer reports what is pending at each entry and exit.

## 22 Oct 2026
- **A quick-save during a freeze is kept only as `quick.state`.** The kept copy (`play_<date>_<time>.state`) is
  written by the game's next frame, which never comes when the game has hung, and the next session's first
  quick-save overwrites `quick.state`. Crash 1 of 20 Oct lost its save this way (`docs/saves_1021.md`). Until the
  copy is made by the runtime right after saving: after a crash, copy `quick.state` before playing again.
- **Copy many large files over SSH one at a time:** one `scp` with a wildcard for 3.7 GB
  died on a timeout halfway with nothing to resume from; a per-file loop that skips complete files
  (`game/analysis/play1021/fetch.sh`) finished on the second try. PowerShell's `-Filter` has no `[01]` classes:
  use `Where Name -match`.
- **The same VIF layout can mean different things per pass.** A static packet's V4-8 at VU 0x83 is a colour in
  pass 0 and a normal in pass 1 (the lit driver); the converter read the sword's normals as colours (alpha 1/128,
  alpha-tested away). Decide what an upload is from the pass/driver that consumes it, and audit with a census over
  all saves (`tools/staticcensus.py`) rather than one scene.
- **Skip tests must keep the hardware state the skipped call would have left.** `POP_NATIVE_SKIP` kept any chain
  whose skip would miss a VU1 driver switch, so "skinned" looked as if it did not hold the sword; replaying the
  switches (`POP_NATIVE_SKIP_REPLAY=1`) made the test clean.
- **A dedupe key without the pass hides a bug until a decoder improves.** Instanced pass 0 and pass 2 shared one
  key: harmless while pass 0 never decoded, a lost second layer once it did. Give every key every field that makes
  two draws different.

- **Don't take the render target from GS state at an arbitrary point in the stream** (23 Oct, load flash). The GS registers
  at our marker were whatever the last draw left; on the first frame after a load the game had sent no clear yet, so
  FRAME still named the displayed buffer. A cheap invariant caught it: a double-buffered game never draws into the
  buffer on screen. Getting there took 6 playtests: a GS trace of the frames in question (runtime patch 40, started from
  an override at the moment of interest) settled what dumps of our own picture could not.
- **Put seconds in log names** when two boots can start within a minute (one GS-only log was overwritten).
- **Check the judge before the thing judged (23 Oct, cloud layer).** A "native renders the clouds wrong" lead was the
  offline viewer drawing a newer frame format opaque-first while the game window drew in game order. Every input the
  draw had (texture, UV matrix, vertex alpha, GS state) matched the game's DMA chain; the order of the draws did not. When
  a difference only shows offline, diff the offline path's settings against the in-game path first.
- **When a local model fails the same few cases the same way for many rounds, patch its attempt by hand and judge
  it before giving it more rounds** (24 Oct, P6 water port). Four stages stuck at "expected 0, got small integer
  bits" for ~60 rounds over two nights were one rule missing from the spec (the VU reads denormals as zero; these
  lanes held colour bytes). Retries cannot fix a spec gap; one judged patch found it in minutes. Also: read the
  reference implementation's float handling (here `ps2_vu1_impl.inl`'s `vnorm`) before writing a spec for it.
- **"pc unchanged after a call" is not always a stub return (24 Oct).** Generated code can come back to the
  scheduler with the same pc without unwinding (a Ghidra-split loop whose pieces call each other). A runtime rule
  "pc unchanged = return to $ra" must be limited to functions known to be host code (replaced after
  registration), or it jumps into garbage (patches 41/42).
- **A stage that fails for many rounds on one value: check the judge's borders first (25 Oct).** Two P6 pieces
  failed 26-28 rounds over two nights because a CLIP was in flight at their borders and the case snapshot took the
  clip flags before it landed: no correct port could pass, and one "pass" was the model clearing the flags to match.
  Re-judged after the fix, the model's round-1/2 attempts were right. Snapshot every pipelined value the same way
  (as landed) and let a tool pick borders where nothing unsafe is in flight (`tools/vu1port/cutscan.py`), and be
  suspicious of a pass that writes constants to state just before returning.
- **Check that a scripted input actually ran, not just that it was passed (25 Oct).** `boot.ps1` cleared any
  `POP_INPUT` given through `-Env`, so every "R1 held" fight boot ran without blocking (the Prince died in a
  300 s crash-1 check). The override logs `[pop:input] <button> at <t> s` for each press: grep for it in every job
  that relies on scripted input.
- **A few screenshots cannot show a short-lived effect is absent; the user's eyes with clock times can (25 Oct).**
  For N1 I called "no tint" from two frames per boot (one of them the load dialog) and later from single frames that
  fell after the slow ended. The user noted the times the tint showed, and the job log's run start times mapped them
  to runs (GS-only: yes; native: no). Log each run's start time, and for a visual check take many frames or ask
  the user to note times. Also: when native and GS differ, look for game code that edits data in memory (palettes,
  textures) that native reads from the disc instead.

## 26 Oct 2026
- **Prove a profiler hot spot by removing it before acting on it** (from gow-native-pc, `docs/reference.md`).
  Their thread-suspension sampler showed ~40 % of VU1 time in a dispatch loop; removing the loop saved ~2 %, since
  such samplers pile samples around calls and returns. Applies to `pop_profiler` too.
- **A clean session says little about a rare crash; compare it with the crash's rate.** Crash 2 was seen once in
  ~90 min, so a 40-minute session without it is likely even if nothing changed (and nothing was built against it).
  For a crash seen several times (crash 1: ~16 and ~23 min of fights), a long clean session counts, but only if it
  had the same activity (time spent fighting, from the log's low-fps stretches).
- **A judge on sampled cases passes code that is wrong on the rare inputs.** 14 local-model VU1 ports passed 600
  sampled cases each, yet wrote the largest float as the integer 0x7f7fffff (or read memory at that address): no
  sampled case overflowed. Whole-run checks and every case of the failing runs (`vu1replay --records`) found them;
  a static check for the known bad form now runs before the judge. Also: when a primer describes semantics
  ("product first, then the add"), check it against the reference code; the model copies the primer faithfully.
- **Before hunting for code that never ran, ask the binary who calls it.** P0, P2, P3, P10 (VU1 inventory) were
  settled offline in about an hour, with no captures: references to each MPG packet, the program-selection table, and the
  prototype's labels (its VU driver labels come in the same order as the retail programs). Two were VU0 code, one
  unreachable, one dead library data. `docs/native_renderer.md` "VU1 program inventory".
- **A save's own frame is not a GS reference for anything the native renderer drew.** Saves made with native on
  hold the injected native picture, so `score_saves` "against the game" compares native with its own older output for
  those draws: a change towards the GS can score as "further from the game" (vertex alpha, 27 Oct). Judge native-drawn
  changes against a GS-only frame (all native switches off), and read the microcode before inferring what the GS gets.
- **An offline score is a change detector, not a distance to the goal.** The nightly render gate (28 Oct) scores
  `pop_state_frame` frames, which draw every list natively (late lists, sprites, particles) and no HUD, against GS-only
  saves: fights score 8-13 with nothing broken. Read the side-by-side before treating a high score as a bug, and
  only a rise above the save's own baseline as a regression.
- **Byte matching: read the compiler flags off the retail code before blaming the model** (28 Oct, DARE). Every
  global in DARE is reached as `lui at` / `lw (at)` and never via `gp`, which only `-sdatathreshold 0` reproduces;
  without the flag no function with a global could match, whatever Qwen wrote. Count such patterns across all target
  functions (gp uses, float constants, tail calls) when a whole class of attempts misses.

## 29 Oct 2026
- **A hybrid frame must keep the game's draw order for every order-dependent draw.** The native picture goes in
  where the frame's 3D starts, so every GS draw lands on top of every native one. Opaque Z-writing draws do not care,
  but a blended or Z-less draw drawn natively before a GS draw the game made earlier ends up under it (N13/N14/N16:
  GS walls over native corner stones). Such draws stay native only while nothing earlier went to the GS.
- **Float depth breaks the GS's ties.** Decals sit on their walls with no depth write and pass GEQUAL on the GS's
  integer Z; our float Z, which differs from VU1's by tens of units, made them fail on some frames (N16 moss). A small
  bias toward the camera for draws without depth write (32 GS units) restores the GS's behaviour.
- **Bisect with switches and the user's eye before reading code.** Four runs (world only, + shadows, + translucents)
  found the N13 cause after two wrong theories from code reading (lightmap, shadow), each of which checked out as
  exact. An offline save does not reproduce what only the live hybrid does (which draws go to the GS).
- **Check that a debug switch took effect before trusting a "no change".** `POP_NATIVE_SKIP` is ignored with the
  native world on, so a whole run tested nothing; the log's counters showed it.
- **Ask for an annotated screenshot early.** A red circle on a capture frame located the moss flicker at once, where
  a guess (the Prince's shadow) cost a run. Send the frame to the PC desktop as well (`scp` to `Desktop/`).
- **A port judged on traced runs can lean on registers the traces always carried in.** Qwen's P6 stage `a_g1_row`
  dropped `LQ vf4, 4(vi0)`; it passed 600 whole runs because the mask pass (which loads vf4) always ran just before
  with the same matrix. Offline, with only the colour pass, the water landed in a screen corner. Before wiring a port
  in, run it against the interpreter on the new caller's inputs (same VU memory and registers, compare the data
  memory after each run), not only on the traces' run order.
- PowerShell unrolls a one-element array returned from an `if` expression: `$r = if (c) { ,@('a','b') }` iterates the
  strings, so `$r[0]` is 'a'[0]. Build the list first and narrow it with `$runs = @(,$runs[0])`; echo the run names before
  booting (29 Oct: a job booted without its save). Stop a job's game with `taskkill /f /pid` for each process and check
  `tasklist` before the next boot: `Stop-ScheduledTask` leaves the game (and a looping script's next run) going.
