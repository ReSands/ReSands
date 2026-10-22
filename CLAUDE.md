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
Migrate part by part; emulated parts remain until replaced. Later, optional upgrades on top (60 fps, higher
resolution, upscalers, widescreen, assets from the other versions, ray tracing): see the roadmap below.

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

## Current status (22 Oct 2026 night; start here)

Older status blocks: `docs/history.md` (last one moved: "Moved from CLAUDE.md on 22 Oct 2026 evening").

- **Renderer step 1 (native static world, roadmap 0.11) done and in daily use (22 Oct).** `POP_NATIVE_WORLD=1`,
  now on by default in `tools\windows\play.ps1` (the only desktop shortcut, "PoP 60 fps"; `-NoNativeWorld`
  turns it off). The static draws skip their DMA, a worker draws them in Vulkan, a GS host hook (runtime patch
  38) writes picture + Z into the GS buffers where the frame's 3D starts; the GS draws everything else on top.
  In game: 857 elements converted, ~400 native draws per frame, ~60 fps, 3.4 ms per frame; world there at once
  and through camera changes after the ZMSK fix (bd89bd4); shadows over the world flicker less with the depth
  bias `POP_NATIVE_WORLD_ZBIAS=128` (the default; 0 = off). **Test it from a normal boot (title -> memory card):
  save-state boots never run ReadElement, so nothing is converted.** Details and numbers:
  `docs/native_renderer.md` "Native world in the game window".
- **Renderer step 2, characters: done and in daily use (22 Oct evening).** `POP_NATIVE_CHARACTERS=1`, on by default
  in `play.ps1` (`-NoNativeCharacters` turns it off): opaque CPU-posed characters join the world frame, their
  Display calls are skipped with the VU1 driver switches replayed. In game ~59.9 fps, user: fixed after the light-set
  fix (the Prince's white flash = a junk light vector; `DoSetSkinLightEnv`'s typed path decompiled). Point lights in
  character lighting: falloff measured from the VU1 program's own output (`tools/vu1port/lightprobe.cpp`; built on the
  PC, not yet seen in game). Details: `docs/native_renderer.md` "Native characters in the game window".
- **Static world audit + sword: done and checked in game (22 Oct night).** The sword (element 0x00B395B0) is a lit
  static mesh: pass 1 carries normals at VU 0x83 (pass 0 colours); the converter drew it with normals as colours
  (in game: reddish-brown, not missing; the "missing" sword was the offline frame). Census of 196 saves
  (`tools/staticcensus.py`) + world accounting: lit static passes stay on the GS; V4-32-position objects
  (mirror-room puzzle pieces) decode from memory; instanced pass 2 had a shared dedupe key; packetless refs end
  the chain. Undecoded calls 3,629 -> 1,044, all explained (lit pass 1 = step 2; pass-2 layers of V4-32 objects).
  User check (memory-card slot 04, the fountain after the mirror room): sword silver, mirror room right, ~60 fps.
  Details: `docs/native_renderer.md` "Static world audit". Memory-card slots: header +4 = play time (s), +0x10 =
  save-point key; copies in `game/analysis/memcard/`.
- **Lit statics, in progress (22 Oct late):** measured (docs/native_renderer.md "Lit statics measured"): pass 1 = character lighting (P4), pass 2 = env map from FUN_004B6D80 (also covers the mirrors' pass-2 layers), pass 4 = SetMirrorParameterForJsm (stays on the GS). Code: `packetWords()` factored out of `livePacketBatches`; next write `driverKind()` (table 0x700000B0, index pass + 5 x bit 1 of element +0x47), `litPacketBatches()` (batches split at MSCAL/MSCNT, strip flag = normal byte 4) and a display() branch before `it == g_elements.end()` that poses via `poseSkinned` (pass 1 lit, pass 2 env UVs); then judge offline.
- **NEXT (user agreed 22 Oct night): renderer step 2, the rest (roadmap 0.12), first the lit statics:** the 500
  pass-1 static calls now left to the GS (the sword, 40 instanced refs) drawn natively with the character lighting.
  Plan, element list, open question (which VU1 program/formula lights them: measure first) and judges (saves 192521,
  204040, 204108; in game memory-card slot 04): `docs/native_renderer.md` "Next: lit statics native". Then the
  pass-2 second layers of V4-32 objects (mirror reflections), then translucents + particles (below).
- **Open notes:** a full-frame flash right after the loading glyph when entering a scene: ours (the GS-only boot,
  `play.ps1 -NoNativeWorld`, does not flash); to capture the frames after a load. Trouser flicker: not seen in the
  last session (watch). Load-flash candidates in `docs/native_renderer.md`.
- **After the lit statics (0.12): translucent draws + particles native.** Measured 22 Oct over 108
  saves (`pop_state_frame --world` prints what is left per kind/list/blend): native 392 draws / 33k triangles per
  frame; the GS still draws ~136 mesh draws / 3.8k triangles, all translucent (blend 1/4/6 in lists +0x3C, +0x4C,
  +0x88, +0x98, +0xA8) except ~5 opaque in +0x4C, plus ~14 GFX particle and ~6 sprite draws (shadows, halos, shafts
  not replayed). Plan: open the world mode to these lists in the game's order together with the particles (particles
  alone would be drawn over by later GS draws), offline over the saves first. Qwen candidate after the water job: the
  whole character VU1 program (image 31829861, 2,048 lines) to C++, judged on the trace of save 192521
  (`game/analysis/vu1trace/`, `tools/windows/vu1trace.ps1`).
- **Water (P6) VU1 port, Qwen overnight from 22 Oct:** the 11 failing stages were split into 19 pieces of 9-20 pairs
  (+ the two clip checks); the night job `tools/windows/oct22.ps1` runs on the PC alone (judge built with MSVC,
  llama-server via the LlamaServe task), two passes of 6 rounds, `--chain` if all pass, then shuts the PC down.
  The user starts it from the PC desktop shortcut "Qwen overnight (then shuts down)"; log `game\jobs\qwen-night.log`,
  results `game\analysis\qwen\vu1_p6_*` (PASS files). Morning: read the log, re-run what failed (or hand-port it).
  Details: `docs/llm_jobs.md` "VU1 water program P6" (22 Oct note). Needed for 0.13.
- **Qwen candidates for later nights:** standalone tested decoders (`livePacketBatches`, `skinnedFromPacket`, particle
  quads; judged against the Python decoders on DMA captures) and the character VU1 program (above). User rule: Qwen
  work is overnight only; daytime is Claude + user. GPU skinning only if the profiler asks (or for motion vectors at 1.3).
- **Bugs written up, not fixed (user: later), `docs/saves_1021.md`:** camera X reversed in fixed-camera views
  (low priority); a door flickering with a black web (GS output too); **two freezes in combat**: crash 1 = the
  scheduler resuming at return address 0x172844 that has no entry (Ghidra split FUN_00172800's epilogue off at
  the delay slot of a `jal`; its save was lost), crash 2 = cause unknown, save kept
  (`crash_20261021_210951.state`). Both are needed for roadmap 0.1. Also (22 Oct): once, Alt+F4 did not close
  the game (the bias-0 session; force-killed over SSH; its log ends normally at 59.9 fps); cause unknown, the
  user does not want it chased now. Esc now quits (runtime patch 39; `PS2X_ESC_QUITS=0` turns it off).
- **Rules from the user (19 Oct):** the PC's TV is off unless the user is playing: ask before any job that boots
  the game (`nvidia-smi --query-gpu=display_active` must be Enabled). Survey other projects before fixing a
  narrowed-down problem (`docs/ps2_projects.md`); general learnings to `docs/lessons.md`; one dated job script
  per night; optimise in order of biggest win. Shut the PC down after overnight work. Playtesting: the user
  gathers saves; I work offline; milestone checks by the user.
- **Open items carried over:** audio bugs (a quiet/missing track in one fight, Prince-Farah in-engine dialogue out
  of sync, sound slows with the frame rate). Known limits: save-state boots have no sound and convert no
  meshes; runtime screenshots come out as the game in a corner.

## Release roadmap (user, 22 Oct 2026; replaces the 15/16 Oct numbering)

- **0.1 = playable start to finish** with emulated graphics and sound (the two combat freezes of 21 Oct fixed).
- **0.11 native static world -> 0.12 native characters + particles -> 0.13 native effects** (water P6, sprites P9, shadows, halos, light shafts, flashes) **-> 0.14 native HUD, menus, loading screens, movie display.**
- **0.2 = fully native graphics:** GS emulation off by default, judged frame by frame against the emulated GS output of the same build (target: visually indistinguishable; bit-exact pixels are unrealistic because VU1 and host floats differ, so residual differences must be measured and explained).
- **0.3 = native sound + native PS2 movie decoder:** sound from the engine's DARE/UbiSound calls (no IOP/SPU2 emulation); a **native decoder for the PS2 movie files** behind the game's own player (no IPU emulation; replaces the 13 Oct plan to adapt ps2-web-recomp's IPU).
- **0.4 = fully native,** incl. memory card, pad and rumble.
- **1.0 = cleaned up, human readable, GS reference toggle; publish.** Plays exactly like the original release, at the original 30 fps.
- **Vibration (user, 18 Oct; later):** (1) faithful rumble: the game's DualShock 2 motor commands (small on/off, large 0-255 via the pad library), ignored by the runtime today, passed through a portable library (SDL rumble: DualSense, Xbox, ...); (2) optional DualSense haptics and adaptive triggers mapped to game events, designed with Jellybit's DualSense Haptic Designer (MIT; https://github.com/Jellybit/DualSense-Haptic-Designer; outputs 4-channel WAV for the grip motors and raw trigger bytes; full haptics need USB, Bluetooth only experimental).
- **Pre-publish clean-up before 1.0 goes public** (user, 18 Oct): a final housekeeping pass over our own code (overrides, native renderer, tools, runtime patches; not the generated code) for human readability, possibly with a stronger model (e.g. Fable). Before it: write a short style guide (naming, file size, comment style, hook layout), with id Software's GPL Doom 3 source as the style reference (style, not code); freeze golden outputs (byte-identical `pop_state_frame` frames for every test save, nightly checks, movie/ADPCM/audio reference outputs). During it: behaviour-preserving changes only, small commits per subsystem, all golden checks after each; comments keep their evidence (move long histories to `docs/`, never drop the why); decide which `POP_*` debug switches stay. After it: a short playtest by the user (input, timing, window). Lighter tidying at each milestone before that (e.g. `pop_native_live.cpp`, ~1,800 lines).
- **Reference renderer toggle in 1.0** (user, 16 Oct): the emulated GS ships as an optional reference renderer, native Vulkan is the default. One key toggles between them; by default the emulated GS is fed only while shown (correct again within a frame or two after switching, since PoP re-uploads its textures each frame); a "comparison mode" setting keeps feeding it every frame for instant switching (costs the VU1 + GS work). Possibly a split-screen view. So Stage D step 16 means "GS emulation off by default", not removed.
- **1.1 = 60 fps setting** (30 stays available). Basis: the engine is variable-timestep (`TIM_Clock_Update` scales logic by real elapsed time) and the PCSX2 one-word patch (`POP_60FPS=1`) already gives 59.9 fps on the PC (16 Oct). Work: play-test for 30 fps assumptions (physics, animation, sound, cutscenes, sand rewind) and fix them.
- **1.2 = native higher resolution + crisp HUD.** Mostly render-target size once everything is native (2D/post-processing must scale too). DLSS/FSR/XeSS do not upscale the HUD: draw its sprites at output resolution, with upscaled or redrawn HUD textures.
- **1.3 = upscaler interface: DLSS (incl. frame generation), FSR, XeSS.** DLSS to 4K at 4:3; frame generation for 120+ fps on top of the native 60 (NVIDIA advises a base of ~60 fps; generated frames do not cut input latency). Needs motion vectors from the renderer (previous-frame matrices, skinned characters too).
- **1.4 = widescreen (Hor+**, 4:3 stays the default): wider projection, widened game culling (findings from the PCSX2 widescreen patch; reSOT's Xbox projection/culling hooks as a reference), HUD anchored, movies/loading screen pillarboxed; check cutscene framing and edge pop-in by playing through.
- **1.5 = assets from other versions** (PC, Xbox, GameCube; all owned by the user), substituted by asset key (same keys on all four platforms), in order: **textures, static meshes, skinned meshes, audio/sound effects/music/voice, movies** (Xbox Bink: highest bitrate, 5.1). Mesh swaps go through the native renderer; they need a decoder per platform format (Xbox/PC D3D vertex buffers, GameCube GX), and skinned meshes only swap if the bone layout matches; collision is separate data, so visual swaps do not change gameplay. Unverified: whether Xbox/PC meshes are more detailed (compare vertex counts per geometry key first; Xbox may be the better source). A per-category selector (texture source, mesh source, ...). The PS2 disc alone must still give the full game.
- **1.6 = Linux / macOS** (the portability rules below apply from now on, so this stays cheap).
- **1.7 = ray tracing:** shadows on select objects, ambient occlusion, reflections, diffuse GI, modern atmospheric effects, full path tracing. Each a separate toggle, drawn **before** the PS2 post-processing so the PS2 haze stays on top (see the RTX Remix notes in `docs/reference.md` for one route).
- **PAL and JP discs (some time):** each is a different ELF with different addresses; overrides hard-code retail US addresses, so new code should take addresses from one table per disc. PAL also brings 50 Hz and its language versions.
- **Publish at 1.0** (user, 15 Oct), as a base for the community; 1.1 onwards follow as updates. Publishing is limited to our own code (overrides, tools, runtime patches under their licences, PS2Recomp is GPL-3.0); users supply their own disc and build it. Never ship game data, generated code or prototype-derived material.

## Portability principle (user, 15 Oct 2026)

Windows is the target; Linux and macOS (Apple Silicon, via MoltenVK) should stay cheap to add later. Rules for new native code (renderer, sound, movies): Vulkan only, within what MoltenVK supports for the base path (no geometry shaders etc.); DLSS/ray tracing as optional extensions behind a small upscaler interface (MetalFX/FSR can plug in later); window, input, audio via portable libraries (SDL-class, FFmpeg); standard C++ without x86 intrinsics, or behind a wrapper with a portable fallback; no Windows-only APIs; shaders as ahead-of-time SPIR-V. Expected later effort (estimate): Linux days to ~2 weeks; macOS 1-3 weeks. DLSS frame generation on Linux is unverified. Do not give up a fast x86 path where it matters, but keep a portable one next to it.

## Working reference (distilled 17 Oct 2026; the details and their history are in `docs/history.md` and `docs/reference.md`)

- **Where things are written down.** `docs/history.md`: dated session logs (boot log 11-12 Oct, sound bring-up 13-14 Oct, mesh reader / Stage A-C findings 14-16 Oct, old status blocks and plans). `docs/reference.md`: background that is not a session log (PS2Recomp pipeline/PRs/status, prototypes and symbols, cross-version assets, texture formats, survey of other PS2 projects, RTX Remix, HD packs, renderer/DLSS notes, sources, answered open questions). Topic docs: `docs/native_renderer.md` (Stage B-D, live window), `docs/postfx.md`, `docs/movies.md`, `docs/llm_jobs.md`, `docs/vu1_scheduling.md` (build-time VU scheduling plan), `docs/upstream_ps2recomp.md` (our PS2Recomp bug list), `docs/nightly/`. Before re-deriving a fact, grep these.
- **Builds.** PC (the real target, pop-sotc runtime, `game\recomp-sotc`): `tools\windows\rebuild.ps1 -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc` with `-Fast` (override/runtime-only changes, ~30 s-few min) or `-SkipGhidra` (recompiler or runtime-header changes, full ~30-50 min); boot `tools\windows\boot.ps1 -Play -Out game\recomp-sotc [-Seconds N] [-Env 'A=1;B=2'] [-Shots <fields>]`. Runtime patches live in `tools/patches/pop-sotc/` (`git am` into `C:\tools\PS2Recomp-sotc`, then `git -C game\recomp-sotc\runtime-src checkout --detach` its HEAD; currently up to 39 (39 = Esc quits); remote `sotc` = TaylorNAlbarnaz/PS2Recomp). Compiled VU1/VU0 microcode: record with `PS2X_VU1_CAPTURE=game\recomp-sotc\vu1_capture` (both units since patch 31) or take the images from saves (`tools/vuimages.py <dir> <states>`), build with `rebuild.ps1 ... -Vu1 <that dir>` (all runner files recompile, ~30 min). Never put `vu0_*.bin` in a capture folder an older `vu1_recomp` reads (it expects 16 KB images). The play build links with /MAP (symbols for the profiler). Mac: the sotc base does not build there (x86 headers); the Mac uses the `pop` branch (`tools/rebuild.sh [--fast]`, `POP_BUILD=build-mac-play tools/boot.sh [secs] [log]`, exit 142 = time limit); Mac-only problems (medallion drift, curtain-load stall) are parked. Native tools: `cmake -S tools/native -B game/native-build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build game/native-build` (Mac: Homebrew molten-vk, vulkan-loader, vulkan-headers, glslang, SDL3).
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
