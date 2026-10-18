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
