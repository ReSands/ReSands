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

## Current status (1 Nov 2026; start here)

Keep this short: finished work goes to the topic docs, replaced bullets to `docs/history.md` (last moved:
"Moved from CLAUDE.md on 1 Nov 2026 (0.14 closed)").

- **Where we are (1 Nov): 0.14 closed (user TV check passed, 1 Nov); native 2D layer on by default in `play.ps1` (`-NoNative2D`).**
  Native renderer on by default in `play.ps1` (world, lit statics, characters, translucents, shadows, P5/P6/P9, late
  lists, halos, Pinch, post chain; `-NoNative*` switches send parts back to the GS). The frame is "pure" (no GS draw yet)
  up to the 2D pass in ~2399/2400 frames (192645, 162621 boots 1 Nov): it ends at list +0xB8 (`list18`, `idx`), which is
  0.14's job. Closed 31 Oct-1 Nov: N18, N19 (skinned instanced refs now posed natively, afa2244), N20, N21, N22 (entries
  in `docs/native_bugs.md`). Parked: N23 / N26 (halo fps), N27 (needs a real-PS2 reference). Render gate (13 GS-only
  saves, `game/analysis/gsref1027/states/`) unchanged by the N19 fix (pixel-identical). Docs: `docs/native_renderer.md`,
  `docs/native_bugs.md`, `docs/renderer_audit.md`.
- **0.1:** crash 1 closed 30 Oct (fix 3b80b66; ~20 min fight session clean, `docs/saves_1021.md`). Crash 2 only watched (full dump on a freeze; no speculative emulated-sound fixes).
- **Rules in force:** hybrid-frame bugs park until 0.2, bugs in native code are fixed now; all performance work waits
  until fully native (user). The user's in-motion check is the reference for short effects; our GS emulation is not
  ground truth. Render-gate reference saves must be GS-only.
- **Next: 0.2** (GS off by default; plan + progress `docs/gs_off.md`). Step 1 (GS census, `POP_NATIVE_GS_CENSUS`) 1 Nov:
  pure game frames draw only alpha on the GS; title menu and intro movie draw no colour either. Fallbacks fixed and
  user-checked 1 Nov: blended Pinch native (6054f84), all-clipped water skipped natively (4e7a046); left: rare single
  calls (~1 per session, judge in step 4). N29 (rewind's white flash) is the game's own. Step 2 (native presenter, `POP_NATIVE_PRESENT=1`, patch 45) closed 1 Nov. Step 3 (GS off, `POP_GS_OFF=1` / `play.ps1 -GsOff`, patch 46) closed 1 Nov. **Step 4 (native vs GS): first run 1 Nov (fa8f097: scenery, menus, movies match; moon blob gone; fights not comparable across two boots). Step 4b first run 1 Nov (8cf4778, `docs/gs_off.md` "Step 4b, first run"): effects/scenery match on the same frame; fights not same-frame offline (character poses ahead of the saved picture). Step 4c (offline frame = the save's last chain; plan + results `docs/gs_off.md` "Step 4c plan"): points 1-5 done 1 Nov (poses, reference = the buffer the chain drew via `popstate --drawn`, shadow lists, halos, text all from the chain offline; switch `POP_STATE_CHAIN=0`; gate pixel-identical with it off, re-based with it on). Next, in order (user 1 Nov): (1) /ponytail-review of points 2-5 (commits 8601a4f..fdc7338; live file `pop_native_live.cpp`: `poseSkinned` packet argument, null hooks `g_chainBones` / `g_haloFilter` / `g_chainTextSlot`, `letterRecord` takes ST floats; not yet built on the PC); (2) point 6 (glow over-brightening offline, +0x88 mist); point 7 (hardware bundle) waits until the user has the PS2 in hand (loading route, RAM fit, readback vs capture decide the format). Then step 5 (parked bugs with the GS off), then flip the default.** Parked (`docs/native_bugs.md`):
  N28 (Retry fade pop-in), N30 (slowed enemy white vs sandy: real-PS2 decides), progressive scan from the title menu crashes (check GS-only first).
- **Side tracks:** DARE decomp for 0.3/2.0 (`docs/decomp_dare.md`, 124/669 matched; 1 Nov: run 5 analysed, header + aligned diff + runner fixes, short test run (same passes, better near misses, faster); run 6 prepared: compile-fix calls + second first draft (team-solve ideas); Qwen queue via the "Qwen DARE
  audio decomp" shortcut). Small open items (Mac runtime build: stale precompiled header after a compiler update, delete `game/recomp/runtime-src/build-mac` PCH; Esc not quitting, `POP_NATIVE_SKIP` ignored, `pop_world_test` exit 138
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
  `tools/patches/pop-sotc/` (up to 46, all applied on the PC; after `git am` also check out its HEAD in `game\recomp-sotc\runtime-src`, the build reads that clone). Mac builds the `pop` branch only
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
