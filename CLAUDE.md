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
"Moved from CLAUDE.md on 30 Oct 2026 (late night, 0.13 closed)").

- **Where we are: 0.13 closed (30 Oct night, user in-motion check passed).** Native renderer on by default in `play.ps1`
  (static world, lit statics, characters, translucents, shadows, P5/P6/P9, and since 30 Oct night the late lists
  +0x88/+0x98/+0xA8, halos, Pinch; `-NoNativeLate` sends the late part back to the GS). The native frame is "pure"
  (no GS draw yet, so native draws keep the game's order) up to the 2D pass in nearly every frame: sun 162621, fountain
  202926, sand 115853 all end only at the HUD (list +0xB8). User check (sun halo + torches, fountain column glow and
  saving, dagger glow, rewind): all good. Last night's work: `docs/native_renderer.md` "Lists after the shadows" (30 Oct
  night), the moon halo (offline-only artifact, closed) in "Native halos", dagger glow in `docs/renderer_audit.md` item 9.
  Docs: `docs/native_renderer.md` (layers, VU1 inventory), `docs/native_bugs.md` (open bugs, parked list), `docs/renderer_audit.md`.
- **0.13 tail: native post chain live, built, not booted (30 Oct late).** Live, glow / SmartBlur / MotionSmooth /
  FixBlend / ZoomSmooth were still the GS's over the injected picture (only Pinch was native: 478/488 in eve1030k).
  `POP_NATIVE_POST=1` (with `-NativeLate`) sends them to the world frame's post chain while pure and skips the GS
  originals; GPU in `renderOffscreen` = CPU exactly on 4 saves (`POP_VIEW_GPU_POST=1`). Next: a boot (TV) on the glow
  saves, then the user's look. `docs/native_bugs.md` "Native Pinch" (eve1030k), new bugs N18 / N19 there.
- **0.1:** crash 1 closed 30 Oct (fix 3b80b66; ~20 min fight session clean, `docs/saves_1021.md`). Crash 2 only watched (full dump on a freeze; no speculative emulated-sound fixes).
- **Rules in force:** hybrid-frame bugs park until 0.2, bugs in native code are fixed now; all performance work waits
  until fully native (user). The user's in-motion check is the reference for short effects; our GS emulation is not
  ground truth. Render-gate reference saves must be GS-only.
- **Next: 0.14 (native HUD, menus, loading screens, movie display; full-screen fades too). Plan: `docs/native_2d.md`
  (30 Oct late, approved: engine-level hooks per 2D producer, no GS packets; survey done, 4 new hooks + 2 existing paths; next: step 1, the offline 2D layer).** Known facts to build on:
  - **+0xB8 is the 2D pass:** PS2_After2D (0x4B4A00) -> FUN_004BBFC0 sets draw manager +0x28 = 1 and renders list +0xB8,
    after the after-effects (glow, SmartBlur). The native renderer has **no layer after its post effects** (its picture
    is injected at the 3D marker, the GS draws +0xB8 on top): that layer is the groundwork (`docs/native_bugs.md`
    "Flashes survey").
  - **HUD text and bars = FUN_00469C00 (list kind 0x18), the Jade STR text renderer:** per font page FUN_004B2000 sets
    the state, then CNT + DIRECT + GIF tag (PRIM 0x5E sprite, textured, blended; ST RGBAQ XYZ2 ST XYZ2), screen
    coordinates, Z 0; letters by FUN_0046A980 / FUN_0046AA80 (the hook points, not the packets; `docs/native_bugs.md`
    "Light shafts"). `pop_state_frame` does not replay +0xB8 yet.
  - **Fades = kind 11 entries in +0xB8** (FUN_004B4580 rectangles, table 0x6DF6E0): one blended screen quad each.
  - Menus, loading screens (drawn once and left on screen: `docs/widescreen.md`), movie picture: not surveyed;
    `docs/native_renderer.md` ~79 and ~623 have only one-liners. Survey other PS2 recomp projects first (`docs/ps2_projects.md`).
  - Test saves: any save with the HUD (185602 shows life bar, sand tanks, tutorial text); fades 191358 (white-out),
    192114 (half black). `POP_NATIVE_WORLD_WHY=1` prints `purity ended: list18 +0xb8` today, which should move on.
- **Side tracks:** DARE decomp for 0.3/2.0 (`docs/decomp_dare.md`, 79/669 matched, Qwen queue via the "Qwen DARE
  audio decomp" shortcut). Small open items (Esc not quitting, `POP_NATIVE_SKIP` ignored, `pop_world_test` exit 138
  on the Mac): `docs/native_bugs.md` "Side jobs".

## Release roadmap (user; full text: `docs/roadmap.md`)

```
0.1   Playable start to finish, emulated graphics + sound (crash 1 fixed; crash 2 watched)
0.11  Native static world
0.12  Native characters, translucents, shadows + particles
0.13  Native effects: water (P6), sprites (P9), halos, light shafts, flashes (closed 30 Oct, user check)
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
