# Project brief: Prince of Persia: The Sands of Time, PS2 recompilation and PC modernisation

Started 11 Oct 2026. Trimmed 30 Oct 2026 to save context: this file is read on every turn, so it holds rules,
the current step and pointers. Full working reference (switches, tools, addresses, layout): `docs/reference.md`
"Working reference"; old status blocks and session logs: `docs/history.md`. (estimate) = judgement call,
(unverified) = single forum post or similar: re-check before relying on either.

## Goal and approach

Personal-use, better PC version of PoP: Sands of Time (Ubisoft Montreal, 2003) that keeps the PS2 look (haze,
post-processing). Static recompilation of the PS2 version (PS2Recomp) with hardware replaced **at the engine
level**, not emulated: native Vulkan renderer drawing the Jade draw manager's calls (not GS emulation), native
mixer driven by DARE/UbiSound calls (not IOP/SPU2), native decoder for the PS2 movie files behind the game's own
player (not IPU). Migrate part by part; emulated parts stay until replaced and serve as the reference.

Decided 22 Oct: keep our own native renderer; no switch to the PC port's renderer, OpenJade or rePOP as a base
(reasons: `docs/history.md`). pop-bf-lab and OpenJade's source (if released) are format references only; rePOP
for Xbox addresses and Hor+ hooks.

## Ground rules

- Personal use only; the game and its source belong to Ubisoft. Work from the owned US disc (SLUS-20743) and GOG copy.
- Never commit or redistribute the ISO, ELF, game data, texture packs, prototypes or recompiled output. All game
  material stays under `game/` (git-ignored). Only our own tooling and glue code may be shared.
- The PS2 disc is the base: the recomp must run from it alone. Assets from other owned versions (Xbox, PC) are
  optional extras (`docs/reference.md` "Cross-version assets").
- Provenance: our code comes only from the PoP binary, our own analysis, public hardware documents and open-source
  projects under compatible licences. Never use leaked Sony SDK source/headers/samples (e.g. ezmpeg) or leaked Jade
  source (incl. raydev.wiki's derived format list), and never write such code from memory (precedent:
  OpenRAC's `docs/MOVIE.md`). Unreleased prototypes: local research only.
- Status reports: say plainly when something does not work.
- Simplest change that works (user, 25 Oct); simplify before reporting. Propose a workflow change only if the same
  complexity has come up before.
- **User rules (19 Oct):** the PC's TV is off unless the user plays: ask before any job that boots the game
  (`nvidia-smi --query-gpu=display_active` must be Enabled). Survey other projects before fixing a narrowed-down
  problem (`docs/ps2_projects.md`); general learnings to `docs/lessons.md`; one dated job script per night; optimise
  biggest win first. Shut the PC down after overnight work. The user gathers saves and does milestone checks; I work
  offline.
- New native code stays portable: Vulkan within MoltenVK's base path, SDL-class libs, no Windows-only APIs, x86
  fast paths only with a portable fallback, AOT SPIR-V (`docs/reference.md` "Portability principle").

## Current status (30 Oct 2026; start here)

Keep this short: finished work goes to the topic docs, replaced bullets to `docs/history.md` (last moved:
"Moved from CLAUDE.md on 30 Oct 2026").

- **Where we are:** native renderer at roadmap 0.12, on by default in `play.ps1` (static world, lit statics,
  characters, translucents, shadows, P9 sprites); late lists (+0x88/+0xA8) stay on the GS unless `-NativeLate`.
  Working on **0.13** (details: `docs/native_bugs.md` "Native Pinch" and "Flashes survey"): P5, P6 on by default
  (user-checked); halos, Pinch (= the rewind's warp) and the rewind haze (= the unmasked glow, fixed by GS truncation
  in ColorDiffusion) done offline, not yet on the PC; light shafts need no new path
  (glow-mask meshes + ColorDiffusion, native with `-NativeLate`); full-screen fades and the HUD (list +0xB8, the 2D pass)
  moved to 0.14 (user 30 Oct). MotionSmooth + FixBlend + ZoomSmooth 0 (= the power of haste) native offline (30 Oct, `docs/native_bugs.md`; Spiral,
  ZoomSmooth 1, Vision unused in retail). Left: the moon halo, the frame-enders that keep halos / Pinch on the GS.
  Docs: `docs/native_renderer.md` (layers, VU1 inventory), `docs/native_bugs.md` (open bugs, 0.13 candidates,
  parked list), `docs/renderer_audit.md`.
- **0.1:** crash 1 closed 30 Oct (fix 3b80b66; ~20 min fight session clean, `docs/saves_1021.md`). Crash 2 only watched (full dump on a freeze; no speculative emulated-sound fixes).
- **Rules in force:** hybrid-frame bugs park until 0.2, bugs in native code are fixed now; all performance work waits
  until fully native (user). The user's in-motion check is the reference for short effects; our GS emulation is not
  ground truth. Render-gate reference saves must be GS-only.
- **PC session 30 Oct evening (done):** bundle + `-Fast` build; patches 43/44 boot check fine (640x448 shots, P5: no
  interpreter hand-over); GS-only fountain save -> gate line `202926 vu1 6.67`; traces of 185807 and 145139 plus the 27 Oct
  census traces copied to `game/jobs/gstrace1030/`, `game/jobs/gscensus/`. TV off: boots still 60 fps. **Found:** live
  world mode never installed the halo / Pinch hooks (only the capture modes did); fixed 5804fc9. Live now: sun 162621 halos
  1388 calls, native 0 (frame impure from `ref +0x2c`); sand 115853 pinch 3, native 0 (impure from `mesh +0x98`); fountain
  ends at `gfx +0x88`. Later that evening: fountain's `gfx +0x88` fixed (599c632: recoloured textures count as ready;
  2c35f94: `textureKey` took free TEX_List entries as texture 0, so the save-point column drew blocky; user: column looks
  good now). Frame-enders left: `mesh +0x98` (glow-mask meshes writing Z: fountain, sand) and the sun's `ref +0x2c`
  (not lightmap / texture / decode: cause unknown). Jobs `game/jobs/eve1030*.ps1`.
- **ZoomSmooth 0 (haste) judged 30 Oct:** GS-only saves `game/analysis/saves1030/` (174127/131/137 on, 174148 off): native
  blur matches the GS (vu1 --noglow 6.16 / 7.45 vs 6.09 off; the HUD is most of it); 174131 in the render gate.
- **Next Mac session (offline, no PC needed), in order** (30 Oct list done: `docs/native_bugs.md` "Rewind look
  resolved", `docs/renderer_audit.md` item 9; offline saves lack shadows when the save's lists were already emptied):
  1. Dagger glow: colour fixed 30 Oct (GS saturates Ct x Cv at 0xFF, `mesh.frag`); still brighter than the GS at its
     centre. Waits on PC step 4's traces: the 115837 census trace (draw's RGBAQ / FOG / ALPHA; the alpha is
     animated and matches: `docs/renderer_audit.md` item 9 "Dagger glow").
  2. Rewind ramp (185807) closed 30 Oct: the GS frame's floor shadows, which the save can't replay
     (`docs/native_bugs.md` "Rewind look resolved"). Nothing left offline for the rewind.
  Waiting on the PC / user: moon halo (PC step 4), ZoomSmooth 0 score (a GS-only save while haste runs).
- **Side tracks:** DARE decomp for 0.3/2.0 (`docs/decomp_dare.md`, 79/669 matched, Qwen queue via the "Qwen DARE
  audio decomp" shortcut). Small open items (Esc not quitting, `POP_NATIVE_SKIP` ignored, `pop_world_test` exit 138
  on the Mac): `docs/native_bugs.md` "Side jobs".

## Release roadmap (user; full text: `docs/roadmap.md`)

```
0.1   Playable start to finish, emulated graphics + sound (crash 1 fixed; crash 2 watched)
0.11  Native static world
0.12  Native characters, translucents, shadows + particles
0.13  Native effects: water (P6), sprites (P9), halos, light shafts, flashes
0.14  Native HUD, menus, loading screens, movie display
0.2   Fully native graphics: GS off by default, judged frame by frame against the emulated GS
0.3   Native sound (DARE/UbiSound, no IOP/SPU2) + native PS2 movie decoder (no IPU)
0.4   Fully native: memory card, pad, faithful DualShock 2 rumble
1.0   Clean-up, GS reference toggle; publish own code only; 30 fps like the original
1.1   Widescreen Hor+ | 1.2 uncapped fps | 1.3 higher res + crisp HUD | 1.4 PC/Xbox assets
1.5   Linux / macOS | 1.6 ray tracing toggles | 1.7 DualSense | -- PAL/JP discs | 2.0 (potential) byte-matching decomp
```

## Working reference (short; full version with every switch and tool: `docs/reference.md` "Working reference")

- **Where to look first:** grep `docs/` before re-deriving a fact. Topic docs: `roadmap`, `native_renderer`,
  `native_bugs`, `postfx`, `movies`, `llm_jobs`, `vu1_scheduling`, `upstream_ps2recomp`, `decomp_dare`, `lessons`.
- **PC access:** `ssh 10.0.0.202` (ping blocked; wake-on-LAN), repo `C:\dev\PoP_Decomp`. The PC can't pull from the
  Mac: git bundle to `game\jobs\pop.bundle`, then `git pull --ff-only`. PowerShell over SSH via `-EncodedCommand`;
  complex env strings in a wrapper under `game\jobs\`. Windowed jobs need the desktop session:
  `tools\windows\start-job.ps1 -Script <ps1>` (log `game\jobs\job.log`), watched with `tools/windows/watch-job.sh`
  under Monitor. git on the PC has no identity (`git -c user.name=… -c user.email=…`). Before a boot: user's game
  closed (`taskkill /f /pid`, check `tasklist`).
- **Builds:** PC `tools\windows\rebuild.ps1 -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc`
  with `-Fast` (override/runtime-only) or `-SkipGhidra` (recompiler/runtime headers, 30-50 min). Runtime patches:
  `tools/patches/pop-sotc/` (up to 44, all applied on the PC). Mac builds the `pop` branch only
  (`tools/rebuild.sh`); native tools via CMake in `tools/native` -> `game/native-build`.
- **Test loop:** edit -> bundle -> `game\jobs\stnative.ps1` (45 s save boots, R1 held). User bug checks:
  `play.ps1 -Mode hybrid|gs|native -Save <name>`. Offline: `pop_state_frame`, `tools/native/score_saves.sh`.
  Saves: PC `game\ps2\disc\states\`, Mac copies under `game/analysis/saves*/`, GS-only refs `gsref1027/`.
- **Override rules learned the hard way:** never rely on code after calling a generated function's original (the
  scheduler can unwind mid-call; compare `ctx->pc` with the entry `$ra`); call generated functions by address
  (`callOriginal`); self tail calls are internal jumps (a hook sees only the first); PS2Recomp substitutes its own
  C++ for functions ps2_analyzer names (wrong for libvu0, `rand`, `sceSdRemote*`): check any new stand-in.
- **Practical:** judge sound on the TV or a WAV, never over Moonlight. macOS has no `timeout` (use
  `perl -e 'alarm N; exec @ARGV'`). PC Python defaults to cp1252: open text with `encoding='utf-8'`. Save-state
  boots have no sound; runtime screenshots sit in a corner of a 9600x5400 image (`sips -Z 1600` before looking).
