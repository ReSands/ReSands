# Project brief: Prince of Persia: The Sands of Time, PS2 recompilation and PC modernisation

Written 11 Oct 2026 from a planning chat. Claims marked (estimate) are judgement calls, not sourced. Claims marked (unverified) came from a single forum post or similar. Re-check anything marked with either before relying on it.

## Goal

Personal-use project to get a better PC version of Prince of Persia: The Sands of Time (Ubisoft Montreal, 2003) than the original PC port, using:

1. A static recompilation of the PS2 version (PS2Recomp), with a modern renderer.
2. Community HD texture packs, integrated into that renderer.
3. Later: DLSS (and possibly path tracing).

In parallel, test a much cheaper route: RTX Remix on the existing PC (GOG) version.

## Ground rules

- Personal use only. The game and its source belong to Ubisoft. Original source code is not available to us.
- Work from a legitimately owned disc (US NTSC release, serial SLUS-20743) and a legitimately owned GOG copy.
- Never commit or redistribute the ISO, ELF, game data, texture packs or recompiled game output. Keep them out of any git repo (use `.gitignore`). Only our own tooling and glue code may be shared.
- The PS2 disc is the base: the recomp must run from it alone, so movies come from a native decoder for the PS2 movie files behind the game's own player (user, 15 Oct 2026; replaces the 13 Oct plan to emulate the IPU with ps2-web-recomp's MIT IPU). Assets from other versions the user owns (GameCube, Xbox, PC) are optional enhancements on top: the "ultimate version" goal (12 Oct 2026, see "Cross-version assets" in `docs/reference.md`). This revises the earlier "PS2 disc only" rule.
- Leaked Jade source (e.g. the 2006 Rayman Raving Rabbids distribution linked from raydev.wiki) is off limits like leaked SDK code; raydev.wiki's format list is derived from it and has no specifications anyway (checked 15 Oct 2026).
- Unreleased prototypes may be used locally for research and reference (user decision, 12 Oct 2026). Like all game material, they stay under `game/` and never reach git.
- Provenance: our code (overrides, HLE replacements, tools) comes only from the PoP binary, our own analysis, public hardware documents and open-source projects under compatible licences. Never use leaked Sony SDK source, headers or samples (e.g. ezmpeg), and do not write such code from memory. Precedent: OpenRAC/rac1-decomp reverted its whole movie player for this (its `docs/MOVIE.md`).
- Be straightforward in status reports: say plainly when something does not work.

## Current status (19 Oct 2026 night; start here)

Older status blocks: `docs/history.md` (last one: "19 Oct 2026 evening"). Tonight's details: `docs/native_renderer.md`
"Restart, 19 Oct evening".

- **Back on the native renderer (user, 19 Oct).** Agreed order (details in `docs/native_renderer.md` "Plan of attack"):
  renderer 1 native static world in the game window (the 60 fps win) -> 2 characters + particles native (GS stops
  drawing 3D) -> 3 gaps (water P6, sprites P9, shadows, halos, light shafts/mirror beams, flashes/white-outs) ->
  4 2D/HUD/menus/letterbox -> 5 GS off by default (reference toggle). Then sound: save-state boots with sound
  first, sound-call map, native mixer, dual-render comparison. Then IOP (pad via SDL, memory card as files),
  native frame pacing. Movies are already native (16 Oct).
- **Playtesting plan (agreed 19 Oct):** the user does a data-gathering playthrough on the current build soon
  (quick-saves at every new room/effect/menu/cutscene, in chunks); I work offline from saves and the user checks
  milestones; one full verification playthrough at the end. Sound is the playtest-heavy part until save-state
  boots have sound.
- **Measured tonight (PC, TV on; `game\analysis\native\prize\fps_<group>.txt`, `POP_NATIVE_SKIP`, measurement
  only):** skipping the **static world** (non-skinned CJMeshElement, CJMeshElementRef, CJIdxMesh) puts **every**
  fight/slow save at a steady 60 (were 46-56). Skipping lightmaps or GFX particles: no gain (cheap). Skipping
  characters: little, but most character calls had to stay (they switch the VU1 driver). Profiles of the fight
  185602: nothing skipped = VU thread 88% busy, game thread 46% in sceGsSyncPath; all skipped = VU 53%, game
  thread 35% idle waiting for vblank (headroom). **So renderer step 1 alone removes the fight dips.**
- **Baseline (Mac):** `tools/native/score_saves.sh` over the 49 saves of 19 Oct: all render, mean difference
  18.6, no regressions (`game/analysis/play1019/native/`). **VU1 inventory** (`tools/vu1inventory.py`): P1 static
  + lightmaps (most runs), P4/P7/P8 characters, P5 particles, P9 sprites, **P6 = water** (the old "program 7").
- **Water VU1 traces recorded** (`PS2X_VU1_TRACE`, runtime-built-in): `game/analysis/vu1trace/` (Mac and PC):
  courtyard pool 091049 (240 P6 runs) and healing pool 091734 (360), with all other programs and the code
  images. Record format: `ps2_vu1_core.cpp` traceVu1Run ('V1TR', maxCycles, image hash, tick, TraceState as the
  MSVC build lays it out, then VU data deltas against the previous record); `traceRestore` exists in the runtime.
- **Overnight plan, 19-20 Oct (user: fill the night; no game boots, the TV is off):**
  1. **Main work: renderer step 1, native static world in the game window** (`docs/native_renderer.md` "Plan
     of attack"). In-process Vulkan drawing in the pop-sotc runtime (port the presenter to Vulkan rather than GL
     interop; reuse `tools/native/` renderer + the live capture in `pop_native_live.cpp`), the static draw
     paths stop building DMA (as `POP_NATIVE_SKIP=static`, keeping the VU1 driver switches), the emulated GS
     draws the rest on top; behind a switch so the current build stays the default until tested. Validate
     offline all night: shared drawing code through `pop_state_frame` + `tools/native/score_saves.sh` (scores
     must not get worse), compile on the PC (builds need no display; `rebuild.ps1 -Fast -Play`, or the full
     pipeline if runtime headers change). **First boot test in the morning with the user (TV on).**
  2. **Background: water port for Qwen.** Mac trace replayer = the judge (build `ps2_vu1_core.cpp` x86_64 like
     `tools/vu1sched/check.sh`; check the MSVC TraceState layout against clang's; restore with `traceRestore`,
     run the interpreter with a packet sink, write each run's GIF output), stage P6 along its loops
     (`tools/vu1sched/loops.py`), a VU variant of `tools/llm_port/generic/` (test the runner end to end with a
     known-good answer first, e.g. vu1_recomp's code for one stage), start the queue (model on the PC via
     llama-serve, judge on the Mac under caffeinate). Set it up early so it runs while step 1 is worked on.
  Leave for the user: PC on, TV off, Mac awake.
- **Rules from the user (19 Oct):** the PC's TV is off unless the user is playing: ask before any job that boots
  the game (`nvidia-smi --query-gpu=display_active` must be Enabled, else boots run at ~4 fps; `oct19.ps1 -Native`
  refuses). Survey other projects before fixing a narrowed-down problem (`docs/ps2_projects.md`); general
  learnings to `docs/lessons.md`; one dated job script per night (today `tools/windows/oct19.ps1`, modes `-Sched`,
  `-Experiments`, `-Native [-TracesOnly]`); optimise in order of biggest win.
- **Open items carried over:** first-person X fix built 19 Oct, not yet confirmed by the user; audio bugs for
  later (a quiet/missing track in one fight, Prince-Farah in-engine dialogue out of sync, sound slows with the
  frame rate); the PC's play build is the scheduled VU1 build (patch 37) plus `POP_NATIVE_SKIP`.
- **Known limits:** save-state boots have no sound; runtime screenshots come out as the game in a corner.

## Working reference (distilled 17 Oct 2026; the details and their history are in `docs/history.md` and `docs/reference.md`)

- **Where things are written down.** `docs/history.md`: dated session logs (boot log 11-12 Oct, sound bring-up 13-14 Oct, mesh reader / Stage A-C findings 14-16 Oct, old status blocks and plans). `docs/reference.md`: background that is not a session log (PS2Recomp pipeline/PRs/status, prototypes and symbols, cross-version assets, texture formats, survey of other PS2 projects, RTX Remix, HD packs, renderer/DLSS notes, sources, answered open questions). Topic docs: `docs/native_renderer.md` (Stage B-D, live window), `docs/postfx.md`, `docs/movies.md`, `docs/llm_jobs.md`, `docs/vu1_scheduling.md` (build-time VU scheduling plan), `docs/upstream_ps2recomp.md` (our PS2Recomp bug list), `docs/nightly/`. Before re-deriving a fact, grep these.
- **Builds.** PC (the real target, pop-sotc runtime, `game\recomp-sotc`): `tools\windows\rebuild.ps1 -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc` with `-Fast` (override/runtime-only changes, ~30 s-few min) or `-SkipGhidra` (recompiler or runtime-header changes, full ~30-50 min); boot `tools\windows\boot.ps1 -Play -Out game\recomp-sotc [-Seconds N] [-Env 'A=1;B=2'] [-Shots <fields>]`. Runtime patches live in `tools/patches/pop-sotc/` (`git am` into `C:\tools\PS2Recomp-sotc`, then `git -C game\recomp-sotc\runtime-src checkout --detach` its HEAD; currently up to 34, on branch `up1017` there; remote `sotc` = TaylorNAlbarnaz/PS2Recomp). Compiled VU1/VU0 microcode: record with `PS2X_VU1_CAPTURE=game\recomp-sotc\vu1_capture` (both units since patch 31) or take the images from saves (`tools/vuimages.py <dir> <states>`), build with `rebuild.ps1 ... -Vu1 <that dir>` (all runner files recompile, ~30 min). Never put `vu0_*.bin` in a capture folder an older `vu1_recomp` reads (it expects 16 KB images). The play build links with /MAP (symbols for the profiler). Mac: the sotc base does not build there (x86 headers); the Mac uses the `pop` branch (`tools/rebuild.sh [--fast]`, `POP_BUILD=build-mac-play tools/boot.sh [secs] [log]`, exit 142 = time limit); Mac-only problems (medallion drift, curtain-load stall) are parked. Native tools: `cmake -S tools/native -B game/native-build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build game/native-build` (Mac: Homebrew molten-vk, vulkan-loader, vulkan-headers, glslang, SDL3).
- **PC access.** `ssh 10.0.0.202` (ping is blocked; wake-on-LAN in the dated launchers), repo `C:\dev\PoP_Decomp`. The PC cannot pull from the Mac: send a git bundle to `game\jobs\pop.bundle` and `git pull --ff-only` it. Run PowerShell over SSH with `-EncodedCommand`; complex env strings go in a wrapper script under `game\jobs\`. Long jobs: `powershell -ExecutionPolicy Bypass -File tools\windows\start-job.ps1 -Script <ps1>` (log `game\jobs\job.log`, `-Status`; second slot task `PoP_Decomp_Job2`); anything with a window needs the desktop session (scheduled task, not plain SSH). git on the PC has no identity (`git -c user.name=… -c user.email=…`). `tools/stop-pc-job.command` stops a job; `shutdown /s /t 5` over SSH turns the PC off.
- **Save states.** F5 or Select (pad) quick-saves (`states\quick.state`, plus a kept copy `play_<date>_<time>.state`). Load with `PS2X_STATE_LOAD_AT=<field>:states/<file>` (a field number, ~60 per second: 120 = 2 s after boot). PC states: `gameplay1` (0102_Remparts, after the curtains), `enemies` (fight on the 0101_Entree ramparts, saved at field 22625), `before_curtain`, `loading`. Limits: no sound after a load, no bigfile lookups and no `ReadElement` (so no native conversion yet).
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

## Mac tool setup (done 11 Oct 2026)

- Tools live outside the repo in `~/tools/`: `ghidra_12.1.4_PUBLIC/` (EE Reloaded v2.1.38 installed in `Ghidra/Extensions/`), `PS2Recomp/` (clone).
- Java: Homebrew `openjdk@21`. Ghidra's launcher does not find it on its own in non-interactive shells; export it first:
  `export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home PATH=/opt/homebrew/opt/openjdk@21/bin:$PATH`
- Ghidra ships no macOS natives. They were built with `cd ~/tools/ghidra_12.1.4_PUBLIC/support/gradle && ./gradlew buildNatives` and copied from each module's `build/os/mac_arm_64/` into `os/mac_arm_64/`. Redo this after any Ghidra upgrade.
- EE language ID for headless imports: `r5900:LE:32:default`.
- `innoextract` (Homebrew) unpacks the GOG installer.
- PS2Recomp recompiler builds on Apple Silicon (uses sse2neon on ARM). Homebrew `cmake` + `ninja`; recompiler only:
  `cmake -S ~/tools/PS2Recomp -B ~/tools/PS2Recomp/build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF && cmake --build ~/tools/PS2Recomp/build-mac`
- Export from Ghidra headlessly (script's two `askFile` prompts are answered by positional args):
  `analyzeHeadless game/ghidra PoP_SLUS20743 -process SLUS_207.43 -noanalysis -readOnly -scriptPath ~/tools/PS2Recomp/ps2xRecomp/tools/ghidra -postScript ExportPS2Functions.java <abs>/game/recomp/config.toml <abs>/game/recomp/functions.csv`
- Recompile: `~/tools/PS2Recomp/build-mac/ps2xRecomp/ps2_recomp game/recomp/config.toml` (about 15 s, 0.8 GB RAM; 59,674 `.cpp` files, 458 MB in `game/recomp/output/`).

## Project direction (user, 14 Oct 2026)

Target: a **gold-standard recompilation**: native game code (static recompilation) with the hardware replaced **at the engine level**, not emulated: a native renderer that understands the Jade draw manager's draw calls (not GS emulation), a native sound mixer driven by the engine's sound calls (DARE/UbiSound, not IOP/SPU2 emulation), native movie decoding behind the game's own player (not IPU emulation). Migrate part by part; emulated parts remain until replaced.

Nightly validation **built and proven on the PC (14 Oct)**: Mac button `tools/overnight-pc.command` option 4 = `llm-job.ps1 -Job nightly` -> `tools/nightly/checks.py` (order: mmi ~6 s, boot from gameplay1.state 60 s with frame/ADPCM captures, then meshes over all 566 world packs: saved per pack under `game/analysis/nightly/meshes/<converter fingerprint>/`, resumed across runs, 3 workers, one process per pack; c2py_rt runaway guards turn misread sizes into errors) -> `tools/nightly/report.py` (script decides findings + evidence; Gemma explains one finding per small prompt) -> `docs/nightly/<date>.md`. First run found: 18 MMI instructions still wrong (known), **missing branch target 0x16AA0C in RPS2::UpdateBuffer** (new; switch-table candidate), meshes (67 packs so far) 39 of 1,307 geometry records mis-parsed, 11 of 797 elements without vertices. Gemma's explanations are correct but generic; the value is the scripted findings. Original plan: a check suite on the PC (instruction test, decoder record/replay, converter over all world packs with exact-size parsing, save-state boots with screenshot/log diffs against a baseline, frame capture vs conversion) produces failures by script; Gemma only turns them into grouped bug reports (`docs/nightly/<date>.md`), every line citing evidence. Scripts find wrong bytes; the model writes the reports.

## Release roadmap (user, 15 Oct 2026; renumbered 16 Oct)

- **1.0 = faithful and native.** Plays exactly like the original release, with every hardware layer replaced natively: Vulkan renderer (no GS emulation), native sound (DARE/UbiSound calls, no IOP/SPU2 emulation), a **native decoder for the PS2 movie files** behind the game's own player (no IPU emulation; this replaces the 13 Oct plan to adapt ps2-web-recomp's IPU). Judged frame by frame against the emulated GS output of the same build (target: visually indistinguishable; bit-exact pixels are unrealistic because VU1 and host floats differ, so residual differences must be measured and explained).
- **Vibration (user, 18 Oct; later):** (1) faithful rumble: the game's DualShock 2 motor commands (small on/off, large 0-255 via the pad library), ignored by the runtime today, passed through a portable library (SDL rumble: DualSense, Xbox, ...); (2) optional DualSense haptics and adaptive triggers mapped to game events, designed with Jellybit's DualSense Haptic Designer (MIT; https://github.com/Jellybit/DualSense-Haptic-Designer; outputs 4-channel WAV for the grip motors and raw trigger bytes; full haptics need USB, Bluetooth only experimental).
- **Pre-publish clean-up before 1.0 goes public** (user, 18 Oct): a final housekeeping pass over our own code (overrides, native renderer, tools, runtime patches; not the generated code) for human readability, possibly with a stronger model (e.g. Fable). Before it: write a short style guide (naming, file size, comment style, hook layout), with id Software's GPL Doom 3 source as the style reference (style, not code); freeze golden outputs (byte-identical `pop_state_frame` frames for every test save, nightly checks, movie/ADPCM/audio reference outputs). During it: behaviour-preserving changes only, small commits per subsystem, all golden checks after each; comments keep their evidence (move long histories to `docs/`, never drop the why); decide which `POP_*` debug switches stay. After it: a short playtest by the user (input, timing, window). Lighter tidying at each milestone before that (e.g. `pop_native_live.cpp`, ~1,800 lines).
- **Reference renderer toggle in 1.0** (user, 16 Oct): the emulated GS ships as an optional reference renderer, native Vulkan is the default. One key toggles between them; by default the emulated GS is fed only while shown (correct again within a frame or two after switching, since PoP re-uploads its textures each frame); a "comparison mode" setting keeps feeding it every frame for instant switching (costs the VU1 + GS work). Possibly a split-screen view. So Stage D step 16 means "GS emulation off by default", not removed.
- **1.0 runs at the original 30 fps** (user, 16 Oct): the faithful reference, like the original release.
- **1.1 = 60 fps** (user, 16 Oct; was not a separate release before). Basis: the engine is variable-timestep (`TIM_Clock_Update` scales logic by real elapsed time) and the PCSX2 one-word patch (`POP_60FPS=1`) already gives 59.9 fps on the PC (16 Oct). Work: play-test for 30 fps assumptions (physics, animation, sound, cutscenes, sand rewind) and fix them; make it a setting, 30 stays available.
- **1.2 = DLSS, incl. frame generation** (user, 15 Oct; was 1.1 until 16 Oct): DLSS upscaling to 4K at 4:3, and DLSS frame generation for 120+ fps on top of the native 60 (NVIDIA advises a base of ~60 fps, which 1.1 provides; generated frames do not cut input latency).
- **1.3 = cross-version assets: everything** (user, 16 Oct; was 1.2): optional upgrades from the other versions the user owns (GameCube, Xbox, PC), substituted by asset key (same keys on all four platforms): **textures, meshes (user, 16 Oct: mesh swaps as well as texture swaps), audio/sound effects, music, voice, video** (Xbox Bink: highest bitrate, 5.1), anything else where a version is better. Mesh swaps go through the native renderer (it draws the game's meshes, so it can draw another version's mesh for the same key); they need a decoder per platform format (Xbox/PC D3D vertex buffers, GameCube GX), and skinned meshes only swap if the bone layout matches; collision is separate data, so visual swaps do not change gameplay. Unverified: whether Xbox/PC meshes are actually more detailed (compare vertex counts per geometry key first). A per-category selector (texture source, mesh source, ...). The PS2 disc alone must still give the full game.
- **1.4 = native higher resolution and native widescreen** (optional; was 1.3). Native resolution is mostly render-target size once everything is native (2D/post-processing must scale too). Widescreen (Hor+, 4:3 stays the default): wider projection, widened game culling (findings from the PCSX2 widescreen patch; reSOT's Xbox projection/culling hooks as a reference), HUD anchored, movies/loading screen pillarboxed; check cutscene framing and edge pop-in by playing through.
- **1.5 and beyond = ray tracing / path tracing and other lighting experiments** (see the RTX Remix notes in `docs/reference.md` for one route).
- **Publish at 1.0** (user, 15 Oct), as a base for the community; 1.1 onwards follow as updates. Publishing is limited to our own code (overrides, tools, runtime patches under their licences, PS2Recomp is GPL-3.0); users supply their own disc and build it. Never ship game data, generated code or prototype-derived material.

## Portability principle (user, 15 Oct 2026)

Windows is the target; Linux and macOS (Apple Silicon, via MoltenVK) should stay cheap to add later. Rules for new native code (renderer, sound, movies): Vulkan only, within what MoltenVK supports for the base path (no geometry shaders etc.); DLSS/ray tracing as optional extensions behind a small upscaler interface (MetalFX/FSR can plug in later); window, input, audio via portable libraries (SDL-class, FFmpeg); standard C++ without x86 intrinsics, or behind a wrapper with a portable fallback; no Windows-only APIs; shaders as ahead-of-time SPIR-V. Expected later effort (estimate): Linux days to ~2 weeks; macOS 1-3 weeks. DLSS frame generation on Linux is unverified. Do not give up a fast x86 path where it matters, but keep a portable one next to it.

## Renderer task order (agreed 14 Oct 2026)

Priority (user): a native renderer using **only PS2 assets and no GS emulation** first; finish A3 (materials/blend) and A7 (skinning) before B (user, 14 Oct); higher-quality assets (PC textures, Xbox movies) only after that. Dependency order:
- A. Read everything (offline, Mac): 1 PC save-state screenshot + packs loaded (judge for A) -> 2 lighting/brightness check -> 3 materials/blend state (alpha test/blend, additive, double-sided, sky) -> 4 positions-only elements -> 5 whole levels (all packs) -> 6 coverage of all world packs -> 7 skinned mesh data.
- B. Static world native (PC): 8 **API: Vulkan (user decision 14 Oct 2026: DLSS and ray tracing support, Windows + Linux; the emulated GS renderer is OpenGL, so plan an interop layer or port the presenter)** + native pass in the presenter -> 9 load-time conversion in C++ (hook CJMeshElement::ReadElement) -> 10 per-frame capture (element lists, matrices, camera) -> 11 draw natively, hybrid with the GS, frame comparison.
- C. Characters: 12 GPU skinning with the game's bone matrices.
- D. Remove the GS: 13 particles/water/sand/shadows -> 14 post-processing shaders -> 15 2D/HUD/menus/loading screen/movie display -> 16 GS emulation off by default (kept as the reference renderer toggle).
- E. Later: 17 resolution/widescreen/frame rate/DLSS/RTX; 18 higher-quality assets.
Sound and PS2 movies are in scope too (user, 14 Oct): eventually native sound (no IOP/SPU2 emulation) and native playback of the PS2 movies (no IPU emulation) - separate tracks after the renderer. Judge frames for section A come from our own recompiled game on the PC (SotC GPU GS emulation of the game's own GS commands), not PCSX2.
