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
- New native code stays portable: Vulkan within MoltenVK's base path, SDL-class libs (SDL3 for the pad), no Windows-only APIs, x86
  fast paths only with a portable fallback, AOT SPIR-V (`docs/reference.md` "Portability principle").

## Current status (1 Nov 2026; start here)

Keep this short: finished work goes to the topic docs, replaced bullets to `docs/history.md` (last moved:
"Moved from CLAUDE.md on 1 Nov 2026 (evening tidy)").

- **Where we are (1 Nov evening):** 0.14 closed (1 Nov). Native renderer + native 2D on by default in `play.ps1`
  (`-NoNative*` switches send parts back to the GS). 0.2 (GS off by default; plan + progress `docs/gs_off.md`):
  steps 1-4c done (census, native presenter patch 45, GS off `POP_GS_OFF=1` / `-GsOff` patch 46, native vs GS on the
  same offline frame via `POP_STATE_CHAIN`; scenery, effects, menus, movies match). PC play build has all of it.
  Closed 1 Nov: progressive-scan crash (patch 48), camera X in fixed views (21ab2c7). Render gate (13 GS-only saves,
  `game/analysis/gsref1027/states/`) pixel-identical. Docs: `docs/native_renderer.md`, `docs/native_bugs.md`,
  `docs/native_2d.md`, `docs/renderer_audit.md`.
- **Next (user, 1 Nov): set up the real PS2 and the pipeline to test on it** (`docs/gs_off.md` step 5 /
  step 4c point 7). Step 5 and the `-GsOff` default flip wait for it; its hardware questions (N27 halo flicker,
  N28 Retry fade, N30 enemy white vs sandy, N15/N17, single GS calls, parked hybrid bugs) are listed there.
  Meanwhile: 0.3 native sound (below).
- **6 Nov: 0.3 native sound; plan + progress `docs/native_sound.md` step 3. (a) recompiled IOP modules (patch 51),
  (b) IOP clock = PC clock (52), (c) native libsd + mixer (53, aligned to PCSX2 in 54) done offline; checked
  against PCSX2's SPU2 core as the reference (`tools/iop/pcsx2_ref`; our emulated SPU2 is not a reference); next:
  the PC build + in-game listen (TV on).** Decided: 0.3 does not
  wait on the DARE byte-match (DARE's EE + IOP driver code run recompiled; libsd -> native mixer, IOP plumbing native, route A in step 3);
  no outside decomp helps. DARE decomp paused at 248/669 (run 13); its desktop shortcut stays for nights the user
  wants it.
- **5 Nov:** 0.4 closed with FFmpeg (decoder native since 16 Oct; `docs/roadmap.md` 0.4). Working on 0.5's
  independent parts while waiting for the PS2 and DARE. **Memory card done (evening, patch 49, user-checked in game):**
  PCSX2 `.ps2` image at `%USERPROFILE%\Saved Games\PoP Recomp\Mcd001.ps2`, old `mc0/` saves imported; open bits
  in `docs/roadmap.md` 0.5 "Progress". **SDL3 pad + rumble: patch 50 built on the PC (night), waiting for the user's in-game check**
  (`docs/roadmap.md` 0.5 "Pad + rumble progress").
- **4 Nov:** rope bridges fixed at any frame rate (fixed 1/30 s steps + extrapolation; user-checked), ropes look
  the same at 30/60, `POP_60FPS=0` now really forces 30 fps (`docs/native_bugs.md` "Bridges"; 1.4 re-check list in roadmap).
- **0.1:** crash 1 closed 30 Oct (3b80b66). Crash 2 only watched (full dump on a freeze; no speculative emulated-sound fixes).
- **Rules in force:** hybrid-frame bugs park until 0.2, bugs in native code are fixed now; all performance work waits
  until fully native (user). The user's in-motion check is the reference for short effects; our GS emulation is not
  ground truth. Render-gate reference saves must be GS-only.
- **Side tracks:** DARE decomp for 2.0 (`docs/decomp_dare.md`, 248/669 after run 13, 6 Nov; night job = Qwen +
  permuter lanes from the desktop shortcut, only when the user starts it). Small open items (`POP_NATIVE_SKIP` ignored, `pop_world_test`
  exit 138 on the Mac): `docs/native_bugs.md` "Side jobs".

## Release roadmap (user; renumbered 5 Nov; full text: `docs/roadmap.md`)

```
0.1   Playable start to finish, emulated graphics + sound (crash 1 fixed; crash 2 watched)
0.11  Native static world
0.12  Native characters, translucents, shadows + particles
0.13  Native effects: water (P6), sprites (P9), halos, light shafts, flashes (closed 30 Oct, user check)
0.14  Native HUD, menus, loading screens, movie display
0.2   Fully native graphics: GS off by default, judged frame by frame against the real PS2
0.3   Native sound (DARE/UbiSound, no IOP/SPU2) | 0.4 native PS2 movie decoder (no IPU; closed 5 Nov, FFmpeg)
0.5   Fully native: memory card (PCSX2 .ps2 image compatible), pad via SDL3, faithful DualShock 2 rumble
0.6   PAL / JP discs (address table per disc; new code uses it now) | 0.7 public playtest (publish own code)
1.0   Clean-up, GS reference toggle, 30/60 switch, performance
1.1   Launcher | 1.2 PC audio + movies | 1.3 widescreen Hor+ | 1.4 uncapped fps | 1.5 higher res + crisp HUD
1.6   Modern enhancements, off by default (AO, shadows, fog, water, GI, emissive, smoke, sky) | 1.7 PC textures/meshes
1.8   Linux / macOS | 2.0 byte-matching decomp (under way) | 3.0 ray tracing toggles (under PS2 postfx)
```

## Working reference (short; full version with every switch and tool: `docs/reference.md` "Working reference")

- **Where to look first:** grep `docs/` before re-deriving a fact. Topic docs: `roadmap`, `native_renderer`,
  `native_bugs`, `native_2d`, `gs_off`, `postfx`, `movies`, `native_sound`, `llm_jobs`, `vu1_scheduling`, `upstream_ps2recomp`, `decomp_dare`, `lessons`.
- **PC access:** `ssh 10.0.0.202` (ping blocked; wake-on-LAN), repo `C:\dev\PoP_Decomp`. The PC can't pull from the
  Mac: git bundle to `game\jobs\pop.bundle`, then `git pull --ff-only`. PowerShell over SSH via `-EncodedCommand`;
  complex env strings in a wrapper under `game\jobs\`. Windowed jobs need the desktop session:
  `tools\windows\start-job.ps1 -Script <ps1>` (log `game\jobs\job.log`), watched with `tools/windows/watch-job.sh`
  under Monitor. git on the PC has no identity (`git -c user.name=… -c user.email=…`). Before a boot: user's game
  closed (`taskkill /f /pid`, check `tasklist`).
- **Builds:** PC `tools\windows\rebuild.ps1 -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc`
  with `-Fast` (override/runtime-only) or `-SkipGhidra` (recompiler/runtime headers, 30-50 min). Runtime patches:
  `tools/patches/pop-sotc/` (up to 54; 51-54 not yet on the PC; Mac: `~/tools/PS2Recomp-sotc` has all 54, `game/recomp-sotc/runtime-src` on the Mac is stale; after `git am` also check out its HEAD in `game\recomp-sotc\runtime-src`, the build reads that clone). Mac builds the `pop` branch only
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
