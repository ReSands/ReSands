# Project brief: Prince of Persia: The Sands of Time, PS2 recompilation and PC modernisation

Started 11 Oct 2026; this file was cleaned up on 22 Oct 2026 once the approach was settled (the earlier
goal, direction and task-order sections are in `docs/history.md`, the Mac tool setup in `docs/reference.md`).
Claims marked (estimate) are judgement calls, not sourced; (unverified) = a single forum post or similar.
Re-check anything marked with either before relying on it.

## Goal and approach

Personal-use project: a better PC version of Prince of Persia: The Sands of Time (Ubisoft Montreal, 2003)
than the original PC port, keeping the PS2 version's look (its haze and post-processing, which the user
prefers to the clean PC look). Method: a **static recompilation of the PS2 version** (PS2Recomp) with the
hardware replaced **at the engine level**, not emulated: a native Vulkan renderer that draws the Jade draw
manager's calls (not GS emulation), a native sound mixer driven by the engine's DARE/UbiSound calls (not
IOP/SPU2 emulation), a native decoder for the PS2 movie files behind the game's own player (not IPU emulation).
Migrate part by part; emulated parts remain until replaced. Later, optional upgrades on top (widescreen, uncapped
frame rate, higher resolution, assets from the other versions, ray tracing): see the roadmap below.

Decided 22 Oct: keep our own native renderer (the PS2 look, haze and post-processing included); no switch to the PC port's renderer, OpenJade or rePOP as a base. Why: the PC renderer is x86 code tied to the PC engine's structures (a second recomp plus a state translation); OpenJade is a closed, unlicensed engine reimplementation (the user tried it: ugly console button icons, WASD prompts hard-coded, too clean a look); rePOP replays Xbox NV2A pushbuffers (GPU emulation, nothing to plug into a PS2 base). Uses: pop-bf-lab (no licence) and OpenJade's source, if released, as format references only; rePOP for Xbox addresses and Hor+ hooks.

## Ground rules

- Personal use only. The game and its source belong to Ubisoft. Original source code is not available to us.
- Work from a legitimately owned disc (US NTSC release, serial SLUS-20743) and a legitimately owned GOG copy.
- Never commit or redistribute the ISO, ELF, game data, texture packs or recompiled game output. Keep them out of any git repo (use `.gitignore`). Only our own tooling and glue code may be shared.
- The PS2 disc is the base: the recomp must run from it alone, so movies come from a native decoder for the PS2 movie files behind the game's own player (user, 15 Oct 2026; replaces the 13 Oct plan to emulate the IPU with ps2-web-recomp's MIT IPU). Assets from other versions the user owns (GameCube, Xbox, PC) are optional enhancements on top: the "ultimate version" goal (12 Oct 2026, see "Cross-version assets" in `docs/reference.md`). This revises the earlier "PS2 disc only" rule.
- Leaked Jade source (e.g. the 2006 Rayman Raving Rabbids distribution linked from raydev.wiki) is off limits like leaked SDK code; raydev.wiki's format list is derived from it and has no specifications anyway (checked 15 Oct 2026).
- Unreleased prototypes may be used locally for research and reference (user decision, 12 Oct 2026). Like all game material, they stay under `game/` and never reach git.
- Provenance: our code (overrides, HLE replacements, tools) comes only from the PoP binary, our own analysis, public hardware documents and open-source projects under compatible licences. Never use leaked Sony SDK source, headers or samples (e.g. ezmpeg), and do not write such code from memory. Precedent: OpenRAC/rac1-decomp reverted its whole movie player for this (its `docs/MOVIE.md`).
- Be straightforward in status reports: say plainly when something does not work.
- Prefer the simplest change that works (user, 25 Oct 2026). After finishing, check whether complexity crept in and simplify before reporting. Propose a workflow change only if the same complexity has come up before.
- **Rules from the user (19 Oct):** the PC's TV is off unless the user is playing: ask before any job that boots
  the game (`nvidia-smi --query-gpu=display_active` must be Enabled). Survey other projects before fixing a
  narrowed-down problem (`docs/ps2_projects.md`); general learnings to `docs/lessons.md`; one dated job script
  per night; optimise in order of biggest win. Shut the PC down after overnight work. Playtesting: the user
  gathers saves; I work offline; milestone checks by the user.

## Current status (29 Oct 2026 evening; start here)

Older status blocks: `docs/history.md` (last moved: "Moved from CLAUDE.md on 29 Oct 2026 evening").
Keep this section short: finished work goes to the topic docs, old status blocks to `docs/history.md`.

- **Where we are.** Native renderer at roadmap 0.12, on by default in `play.ps1` (desktop shortcut "PoP 60 fps";
  `-NoNativeWorld/-NoNativeCharacters/-NoNativeTranslucent/-NoNativeShadows` turn parts off): static world, lit
  statics, characters, translucents, shadows, sprites via the P9 port. The late lists (+0x88/+0xA8, particles) stay
  on the GS (`-NativeLate` turns them on; drawn natively they made glows pop). Details `docs/native_renderer.md`,
  bugs `docs/native_bugs.md`.
- **Renderer audit (`docs/renderer_audit.md`): all five steps done 27 Oct.** Open on purpose: item 11 (hybrid
  boundary, 0.2); items 12/13 measure-only (worker ~3.7 ms per frame, the GS thread waits nearly all of it, no fps
  cost at 60; texture count/MB now end the `[pop:world]` stats line: read growth from a long `play.ps1` log). The
  nightly `render` gate (blend test + 11 GS-only saves, `tools/nightly/render_gate.txt`) guards native output;
  **reference saves must be GS-only** (saves made with native on hold native pixels). 0.13 candidates the gate showed
  (moon halo 145139, light patch 155110, sand haze 115853): `docs/native_bugs.md` step 4. Blending with As > 0x80:
  the user's call at 0.13.
- **Still needed for 0.1:** crash 1 (fix 3b80b66; close after a fight-heavy session without `missing-target`).
  Crash 2 is off the list (user, 26 Oct): watched, full dump on a freeze; no speculative emulated-sound fixes.
  Borrowed fixes placed by milestone (MOVZ/MOVN patch 43 and small screenshots patch 44 built 29 Oct):
  `docs/roadmap.md`.
- **Bugs before 0.13: done 29 Oct evening** (`docs/native_bugs.md`). N13/N14/N16 baths flicker fixed (ae260d9:
  blended/Z-less draws need a pure frame; f8daecb: draws without depth write test `POP_NATIVE_DECAL_ZBIAS`=32 GS Z
  units nearer), checked by the user in game. N15 is in the GS-only run too: revisit on real PS2 hardware (user).
  **All performance (rewind dip 155110, fight fps, N6) waits until fully native, then optimised in one go (user).**
  Rule kept: a bug caused by the hybrid frame is parked until 0.2; bugs in native code are fixed now. Bug checks:
  `play.ps1 -Mode hybrid|gs|native -Save <name>`; locate with a `.glitch` capture + the user's red circle on a frame.
- **Next: 0.13, wiring in the new layers** (agreed 29 Oct). P5 GFX particles: on by default (`POP_NATIVE_P5=0` off).
  **Water (P6) done 29 Oct night** (`docs/native_renderer.md` "Water step 1-4"): offline full water (mask, the 256x256
  refraction buffer rebuilt in mesh.frag from a mid-frame copy, colour pass, P6's own fog), live, on by default since 30 Oct
  (`POP_NATIVE_P6=0` off; native only with `-NativeLate`), user check passed on fountain 202926. Not in the PC build yet.
  Open: the mist around feet / pillars in
  water is missing natively (likely GFX particles of +0x88, which `pop_state_frame` lists but never draws; unproven).
  **Halos started 30 Oct** (`docs/native_renderer.md` "Native halos"): offline done, sun save 162621 10.6 -> 2.6; the moon
  (145139) is too bright natively (GS hides it; needs a GS trace of a moon frame on the PC); not built or booted on the PC
  yet. **Reviews 30 Oct, all done:** halos (8ede351, 22697b4: native halos were refused by a leftover sub-material),
  water (3170ab5 + cddb051: water, halos, sprites, particles and the lit env pass sent with key 0, no longer filling the
  kept-mesh cache), patches 43/44, ae260d9 (pure frame for blended draws), decal Z bias: no fixes needed for the last three.
- **Next PC session (ask about the TV first), in order:**
  1. Bundle to the PC, `-Fast` build (halos, `frameOnly`, halo purity fix are all new there).
  2. Boot check of patches 43 (MOVZ/MOVN) + 44 (640x448 screenshots: look at one shot) with P5 on: any change in behaviour.
  3. GS-only fountain save (gsref job, `tools/nightly/render_gate.txt` needs GS-only), add it as a `vu1` gate line (no gate
     save has water yet).
  4. GS trace (`PS2X_GS_TRACE`) of a moon frame, GS only, from 145139: the Z under the moon core at the halo test.
  5. `-NativeLate` run at the fountain and the sun save 162621: the `[pop:world] halos:` line should show native > 0, and no
     "kept meshes over budget" lines should appear.
  6. Shut the PC down.
  Then light shafts (FUN_00469C00, list type 0x18; the mirror beam is
  something else), flashes (full-screen after-effects: survey first), retry `-NativeLate`. Parked: N3, N4 (until they recur), N7.
  Watch: N8 / N11 in long sessions. Side jobs: `pop_world_test` dies with exit 138 on the Mac (seen 30 Oct, before the key-0 change too), `.glitch` compression, a `-Fast` rebuild that recompiled the whole
  runtime, Esc / Alt+F4 sometimes not quitting, `POP_NATIVE_SKIP` is ignored with the native world on. Lower: camera
  X reversed in fixed views, door flicker with a black web, trouser flicker, torch point lights. Missing sounds: 0.3.
- **How we judge (`docs/lessons.md`):** the user's in-motion check with clock times is the reference for short or
  camera-dependent effects; our GS emulation is not ground truth (PS2 videos / hardware are).
- **Test loop.** Edit -> bundle to the PC -> `game\jobs\stnative.ps1` via `start-job.ps1` (-Fast build, 45 s boots of
  184926 / 115837 / 115853 with R1 held; logs `game\recomp-sotc\stnative_<save>.log`), watched with
  `tools/windows/watch-job.sh` under Monitor. Profiling a save: `tools\windows\profile-states.ps1 -States <save>`
  (native on). User checks from a save: `play.ps1 -Env 'PS2X_STATE_LOAD_AT=120:states/<save>.state'`. Offline:
  `tools/native/score_saves.sh`, `pop_state_frame --world`, viewer `--gsz`. Before any boot: ask about the TV (boots
  ran at ~60 fps with it off on 27-28 Oct; check the fps lines) and make sure the user's game is closed
  (`taskkill /f /pid`, then `tasklist`).
- **Saves.** On the PC in `game\ps2\disc\states\`; Mac copies: `game/analysis/saves1025/` (115837, 115853),
  `saves1025b/` (125052 chasm, 125331 / 125417 save point in a fight), `saves1025c/` (134636 / 134815 slowed enemies),
  `saves1025d/` (145139 / 145142 save point pop), `saves1025e/` (baths), `saves1024b/play_20261024_184926.state`
  (slow fight), GS-only references `gsref1027/`.
- **VU1 ports:** every reachable VU1 program is ported (`tools/vu1port/ported/`; P4 and P5 pass whole-run chains but
  are not in the game yet; `docs/native_renderer.md` "VU1 program inventory").
- **DARE decomp (2.0 for 0.3, started 27-28 Oct night): `docs/decomp_dare.md`.** 79 of 669 byte-matched (29 Oct:
  run 2's header requests fixed in the generator, sndCList template; near misses by hand; names in the Mac Ghidra
  project). Queue resumes with the "Qwen DARE audio decomp" shortcut (the PC has the new prompt).
- **PC build:** sotc patches 43 (MOVZ/MOVN) and 44 (640x448 shots) applied and built 29 Oct evening (full `-SkipGhidra`,
  11.6 min: only the 392 changed generated files recompiled). Not booted yet: watch the first P5 boot for MOVZ/MOVN changes.
- **Known limits:** save-state boots have no sound; runtime screenshots are the game in a corner of a 9600x5400 image
  (shrink with `sips -Z 1600` before looking).

## Release roadmap (user; full text, reasons and references: `docs/roadmap.md`)

```
0.1   Playable start to finish, emulated graphics + sound (crash 1 fixed; crash 2 watched)
0.11  Native static world
0.12  Native characters, translucents, shadows + particles
0.13  Native effects: water (P6), sprites (P9), halos, light shafts, flashes
0.14  Native HUD, menus, loading screens, movie display
0.2   Fully native graphics: GS off by default, judged frame by frame against the emulated GS
0.3   Native sound (DARE/UbiSound, no IOP/SPU2) + native PS2 movie decoder (no IPU)
0.4   Fully native: memory card, pad, faithful DualShock 2 rumble
1.0   Clean-up (Doom 3 style guide, golden outputs), GS reference toggle; publish own code only; 30 fps like the original
1.1   Widescreen, Hor+ (4:3 default)
1.2   Uncapped frame rate (30/60 caps kept)
1.3   Native higher resolution + crisp HUD
1.4   Assets from PC + Xbox: textures, static meshes, skinned meshes, audio, movies (PS2 disc alone still complete)
1.5   Linux / macOS
1.6   Ray tracing (each effect a toggle, drawn before the PS2 post-processing)
1.7   DualSense haptics + adaptive triggers
--    PAL / JP discs (new code takes addresses from one table per disc)
2.0   (potential) Byte-matching decomp alongside the recomp, for historical study
Dropped: DLSS/FSR/XeSS, GameCube assets (23 Oct)
```

## Portability principle (user, 15 Oct 2026)

Windows is the target; Linux and macOS (Apple Silicon, via MoltenVK) should stay cheap to add later. Rules for new native code (renderer, sound, movies): Vulkan only, within what MoltenVK supports for the base path (no geometry shaders etc.); ray tracing as an optional extension; window, input, audio via portable libraries (SDL-class, FFmpeg); standard C++ without x86 intrinsics, or behind a wrapper with a portable fallback; no Windows-only APIs; shaders as ahead-of-time SPIR-V. Expected later effort (estimate): Linux days to ~2 weeks; macOS 1-3 weeks. Do not give up a fast x86 path where it matters, but keep a portable one next to it.

## Working reference (distilled 17 Oct 2026; the details and their history are in `docs/history.md` and `docs/reference.md`)

- **Where things are written down.** `docs/history.md`: dated session logs (boot log 11-12 Oct, sound bring-up 13-14 Oct, mesh reader / Stage A-C findings 14-16 Oct, old status blocks and plans). `docs/reference.md`: background that is not a session log (PS2Recomp pipeline/PRs/status, prototypes and symbols, cross-version assets, texture formats, survey of other PS2 projects, RTX Remix, HD packs, renderer/DLSS notes, sources, answered open questions). Topic docs: `docs/roadmap.md` (release roadmap in full), `docs/native_renderer.md` (Stage B-D, live window), `docs/postfx.md`, `docs/movies.md`, `docs/llm_jobs.md`, `docs/vu1_scheduling.md` (build-time VU scheduling plan), `docs/upstream_ps2recomp.md` (our PS2Recomp bug list), `docs/nightly/`. Before re-deriving a fact, grep these.
- **Builds.** PC (the real target, pop-sotc runtime, `game\recomp-sotc`): `tools\windows\rebuild.ps1 -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc` with `-Fast` (override/runtime-only changes, ~30 s-few min) or `-SkipGhidra` (recompiler or runtime-header changes, full ~30-50 min); boot `tools\windows\boot.ps1 -Play -Out game\recomp-sotc [-Seconds N] [-Env 'A=1;B=2'] [-Shots <fields>]`. Runtime patches live in `tools/patches/pop-sotc/` (`git am` into `C:\tools\PS2Recomp-sotc`, then `git -C game\recomp-sotc\runtime-src checkout --detach` its HEAD; currently up to 44 (39 = Esc quits, 40 = GS trace from an override, 41/42 = scheduler stub return, replaced functions only; 43 = MOVZ/MOVN low 64 bits, 44 = 640x448 screenshots; all applied on the PC); remote `sotc` = TaylorNAlbarnaz/PS2Recomp). Compiled VU1/VU0 microcode: record with `PS2X_VU1_CAPTURE=game\recomp-sotc\vu1_capture` (both units since patch 31) or take the images from saves (`tools/vuimages.py <dir> <states>`), build with `rebuild.ps1 ... -Vu1 <that dir>` (all runner files recompile, ~30 min). Never put `vu0_*.bin` in a capture folder an older `vu1_recomp` reads (it expects 16 KB images). The play build links with /MAP (symbols for the profiler). Mac: the sotc base does not build there (x86 headers); the Mac uses the `pop` branch (`tools/rebuild.sh [--fast]`, `POP_BUILD=build-mac-play tools/boot.sh [secs] [log]`, exit 142 = time limit); Mac-only problems (medallion drift, curtain-load stall) are parked. Native tools: `cmake -S tools/native -B game/native-build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build game/native-build` (Mac: Homebrew molten-vk, vulkan-loader, vulkan-headers, glslang, SDL3).
- **PC access.** `ssh 10.0.0.202` (ping is blocked; wake-on-LAN in the dated launchers), repo `C:\dev\PoP_Decomp`. The PC cannot pull from the Mac: send a git bundle to `game\jobs\pop.bundle` and `git pull --ff-only` it. Run PowerShell over SSH with `-EncodedCommand`; complex env strings go in a wrapper script under `game\jobs\`. Long jobs: `powershell -ExecutionPolicy Bypass -File tools\windows\start-job.ps1 -Script <ps1>` (log `game\jobs\job.log`, `-Status`; second slot task `PoP_Decomp_Job2`); anything with a window needs the desktop session (scheduled task, not plain SSH). git on the PC has no identity (`git -c user.name=… -c user.email=…`). `tools/stop-pc-job.command` stops a job; `shutdown /s /t 5` over SSH turns the PC off.
- **Save states.** F5 or Select (pad) quick-saves (`states\quick.state`, plus a kept copy `play_<date>_<time>.state`). Load with `PS2X_STATE_LOAD_AT=<field>:states/<file>` (a field number, ~60 per second: 120 = 2 s after boot). PC states: `gameplay1` (0102_Remparts, after the curtains), `enemies` (fight on the 0101_Entree ramparts, saved at field 22625), `before_curtain`, `loading`. Limits: no sound after a load, no bigfile lookups and no `ReadElement`; the native renderer still works (25 Oct: elements decoded from their packets, world packs loaded through `game/analysis/packindex.txt` on a texture miss). Fight boots: `POP_INPUT=r1@1:3600` (hold block); job `game\jobs\stnative.ps1` (build + boot saves, prints the world/late/shadow/sprite counters).
- **Debug switches** (override `tools/overrides/pop_slus20743.cpp` and friends): `POP_INPUT` (scripted pad, `button@s[:hold]`, sticks `lup/ldown/lleft/lright`; balcony: `start@55,start@65,cross@85,cross@95,cross@105`), `POP_60FPS=1`, `POP_ASYNC_GS=1`, `POP_PASSDUMP_AT`, `POP_SNDLOG`, `POP_TRACE`, `POP_TEXLOG`, `POP_PACKLOG`, `POP_CAMLOG` + `POP_FRAMECAP=<file>:<n>` (+ `POP_DMACAP`), `POP_NATIVE_LIVE` / `POP_NATIVE_FRAME=<file>:<s>` / `POP_NATIVE_WINDOW=<port>`, `POP_MOVIE_SOURCE=bink`, `POP_VK_VALIDATION=1` (Khronos layer, any native renderer; Mac: also `DYLD_LIBRARY_PATH=/opt/homebrew/lib VK_LAYER_PATH=/opt/homebrew/share/vulkan/explicit_layer.d`), `POP_KEEP_SAVES=0`. Runtime: `PS2X_GS_TRACE=<field>:<n>:<path>` (n >= 4 at 30 fps; decode with `tools/gsvram.py`), `PS2X_AUDIO_DUMP` (header only on clean exit), `PS2X_ADMA_DUMP`, `PS2X_SPU2_STATS`, `PS2X_INVERT_RIGHT_STICK=x` (forces it; default now: the override inverts X in the third-person camera only, `POP_CAMERA_CLASSIC=1` keeps the original), `PS2X_EE_FLOAT_NEAREST=1`, `PS2X_FULLSCREEN=1`, `PS2X_DEBUGGER=0`, `PS2X_FRAME_GRAPH=1`, `PS2X_VU1_CAPTURE=<dir>`, `PS2X_VU1_TIMING=<scale>` (0.5 by default at 60 fps), `PS2X_VU0_VERIFY=1` / `PS2X_VU1_VERIFY=1` (compiled vs interpreter), `PS2X_VU0_RECOMP=0`. Profiler: `POP_PROFILE=<file>:<start s>:<seconds>`, or rolling `POP_PROFILE_LAST=<s>` (every kept quick-save also writes `play_<date>_<time>.prof` of the seconds before it; desktop shortcut "PoP 60 fps (profiling)" = `play.ps1 -Profile`, symbolized to `.report.txt` when the game closes; costs ~1-3 fps in a fight, noisy) (`tools/overrides/pop_profiler.cpp`; symbolize with `tools/profsym.py <file> <map or exe>`; PC: `tools/windows/profile-states.ps1`).
- **Override rules learned the hard way.** On the PC, never rely on code after calling the original of a generated function: the scheduler can unwind mid-call and resume later (compare `ctx->pc` with the entry `$ra`). Call generated functions by address (`callOriginal`), not by generated name. Self tail calls inside generated code are internal jumps, so a hook sees only the first call of a chain. PS2Recomp substitutes its own C++ for any function ps2_analyzer names (wrong for libvu0, `rand`, `sceSdRemote*`): rebuild scripts rename those so the game's own code is recompiled; check any new stand-in before trusting it.
- **Key addresses (retail SLUS_207.43).** Graphics object pointer 0x675148 (world->clip at +0x240, row vectors); Display3DElements 0x4BC000; CElementList::Render 0x4B9440, CLMElementList::Render 0x4B7B80; CJMeshElement::Display 0x4C8A80 / ::ReadElement 0x4C9500; CJMeshElementRef::Display 0x4C7B40; matrix pool *(0x70000260); texture manager *(0x70000050), LoadTexture 0x4B3840, PS2_Texture_Load 0x4B4E80; PS2_Flip 0x4A20C0, DMA kick 0x4A2C80; FullMovie wrappers 0x19F440-0x19F700; BIG_ul_SearchKeyToPos 0x488940; 60 fps word 0x66D044. Prototype names are applied in Ghidra (`PoP_SLUS20743`); the prototype ELF has its own project `PoP_Proto0819`.
- **Tools you will reach for.** `tools/jadetex.py` (textures), `tools/jade2gltf.py` (packs -> glTF, `--place`), `tools/chainskin.py` / `chainfar.py` / `vifwalk.py` / `dmacmp.py` (captured DMA chains), `tools/vudis.py` (VU1), `tools/gsvram.py` / `gslm.py` (GS memory), `tools/c2py.py` + `c2py_rt.py` (Ghidra C -> Python), `tools/mmitest/run.sh` (MMI/FPU instruction tests), `tools/livecmp.py` (live window vs GS; also `score` for any two frames), `tools/popstate.py` (read a save: RAM, GS regs, VRAM; `--frame` = the game's own picture), `tools/statechain.py` (a save's last DMA chain: work per frame), `tools/packindex.py` (texture key -> world pack), `pop_state_frame` (native frame from a save; `docs/native_renderer.md`), `tools/windows/state-shots.ps1` (screenshots + fps of every kept save), `tools/nightly/` (checks + report), `tools/llm_port/` (local-model jobs).
- **Local models.** Qwen3.6-35B-A3B via llama-server (task LlamaServe, port 8033) ports medium functions from a precise spec + strict judge in 1-4 rounds; keep tasks small and staged; test the runner end to end with a known-good answer. Gemma (Ollama) only writes reports from scripted findings. Details `docs/llm_jobs.md`.
- **Practical.** Judge sound on the TV or from a WAV, never over Moonlight. Mac GUI boots need the display awake (`caffeinate -u -t 2`). macOS has no `timeout`: `perl -e 'alarm N; exec @ARGV' …`. Python on the PC defaults to cp1252: open text files with `encoding='utf-8'`.

## Inputs

- PS2 disc image (ISO) of the US release, dumped from the owner's own copy. ELF should be `SLUS_207.43`.
- GOG copy of the PC version (includes GOG's preservation-program wrapper: widescreen/ultrawide, camera scaling, FPS limit, multi-monitor mouse lock).
- PS2Recomp: https://github.com/ran-j/PS2Recomp (GPL-3.0).

## Local layout

All game material lives under `game/`, which is git-ignored. Never move game files outside it.

- `game/ps2/`: the downloaded `.7z`; `iso/` (the ISO); `disc/` (extracted disc contents, incl. `SLUS_207.43` and `DATA/PRINCE.BF`).
- `game/gog/installers/`: GOG offline installer (`setup_…_181_gog_v2_(92028).exe` + `-1.bin`) and the 28547→92028 patch. Install output, if extracted on the Mac, goes in `game/gog/install/`.
- `game/ghidra/`: Ghidra project `PoP_SLUS20743` for the PS2 ELF, plus headless logs.
- `game/gc/` (GameCube), `game/xbox/` (Xbox), each with `iso/`, `disc/`, `analysis/`; `game/proto/` (PS2 prototype).
