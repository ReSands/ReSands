# Release roadmap

Set by the user on 22 Oct 2026 (replaces the 15/16 Oct numbering); post-1.0 order revised 23 Oct; vibration and
the clean-up placed on 25 Oct (DualShock 2 rumble -> 0.4, DualSense -> 1.7, clean-up = part of 1.0). Moved out of
CLAUDE.md on 25 Oct 2026; CLAUDE.md keeps the one-line-per-version table.

## 0.x: playable, then native part by part

- **0.1 = playable start to finish** with emulated graphics and sound (the two combat freezes of 21 Oct fixed).
- **0.11 native static world -> 0.12 native characters, translucents, shadows + particles** (shadows moved here
  23 Oct: everything after them in the draw order waits for them) **-> 0.13 native effects** (water P6, sprites P9,
  halos, light shafts, flashes) **-> 0.14 native HUD, menus, loading screens, movie display.**
- **0.2 = fully native graphics:** GS emulation off by default, judged frame by frame against the emulated GS output
  of the same build (target: visually indistinguishable; bit-exact pixels are unrealistic because VU1 and host floats
  differ, so residual differences must be measured and explained).
- **0.3 = native sound + native PS2 movie decoder:** sound from the engine's DARE/UbiSound calls (no IOP/SPU2
  emulation); a **native decoder for the PS2 movie files** behind the game's own player (no IPU emulation; replaces
  the 13 Oct plan to adapt ps2-web-recomp's IPU). Reference: GT4Recomp's frame pacing + audio rate control
  (`docs/reference.md` "GT4Recomp").
- **0.4 = fully native,** incl. memory card, pad and rumble. References: GT4Recomp's crash-safe memory-card writer
  (cheap, could come earlier) and SDL3 pad layer (`docs/reference.md` "GT4Recomp").
  **Faithful rumble (user, 18 Oct; placed in 0.4 on 25 Oct):** the game's DualShock 2 motor commands (small on/off,
  large 0-255 via the pad library), ignored by the runtime today, passed through a portable library (SDL rumble:
  DualSense, Xbox, ...).

## 1.0: clean up and publish

- **1.0 = cleaned up, human readable, GS reference toggle; publish.** Plays exactly like the original release, at
  the original 30 fps.
- **Clean-up (user, 18 Oct; part of 1.0):** a final housekeeping pass over our own code (overrides, native renderer,
  tools, runtime patches; not the generated code) for human readability, possibly with a stronger model (e.g. Fable).
  Before it: write a short style guide (naming, file size, comment style, hook layout), with id Software's GPL Doom 3
  source as the style reference (style, not code; starting checklist: Table 2, the DOOM 3 BFG C++ conventions, in
  `lemiur/Game-Decompilation-Skill-For-Claude` `.claude/skills/decomp-code-rules/references/style-defaults.md`, read
  25 Oct 2026; no licence, so reference only, our own wording); freeze golden outputs (byte-identical `pop_state_frame` frames for
  every test save, nightly checks, movie/ADPCM/audio reference outputs). During it: behaviour-preserving changes only,
  small commits per subsystem, all golden checks after each; comments keep their evidence (move long histories to
  `docs/`, never drop the why); decide which `POP_*` debug switches stay. After it: a short playtest by the user
  (input, timing, window). Lighter tidying at each milestone before that (e.g. `pop_native_live.cpp`, ~1,800 lines).
- **Reference renderer toggle (user, 16 Oct):** the emulated GS ships as an optional reference renderer, native
  Vulkan is the default. One key toggles between them; by default the emulated GS is fed only while shown (correct
  again within a frame or two after switching, since PoP re-uploads its textures each frame); a "comparison mode"
  setting keeps feeding it every frame for instant switching (costs the VU1 + GS work). Possibly a split-screen view.
  So Stage D step 16 means "GS emulation off by default", not removed.
- **Publish at 1.0** (user, 15 Oct), as a base for the community; 1.1 onwards follow as updates. Publishing is
  limited to our own code (overrides, tools, runtime patches under their licences, PS2Recomp is GPL-3.0); users supply
  their own disc and build it. Never ship game data, generated code or prototype-derived material (GT4Recomp ships its
  translated code; we do not copy that, only its launcher/manifest ideas).

## 1.x: upgrades on top

- **1.1 = widescreen (Hor+**, 4:3 stays the default): wider projection, widened game culling (findings from the PCSX2
  widescreen patch; reSOT's Xbox projection/culling hooks as a reference), HUD anchored, movies/loading screen
  pillarboxed; check cutscene framing and edge pop-in by playing through. Plan: `docs/widescreen.md` (God Hand:
  Rearmed's method: constants computed from the aspect ratio via instruction hooks, HUD groups anchored, menus/loading
  screen latched to 4:3; the PCSX2 patch decoded against our ELF). Also GT4Recomp's any-aspect-ratio work
  (`docs/reference.md` "GT4Recomp").
- **1.2 = uncapped frame rate** (30 and 60 stay available as caps). Basis: the engine is variable-timestep
  (`TIM_Clock_Update` scales logic by real elapsed time) and the PCSX2 one-word patch (`POP_60FPS=1`) already gives
  59.9 fps on the PC (16 Oct). Work: play-test at several rates (60, 120, 144+) for fixed-rate assumptions (physics,
  animation, sound, cutscenes, sand rewind, timers that count frames) and fix them; frame pacing. Fallback if logic
  breaks above some rate: a fixed logic tick with interpolated presentation (Chronicle's `DECOUPLING_PLAN.md`,
  `docs/reference.md` "Renderer and DLSS notes") (estimate: needs only the native renderer's draw lists).
- **1.3 = native higher resolution + crisp HUD.** Mostly render-target size once everything is native
  (2D/post-processing must scale too). HUD sprites drawn at output resolution, with upscaled or redrawn HUD textures.
- **1.4 = assets from the PC and Xbox versions** (both owned by the user; GameCube dropped from the plan 23 Oct),
  substituted by asset key (same keys on all platforms), in order: **textures, static meshes, skinned meshes,
  audio/sound effects/music/voice, movies** (Xbox Bink: highest bitrate, 5.1). Mesh swaps go through the native
  renderer; they need a decoder for the Xbox/PC D3D vertex buffers, and skinned meshes only swap if the bone layout
  matches; collision is separate data, so visual swaps do not change gameplay. Unverified: whether Xbox/PC meshes are
  more detailed (compare vertex counts per geometry key first; Xbox may be the better source). A per-category selector
  (texture source, mesh source, ...). The PS2 disc alone must still give the full game.
- **1.5 = Linux / macOS** (the portability rules in CLAUDE.md apply from now on, so this stays cheap).
- **1.6 = ray tracing:** shadows on select objects, ambient occlusion, reflections, diffuse GI, modern atmospheric
  effects, full path tracing. Each a separate toggle, drawn **before** the PS2 post-processing so the PS2 haze stays
  on top (see the RTX Remix notes in `docs/reference.md` for one route).
- **1.7 = DualSense haptics and adaptive triggers** (user, 18 Oct; placed at 1.7 on 25 Oct), optional, mapped to game
  events, designed with Jellybit's DualSense Haptic Designer (MIT; https://github.com/Jellybit/DualSense-Haptic-Designer;
  outputs 4-channel WAV for the grip motors and raw trigger bytes; full haptics need USB, Bluetooth only experimental).
- **PAL and JP discs (some time):** each is a different ELF with different addresses; overrides hard-code retail US
  addresses, so new code should take addresses from one table per disc. PAL also brings 50 Hz and its language
  versions.

## Dropped

- **DLSS / FSR / XeSS (user, 23 Oct; was the old 1.3).** These temporal upscalers and frame generation only work
  inside a 3D renderer (jittered frames + depth + motion vectors, HUD drawn afterwards), not on finished 2D images.
  Higher resolution comes from rendering natively at it (1.3).
- **GameCube assets (23 Oct).**
