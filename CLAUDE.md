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

## Current status (25 Oct 2026 afternoon; start here)

Older status blocks and handoffs 1-4: `docs/history.md` (last moved: "Moved from CLAUDE.md on 25 Oct 2026 afternoon").
Keep this section short: finished work goes to the topic docs, old status blocks to `docs/history.md`.

- **Where we are.** Native renderer between roadmap 0.12 and 0.13, all on by default in `play.ps1` (desktop shortcut
  "PoP 60 fps"; `-NoNativeWorld/-NoNativeCharacters/-NoNativeTranslucent/-NoNativeShadows/-NoNativeLate` turn parts
  off): static world, lit statics, characters, translucents, shadows, the lists after the shadows (+0x88/+0xA8,
  particles), sprites via the P9 port. User session 25 Oct 12:46: ~60 fps, every shadow projector native. Native
  rendering works after a save-state load (packs via `packindex.txt`). Details: `docs/native_renderer.md`.
- **Agreed order (user, 25 Oct):** (1) 0.12 done: shadow receivers fixed (N12, e5ba9a5; the 25 Oct play log: all projectors native); (2) **now:
  the bug sweep below**; (3) then 0.13 (native water P6, halos, light shafts, flashes).
- **Bug sweep, in this order** (details per bug in `docs/native_bugs.md`, crashes in `docs/saves_1021.md`):
  1. **Crash 1** (scheduler resuming at 0x172844): fix 3b80b66; 25 Oct a 300 s R1 fight from 202314 ran clean.
     Close after a long user fight session without it. Needed for 0.1.
  2. **Crash 2**: cause unknown. 25 Oct: the save's threads look healthy and it runs on when loaded (no freeze), so
     it needs something a save-state boot lacks (sound is a guess, not evidence). User decision 25 Oct: do not fix the
     emulated sound speculatively (native sound replaces it); only act on a reproduced freeze. Needed for 0.1.
  3. **N8/N9/N11 stale per-address caches**: fix 3664837 built 25 Oct (ReadElement drops the element's entries);
     in-game check pending (201124's door after the staircase climb).
  4. **N1 slowed enemies not sandy brown** (user: highest of N1-N4). Needs a hand-taken save with an enemy slowed
     (scripted triangle slowed nobody); FX_Freeze_* is the time scale, not the tint (`docs/native_bugs.md`).
  5. **N10 + save-point glow pop** (torch/glow brightness changes as the camera turns; needs the user at a torch:
     save boots walk away from it): the halo pass (FUN_004ABFC0, GS)
     tests visibility against Z that partly comes from the native world (biased by ZBIAS 128). Test: default vs
     `POP_NATIVE_WORLD_ZBIAS=0` vs `-NoNativeWorld` at one torch and one save point. Save point partly drawn in a fight
     (125417): **closed 25 Oct**, a PS2 playthrough video shows the same (sand glow only, column fades in over the fight).
     A GS reference (an old GSdx plugin build fed our GIF stream, est. a few days) deferred to 0.2 (user asked).
  6. **N2 fog blips on camera turns, N3 stairs flash, N4 camera "teleport" in combat.**
  7. **N5 black one-frame flashes**: fix 30ef5d5 built 24 Oct; confirm none in the 25 Oct play log, then close.
  8. **Small ones:** `.glitch` compression + delete triaged captures; why a `-Fast` rebuild once recompiled the whole runtime.
  Also open, lower: rewind at 30 fps (not reproduced), fight fps (save boots ~49-53 fps: worker ~5 ms, GS thread waits
  ~4.8 ms; profile 184926 / 115837), camera X reversed in fixed views, door flicker with a black web, trouser flicker
  (watch), torch point lights in game, audio bugs (quiet/missing fight track, dialogue out of sync, sound slows with
  the frame rate), Alt+F4 once did not close the game.
- **Test loop.** Edit -> bundle to the PC -> `game\jobs\stnative.ps1` via `start-job.ps1` (-Fast build, 45 s boots of
  184926 / 115837 / 115853; R1 held only since 25 Oct: boot.ps1 used to clear an -Env POP_INPUT, 2160f01; prints fps + world/late/shadow/sprite counters; logs
  `game\recomp-sotc\stnative_<save>.log`), watched with `tools/windows/watch-job.sh` under Monitor. A/B of one save
  with native on vs off + screenshots: `game\jobs\glowab.ps1` (edit the save name). Save-state boots have no sound and
  miss long-session bugs (N8-N11); those need the user's play sessions. Before any boot: TV on (ask), and the user's
  game closed (`taskkill /f /pid`, confirm with `tasklist`).
- **Saves.** On the PC in `game\ps2\disc\states\`; copies on the Mac: `game/analysis/saves1025/` (115837, 115853),
  `saves1025b/` (125052 chasm reference, 125331 / 125417 save point in a fight, play log 124621),
  `saves1024b/play_20261024_184926.state` (slow fight). Letterbox (panorama) saves are wanted: they show far geometry.
- **Qwen tonight:** P4 character VU1 program, `tools/windows/oct25.ps1` (shortcut "Qwen overnight (until Stop
  Qwen)"; shuts the PC down). Morning: `game\jobs\qwen-night.log`, copy `game\analysis\qwen\vu1_p4_*` to the Mac,
  hand-port the failures, `port.py --chain p4`. `docs/llm_jobs.md` "VU1 character program P4"; before the next
  non-VU1 Qwen job, build in the ReAgent learnings (same doc).
- **Known limits:** save-state boots have no sound; runtime screenshots are the game in a corner of a 9600x5400 image.

## Release roadmap (user; full text, reasons and references: `docs/roadmap.md`)

```
0.1   Playable start to finish, emulated graphics + sound (the two 21 Oct combat freezes fixed)
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
Dropped: DLSS/FSR/XeSS, GameCube assets (23 Oct)
```

## Portability principle (user, 15 Oct 2026)

Windows is the target; Linux and macOS (Apple Silicon, via MoltenVK) should stay cheap to add later. Rules for new native code (renderer, sound, movies): Vulkan only, within what MoltenVK supports for the base path (no geometry shaders etc.); ray tracing as an optional extension; window, input, audio via portable libraries (SDL-class, FFmpeg); standard C++ without x86 intrinsics, or behind a wrapper with a portable fallback; no Windows-only APIs; shaders as ahead-of-time SPIR-V. Expected later effort (estimate): Linux days to ~2 weeks; macOS 1-3 weeks. Do not give up a fast x86 path where it matters, but keep a portable one next to it.

## Working reference (distilled 17 Oct 2026; the details and their history are in `docs/history.md` and `docs/reference.md`)

- **Where things are written down.** `docs/history.md`: dated session logs (boot log 11-12 Oct, sound bring-up 13-14 Oct, mesh reader / Stage A-C findings 14-16 Oct, old status blocks and plans). `docs/reference.md`: background that is not a session log (PS2Recomp pipeline/PRs/status, prototypes and symbols, cross-version assets, texture formats, survey of other PS2 projects, RTX Remix, HD packs, renderer/DLSS notes, sources, answered open questions). Topic docs: `docs/roadmap.md` (release roadmap in full), `docs/native_renderer.md` (Stage B-D, live window), `docs/postfx.md`, `docs/movies.md`, `docs/llm_jobs.md`, `docs/vu1_scheduling.md` (build-time VU scheduling plan), `docs/upstream_ps2recomp.md` (our PS2Recomp bug list), `docs/nightly/`. Before re-deriving a fact, grep these.
- **Builds.** PC (the real target, pop-sotc runtime, `game\recomp-sotc`): `tools\windows\rebuild.ps1 -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc` with `-Fast` (override/runtime-only changes, ~30 s-few min) or `-SkipGhidra` (recompiler or runtime-header changes, full ~30-50 min); boot `tools\windows\boot.ps1 -Play -Out game\recomp-sotc [-Seconds N] [-Env 'A=1;B=2'] [-Shots <fields>]`. Runtime patches live in `tools/patches/pop-sotc/` (`git am` into `C:\tools\PS2Recomp-sotc`, then `git -C game\recomp-sotc\runtime-src checkout --detach` its HEAD; currently up to 41 (39 = Esc quits, 40 = GS trace from an override, 41/42 = scheduler stub return, replaced functions only); remote `sotc` = TaylorNAlbarnaz/PS2Recomp). Compiled VU1/VU0 microcode: record with `PS2X_VU1_CAPTURE=game\recomp-sotc\vu1_capture` (both units since patch 31) or take the images from saves (`tools/vuimages.py <dir> <states>`), build with `rebuild.ps1 ... -Vu1 <that dir>` (all runner files recompile, ~30 min). Never put `vu0_*.bin` in a capture folder an older `vu1_recomp` reads (it expects 16 KB images). The play build links with /MAP (symbols for the profiler). Mac: the sotc base does not build there (x86 headers); the Mac uses the `pop` branch (`tools/rebuild.sh [--fast]`, `POP_BUILD=build-mac-play tools/boot.sh [secs] [log]`, exit 142 = time limit); Mac-only problems (medallion drift, curtain-load stall) are parked. Native tools: `cmake -S tools/native -B game/native-build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build game/native-build` (Mac: Homebrew molten-vk, vulkan-loader, vulkan-headers, glslang, SDL3).
- **PC access.** `ssh 10.0.0.202` (ping is blocked; wake-on-LAN in the dated launchers), repo `C:\dev\PoP_Decomp`. The PC cannot pull from the Mac: send a git bundle to `game\jobs\pop.bundle` and `git pull --ff-only` it. Run PowerShell over SSH with `-EncodedCommand`; complex env strings go in a wrapper script under `game\jobs\`. Long jobs: `powershell -ExecutionPolicy Bypass -File tools\windows\start-job.ps1 -Script <ps1>` (log `game\jobs\job.log`, `-Status`; second slot task `PoP_Decomp_Job2`); anything with a window needs the desktop session (scheduled task, not plain SSH). git on the PC has no identity (`git -c user.name=… -c user.email=…`). `tools/stop-pc-job.command` stops a job; `shutdown /s /t 5` over SSH turns the PC off.
- **Save states.** F5 or Select (pad) quick-saves (`states\quick.state`, plus a kept copy `play_<date>_<time>.state`). Load with `PS2X_STATE_LOAD_AT=<field>:states/<file>` (a field number, ~60 per second: 120 = 2 s after boot). PC states: `gameplay1` (0102_Remparts, after the curtains), `enemies` (fight on the 0101_Entree ramparts, saved at field 22625), `before_curtain`, `loading`. Limits: no sound after a load, no bigfile lookups and no `ReadElement`; the native renderer still works (25 Oct: elements decoded from their packets, world packs loaded through `game/analysis/packindex.txt` on a texture miss). Fight boots: `POP_INPUT=r1@1:3600` (hold block); job `game\jobs\stnative.ps1` (build + boot saves, prints the world/late/shadow/sprite counters).
- **Debug switches** (override `tools/overrides/pop_slus20743.cpp` and friends): `POP_INPUT` (scripted pad, `button@s[:hold]`, sticks `lup/ldown/lleft/lright`; balcony: `start@55,start@65,cross@85,cross@95,cross@105`), `POP_60FPS=1`, `POP_ASYNC_GS=1`, `POP_PASSDUMP_AT`, `POP_SNDLOG`, `POP_TRACE`, `POP_TEXLOG`, `POP_PACKLOG`, `POP_CAMLOG` + `POP_FRAMECAP=<file>:<n>` (+ `POP_DMACAP`), `POP_NATIVE_LIVE` / `POP_NATIVE_FRAME=<file>:<s>` / `POP_NATIVE_WINDOW=<port>`, `POP_MOVIE_SOURCE=bink`, `POP_KEEP_SAVES=0`. Runtime: `PS2X_GS_TRACE=<field>:<n>:<path>` (n >= 4 at 30 fps; decode with `tools/gsvram.py`), `PS2X_AUDIO_DUMP` (header only on clean exit), `PS2X_ADMA_DUMP`, `PS2X_SPU2_STATS`, `PS2X_INVERT_RIGHT_STICK=x` (forces it; default now: the override inverts X in the third-person camera only, `POP_CAMERA_CLASSIC=1` keeps the original), `PS2X_EE_FLOAT_NEAREST=1`, `PS2X_FULLSCREEN=1`, `PS2X_DEBUGGER=0`, `PS2X_FRAME_GRAPH=1`, `PS2X_VU1_CAPTURE=<dir>`, `PS2X_VU1_TIMING=<scale>` (0.5 by default at 60 fps), `PS2X_VU0_VERIFY=1` / `PS2X_VU1_VERIFY=1` (compiled vs interpreter), `PS2X_VU0_RECOMP=0`. Profiler: `POP_PROFILE=<file>:<start s>:<seconds>`, or rolling `POP_PROFILE_LAST=<s>` (every kept quick-save also writes `play_<date>_<time>.prof` of the seconds before it; desktop shortcut "PoP 60 fps (profiling)" = `play.ps1 -Profile`, symbolized to `.report.txt` when the game closes; costs ~1-3 fps in a fight, noisy) (`tools/overrides/pop_profiler.cpp`; symbolize with `tools/profsym.py <file> <map or exe>`; PC: `tools/windows/profile-states.ps1`).
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
