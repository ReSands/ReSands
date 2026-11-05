# Release roadmap

Set by the user on 22 Oct 2026 (replaces the 15/16 Oct numbering); post-1.0 order revised 23 Oct; vibration and
the clean-up placed on 25 Oct (DualShock 2 rumble -> 0.4, DualSense -> 1.7, clean-up = part of 1.0). Moved out of
CLAUDE.md on 25 Oct 2026; CLAUDE.md keeps the one-line-per-version table.
**Renumbered 5 Nov 2026 (user):** movie decoder split from 0.3 into 0.4, fully native -> 0.5, PAL/JP discs -> 0.6,
public playtest at 0.7 (first publish of our code, was 1.0), launcher 1.1, PC audio + movies 1.2, widescreen 1.1 ->
1.3, uncapped 1.2 -> 1.4, higher res 1.3 -> 1.5, modern graphical enhancements (new, after rac-rs) 1.6, PC
textures/meshes 1.7, Linux/macOS 1.5 -> 1.8, ray tracing 1.6 -> 3.0; Xbox assets deprioritised, DualSense haptics
off the roadmap ("Some time" below). 0.2 is now judged against the real PS2.

## 0.x: playable, then native part by part

- **0.1 = playable start to finish** with emulated graphics and sound (crash 1 fixed, closed 30 Oct after a clean fight session; crash 2, cause unknown and
  seen once, taken off the list 26 Oct and watched through the play-through: on a freeze, take a full dump).
  Borrowed items for 0.1 (from gow-native-pc, `docs/reference.md` "gow-native-pc"; placed 26 Oct): **MOVZ/MOVN write only the low 64 bits** (our recompiler copies 128; one line in
  `special_translator.cpp`; **written 29 Oct as sotc patch 43**, with MOVZ/MOVN cases in `tools/mmitest` that fail
  without it and pass with it; applied and built on the PC 29 Oct, reviewed 30 Oct, not booted yet): ride along with the next full `-SkipGhidra` rebuild after crash 2 is settled, before
  the play-through. **IOP `maxFreeMemory()`** never goes back up after frees: fix (`-Fast`) only if import stats
  (patch 11) show PoP calls QueryMaxFreeMemSize.
  Also with that rebuild (user, 27 Oct): **runtime patch 44 (written 29 Oct; numbered 43 before), small screenshots.**
  The shot is read with `rlReadScreenPixels` at the render size (LoadImageFromScreen multiplied by the DPI scale),
  cut to the game's draw rectangle and resized to 640x448 (RGBA PNG; `livecmp.py` reads it). Compiled on the PC
  with the play build's flags (single file); first real check = the next `boot.ps1 -Shots` after the rebuild. `PS2X_SCREENSHOT_DIR` shots
  (`ps2_runtime.cpp` ~2911, `LoadImageFromScreen` + `ExportImage`) are 9600x5400 on the 4K TV at 250% DPI with the
  game in a corner, and `livecmp.py` takes minutes per pair. Crop to the game's draw rectangle and resize to 640x448
  before export. Debug shots only; does not change play.
- **Between 0.1 and 0.2 (or when full rebuilds become the bottleneck): clang (llvm-mingw) build trial** (from gow-native-pc, `docs/reference.md` "gow-native-pc"; placed 26 Oct):
  ~15 min full builds there against 45-60 with MSVC, PGO on top; also the compiler 1.5 needs. First check our GS
  for raw `memcmp` of padded structs (with clang every primitive became its own batch there).
- **0.11 native static world -> 0.12 native characters, translucents, shadows + particles** (shadows moved here
  23 Oct: everything after them in the draw order waits for them) **-> 0.13 native effects** (water P6, sprites P9,
  halos, light shafts, flashes) **-> 0.14 native HUD, menus, loading screens, movie display.** (User, 30 Oct: the
  full-screen fades move to 0.14: they are drawn in the 2D pass (+0xB8) and need the same post-effects 2D layer as the
  HUD; 0.13's flashes are the after-effects, Pinch first.) 0.13 closed 30 Oct night by the user's in-motion check; `-NativeLate` is the default.
- **0.2 = fully native graphics:** GS emulation off by default, judged frame by frame against the original PS2 (the
  real console via the hardware reference below; user, 5 Nov; the emulated GS output of the same build stays the
  day-to-day check) (target: visually indistinguishable; bit-exact pixels are unrealistic because VU1 and host floats
  differ, so residual differences must be measured and explained). Plan and progress: `docs/gs_off.md`.
- **Hardware reference (idea, user 25 Oct 2026; tool for 0.2, not investigated yet; groundwork: `docs/gs_off.md` "Step 4c plan"):** a real PS2 as ground truth for
  "is this original?" and as the true reference for `tools/livecmp.py`. Not whole save states (emulator snapshots
  cannot be restored on a console). (a) GS replay (days, estimate): a PS2SDK homebrew player, loaded over ethernet
  with ps2link/ps2client, loads a frame's VRAM + GS registers (`tools/popstate.py`) and its GS packets
  (`PS2X_GS_TRACE`), draws them on the real GS and reads the framebuffer back (GS local->host transfer): exact
  digital pixels, before the display circuit (the display circuit merge is the game's own one-line blend, a formula from its registers, and is the
  same in 480i and 480p: `docs/native_renderer.md` "Display stage"; no capture card needed). Packets come from our recomp, so this checks the GS stage only. (b) DMA chain
  replay (1-2 weeks, estimate): the frame's chain (`tools/statechain.py`) + VU1 microcode + VU1 memory through the
  real VIF1/VU1, which also checks our VU1 recomp; the player must sit in RAM the chain does not use. Native frames
  are still scored with a tolerance (Vulkan vs GS rasterisation). Hardware: any region (homebrew bypasses the disc
  region lock, the GS is the same chip), a model that runs FreeMcBoot with ethernet. VU0/VU1 are inside the EE on
  every model; what varies is EE vs GS: fat SCPH-3000x/5000x have separate EE and GS chips (5000x + network adapter
  is the first choice); SCPH-7000x slims have built-in ethernet and came with separate chips or a merged EE+GS
  chip, with no compatibility differences reported (second choice); avoid SCPH-7500x and later (merged EE+GS, new
  I/O chipset, reportedly "Deckard", known game compatibility problems; no known GS rendering differences,
  unverified; late 9000x may need OpenTuna instead of FreeMcBoot). Readback stays on ethernet (user plans to buy a console, 26 Oct). First step: survey PS2 homebrew GS test tools (none found on 25 Oct beyond PCSX2's own
  dump replay), then try (a) on save 145139 (save point pop).
- **0.3 = native sound:** sound from the engine's DARE/UbiSound calls (no IOP/SPU2 emulation). Reference:
  GT4Recomp's frame pacing + audio rate control (`docs/reference.md` "GT4Recomp"). GT3-revisited (MIT, read 1 Nov):
  an SPU2 reverb + Gaussian/ADSR voice mixer as the native mixer's reference (`docs/native_renderer.md` sound step 3).
- **0.4 = native PS2 movie decoder** behind the game's own player (no IPU emulation). **Closed 5 Nov (user), with
  FFmpeg:** the decoder has worked since 16 Oct (FFmpeg `ipu` behind `FullMovie` in `tools/overrides/pop_movies.cpp`,
  all 11 disc + 39 vision movies, PCM sound straight to the audio output; user watched them on the PC 16 Oct).
  GT3-revisited's MIT decoder not adopted: FFmpeg stays anyway (PS2Recomp's runtime links it, Bink at 1.2); revisit
  only if the real PS2 shows a visible movie difference (`docs/movies.md` "Open question"). Movies also fine at 60 fps
  (user, 5 Nov: watched throughout the 60 fps play-testing).
- **0.5 = fully native,** incl. memory card, pad and rumble. References: GT4Recomp's crash-safe memory-card writer
  (cheap, could come earlier) and SDL3 pad layer (`docs/reference.md` "GT4Recomp").
  **Memory card (user, 5 Nov):** the native card reads and writes PCSX2's plain 8 MB `.ps2` card image (raw pages
  + ECC), so the same card works in PCSX2 and here; still native (the game's save bytes go straight into the
  file, no emulated card controller). PCSX2's folder memory cards are not supported.
  **Card storage (user, 5 Nov evening):** one `.ps2` image for all saves (no per-save files or readable save
  names: PCSX2 compatibility wins), kept in the user's folder, not beside the game: Windows
  `%USERPROFILE%\Saved Games\PoP Recomp\`, macOS `~/Library/Application Support/`, Linux `~/.local/share/`;
  path override allowed. Today's runtime writes loose files to `mc0/` beside the ELF (`Kernel/Stubs/MemoryCard.cpp`):
  the plan keeps those stubs and swaps their storage for a PS2 card file system (superblock, FAT, dirs, ECC; refs:
  PCSX2 GPL-3, ps2sdk mcman AFL), crash-safe write (tmp + flush + rename).
  **Progress (5 Nov evening):** patch 49 = card file system (`McImage.*`, self-test
  `ps2xRuntime/tests/mc_image_test.cpp`) under the existing libmc stubs; cards `Mcd001.ps2`/`Mcd002.ps2`; the old
  `mc0/` folder is imported once (the PC's 10 saves S00-S09 import byte-identical, checked offline); a card that has the
  magic but fails our checks is refused, never offered for formatting. **In-game check passed (user, 5 Nov):** normal
  boot imported all 30 files, a save loaded, a re-save rewrote only S08 (data + icon.sys; ECC clean, no clusters
  leaked; checked on a copy of the card). Open: PCSX2 opening our card, SetFileInfo not applied (logged), import stamps today's date.
  **Pad (user, 5 Nov evening; replaces the morning's XInput choice):** SDL3 (zlib) on every platform: Xbox,
  DualShock 4/DualSense, Switch Pro and generic pads via its controller database, rumble on all of them, so no
  Windows-only exception and no separate 1.8 pad layer. Replaces raylib's gamepad read in `Kernel/Stubs/Pad.cpp`.
  Reference: GT4Recomp's SDL3 layer (MIT).
  **Faithful rumble (user, 18 Oct; placed in 0.4 on 25 Oct, now 0.5):** the game's DualShock 2 motor commands (small
  on/off, large 0-255 via the pad library), ignored by the runtime today, passed to the pad's motors via SDL3 (large -> low-frequency, small -> high-frequency).
  **Pad + rumble progress (5 Nov night):** patch 50 = SDL3 gamepads in `ps2_host_input.cpp` (port N = N-th pad, hotplug,
  by position: south = Cross, triggers -> L2/R2 at 30/255; keyboard + mouse unchanged) and `scePadSetActDirect` -> motors
  (PoP's only motor path: `INO_PadLowVibSet`/`HighVibSet`/`PafSet`/`PifSet` 0x48AD00-0x48AEC0, byte 0 small, byte 1 large,
  aligned 0/1 in its pad state 7 at 0x48B000). Motors stop on state load and exit; the quick-save Select reads SDL too.
  PC play build OK 5 Nov. **Not yet checked in game** (user: pad buttons, sticks, rumble on hits/traps, hotplug).
- **0.6 = PAL and JP discs** (user, 5 Nov; was "some time"; the user is buying both discs). Each is a different
  ELF with different addresses; overrides hard-code retail US addresses. **From 5 Nov, new override code takes its
  addresses from one table per disc**; existing hard-coded addresses are converted at 0.6. PAL also brings 50 Hz and
  its language versions. Idea from rac-rs (`BOUNDARY_CAPS.txt` + `boundary_tests.rs`): a test counts the places our
  code touches raw game addresses outside the table, with a cap that may only go down. rac-rs checked "every
  PAL-to-USA memory address ... on all 19 planets" (its CHANGELOG v0.4.3).
- **0.7 = public playtest** (user, 5 Nov; first publish moved here from 1.0): announce, publish our own code on the
  1.0 terms below (no game data, users build from their own disc), and take bug reports from anyone who wants to
  play-test.
  **FFmpeg licence (5 Nov):** source-only release carries no FFmpeg obligations. Binaries, if any, bundle an **LGPL**
  FFmpeg build as separate replaceable libraries with the LGPL text and source pointer (Windows already fetches
  System233's `lgpl-amd64-shared` 7.1 build), **never Homebrew's GPL v3 build** (`--enable-gpl --enable-version3`).

## 1.0: clean up, performance

- **1.0 = cleaned up, human readable, GS reference toggle, performance optimised, simple 30/60 fps switch** (user,
  5 Nov; 30 = the original release's rate). Otherwise plays exactly like the original.
- **Performance (user, 5 Nov; all performance work waits until fully native, i.e. after 0.5).** Candidates borrowed
  from gow-native-pc (`docs/reference.md` "gow-native-pc"; placed 26 Oct under uncapped fps): **"register locals"
  code generation** (guest registers in C++ locals; -22 % game-thread time there; changes every generated function)
  and **frame overlap** (start frame N+1 before the GS FINISH of frame N; their ~3.5 ms serial gap per frame),
  designed on the native renderer's frame structure. Profile first. Not taken: their batched SPU2 mixer (0.3
  replaces SPU2 emulation) and VU1 clean operands (native ports replace the emulated VU1 programs); revisit only if
  a profile says otherwise. rac-rs: first start "pauses for about 8 seconds ... while shaders compile (once)": ship
  a pipeline cache.
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
- **Publish** (user, 15 Oct: at 1.0; moved to the 0.7 playtest on 5 Nov), as a base for the community; later
  versions follow as updates. Publishing is
  limited to our own code (overrides, tools, runtime patches under their licences, PS2Recomp is GPL-3.0); users supply
  their own disc and build it. Never ship game data, generated code or prototype-derived material (GT4Recomp ships its
  translated code; we do not copy that, only its launcher/manifest ideas).

## 1.x: upgrades on top

- **1.1 = launcher** (user, 5 Nov): a small settings launcher; at first only 30/60 fps and GS vs Vulkan rendering,
  then it grows with each later version (widescreen, resolution, enhancements, asset sources). Reference: rac-rs's
  separate launcher repo (`AsobiCodes/rac-rs-launcher`, not read yet: presets, profiles, backup before update).
- **1.2 = audio and pre-rendered movies from the PC version** (user, 5 Nov; was part of 1.4), substituted by asset
  key, optional (the PS2 disc alone stays complete). PC movies are probably Bink (unverified; FFmpeg's Bink decoder is
  LGPL and portable). Xbox (highest movie bitrate, 5.1) deprioritised.
- **1.3 = widescreen (Hor+**, 4:3 stays the default): wider projection, widened game culling (findings from the PCSX2
  widescreen patch; reSOT's Xbox projection/culling hooks as a reference), HUD anchored, movies/loading screen
  pillarboxed; check cutscene framing and edge pop-in by playing through. Plan: `docs/widescreen.md` (God Hand:
  Rearmed's method: constants computed from the aspect ratio via instruction hooks, HUD groups anchored, menus/loading
  screen latched to 4:3; the PCSX2 patch decoded against our ELF). Also GT4Recomp's any-aspect-ratio work
  (`docs/reference.md` "GT4Recomp"). ICO-pc (2 Nov, `docs/reference.md` "ico-pc"): VU1 region test drops edge
  triangles on a wide frame (its `VU1_PROGRAMS.md` "The region rule"; check whether PoP's VU1 programs do the same);
  its gate-b test (centre columns at every aspect byte-equal to 4:3) as our widescreen check.
- **1.4 = uncapped frame rate** (30 and 60 stay available as caps). Basis: the engine is variable-timestep
  (`TIM_Clock_Update` scales logic by real elapsed time) and the PCSX2 one-word patch (`POP_60FPS=1`) already gives
  59.9 fps on the PC (16 Oct). Work: play-test at several rates (60, 120, 144+) for fixed-rate assumptions (physics,
  animation, sound, cutscenes, sand rewind, timers that count frames) and fix them; frame pacing. Saves to re-check
  above 60 (user, 4 Nov; `docs/native_bugs.md` "Bridges"): rope swing `play_20261103_214828` (R1 held; looked the same
  at 30 and 60, but its AccumulateForces pull scales with dt^3 per step) and the rope bridges `play_20261103_213556` /
  `play_20261104_191745` (already fixed-step with extrapolation; check smoothness and walking back onto the ledge). Fallback if logic
  breaks above some rate: a fixed logic tick with interpolated presentation (Chronicle's `DECOUPLING_PLAN.md`,
  `docs/reference.md` "Renderer and DLSS notes") (estimate: needs only the native renderer's draw lists). Worked
  design for that fallback: ICO-pc's `RENDER.md` "Camera blending" / "Interpolation and presentation" (draws paired
  by key/type/list/ordinal, bone slerp with turn memory, unmoved scenery re-based on the blended camera, glow per tick).
  Performance candidates placed here 26 Oct (register locals, frame overlap) moved to 1.0 on 5 Nov.
- **1.5 = native higher resolution + crisp HUD.** Mostly render-target size once everything is native
  (2D/post-processing must scale too). HUD sprites drawn at output resolution, with upscaled or redrawn HUD textures.
- **1.6 = modern graphical enhancements** (user, 5 Nov, after rac-rs; `docs/reference.md` "rac-rs"). Rasterised,
  no new assets, each its own toggle, **off by default** (the PS2 look stays the default) and drawn **before** the
  PS2 post-processing so the haze stays on top. In order:
  - **1.61 ambient occlusion (GTAO):** contact shading from the native renderer's depth (normals rebuilt from depth).
  - **1.62 improved dynamic shadows:** scene-aware shadow maps in place of the crude overhead shadows (0.12), baked
    environment shadows kept. The game probably has no usable sun direction: likely set by hand per level
    (estimate). rac-rs: casters must be chosen per shadow area; texture-pack lamp posts cast "only a floating head".
  - **1.63 fog and atmospheric scattering:** height fog, better distance fog, light shafts (the game already draws
    its own light shafts, 0.13: decide enhance vs add when we get there).
  - **1.64 reflective water and procedural waves:** water is already its own draw path (P6, 0.13).
  - **1.65 global illumination:** screen-space or light probes (the ray-traced version is 3.0).
  - **1.66 dynamic emissive lighting:** flickering light from fire and other glowing effects; candidates are the
    particle/sprite draws (P9, 0.13).
  - **1.67 volumetric smoke** generated around existing emitters.
  - **1.68 procedural clouds and stars:** the sky draws are not identified yet.
  Watch-outs: the lighting is largely baked into vertex colours, so AO/GI on top can darken corners twice (tune
  each against the original). From rac-rs (CHANGELOG): a Classic/Enhanced umbrella with Off/Low/Medium/High rows and
  presets, a measured speed-impact meter per row, a split-screen original vs enhanced view (we plan one for the GS
  toggle anyway), effects that the driver refuses switch off instead of crashing, and state leaks when switching
  back to Classic (fog stayed faded; water effects stayed off): test every toggle round trip.
- **1.7 = more assets from the PC version: textures, static meshes, skinned meshes** (optional), substituted by
  asset key (same keys on all platforms). Was 1.4 "PC and Xbox assets"; split 5 Nov (audio + movies -> 1.2), Xbox
  deprioritised (user: PC copies are cheap and easy to buy; encourage people to buy the game). Mesh swaps go through the native
  renderer; they need a decoder for the Xbox/PC D3D vertex buffers, and skinned meshes only swap if the bone layout
  matches; collision is separate data, so visual swaps do not change gameplay. Unverified: whether Xbox/PC meshes are
  more detailed (compare vertex counts per geometry key first; Xbox may be the better source). A per-category selector
  (texture source, mesh source, ...). The PS2 disc alone must still give the full game.
- **1.8 = Linux / macOS** (was 1.5; the portability rules in CLAUDE.md apply from now on, so this stays cheap;
  the pad already works there via SDL3 from 0.5).

## 2.0: byte-matching decomp for historical study (user 25 Oct 2026; under way since 27 Oct, DARE first)

- **2.0 = a byte-matching decompilation alongside the recomp,** as a side quest for historical study. It does not
  clash with the recomp: a separate source tree (own folder or private repo) sharing the Ghidra project, the
  prototype's names (10,618 functions) and struct layouts. Knowledge flows both ways, code only later: matched source
  is PS2-shaped (32-bit pointers, fixed memory layout, VU0 inline assembly, scratchpad), so replacing a recompiled
  function with it needs the same work as an override.
- **First step when picked up: a feasibility check, no PC time.** Find the exact Metrowerks CodeWarrior PS2 version
  and flags (the ELF's `.mwcats`), find a usable copy of that compiler (e.g. whether decomp.me hosts it), then try to
  byte-match two or three small functions from one module. No suitable compiler -> byte matching is out; a
  non-matching readable decomp is the fallback.
- **Scale (estimate):** C++, ~4 MB of code; a full match is years, one subsystem (camera, Prince movement) weeks.
  Done a subsystem at a time, single-function sessions rather than many parallel workers.
- **Who does what (25 Oct 2026, estimate).** The per-function grind suits Qwen at night: the byte diff is a strict
  judge, as in the VU1 ports (`docs/llm_jobs.md`). Claude (day sessions) does what one-function-at-a-time cannot: the
  compiler and project setup (split into translation units, link order, build + diff tooling), each subsystem's shared
  headers (class layouts, vtable order, inlines; a wrong header breaks every function using it), and the morning
  review of stuck functions. Qwen works a subsystem's queue easiest first, may not edit shared headers (it reports
  "needs header change"), and a change is kept only if it matches and no matched function regresses.
  decomp-permuter (no LLM) takes the near-misses (compiler-quirk walls: register allocation, scheduling). Most
  functions by count could be Qwen's; most of the thinking is Claude's. Nights are taken by the renderer until 1.x,
  so only the compiler check (no PC time) is worth doing early.
- **Method reference:** `lemiur/Game-Decompilation-Skill-For-Claude` (Claude Code skills for matching decomps:
  matcher, wall-identifier, helper-recovery, permute, decomp-code-rules; read 25 Oct 2026; no licence, so reference
  only, not installed).
- **Feasibility check, part 1 (26 Oct 2026, Mac only).** The retail ELF's `.comment` (and the prototype's) reads
  `MW MIPS C Compiler (2.4.1.01)` + `PlayStation2`; retail has no `.mwcats` (only the prototype ELFs do). decomp.me
  hosts CodeWarrior PS2 compilers (`github.com/decompme/compilers`, `values.yaml`): the ones whose `mwccps2.exe`
  carries exactly that string are 2.4-001213, 3.0-011126 (= 3.0b22-011126), 3.0.1-020123 (= 3.0b22-020123),
  3.0.3-020716 (= 3.0b22-020716) and 3.0b22-020926: **five distinct binaries** (the 2003 builds, 3.0b38 onwards,
  say 3.0.0, so they are out). Copies under `game/decomp/compilers/` (git-ignored; never commit). They are Win32
  exes: no Wine on the Mac, so test compiles run on the PC (CPU only, no TV). Not via decomp.me's site: a scratch
  is public. Test functions picked (retail addr, size): `GRO_Struct_Init__FP16GRO_tdst_Struct_i` 0x446280/44 (C
  style), `AI_EvalFunc_OBJ_HierarchyGet_C` 0x1BD200/44 (has scheduler `nop`s), `SynchroTheme__13CThemeManagerFv`
  0x193400/72 (list loop, `nop` pairs after the `jal`), `SetPos__4TPCMFi` 0x16F500/72 (virtual call); first guesses
  in `game/decomp/try/`. Side find: the prototype's `POP1.ELF` (the bonus classic Prince of Persia 1) has full
  `.debug`/`.line` (DWARF 1) sections with source paths (`D:\dev\POP1\Dare\engine\common\src\sndCTheme.cpp` etc.,
  172 source files): it links the **DARE sound engine** the main game uses (`CThemeManager`, `SndManager`), so its
  debug info gives real struct layouts and file splits for that subsystem (also for 0.3 native sound). Overlap (26 Oct, symbol names only): POP1.ELF has 2,310 functions;
  986 of their names are among the 4,750 named retail SoT functions (612 the same size, 374 a different size, so
  a different DARE version in places); 518 of its ~990 sound-like names (snd/theme/bank/stream/pcm...) are in SoT.
  Source folders: mostly `Dare\engine\common\src` (+ `ToolArch\Managers`, `Sources`, `Renderers\PS2`) and
  `Dare\engine\ps2\src\ee`. The DWARF is version 1 (CodeWarrior), which common tools barely read: needs a small
  parser of our own. Not parsed yet.
- **Feasibility check, part 2 (26 Oct 2026, PC, CPU only): byte matching works.** All four test functions compile
  **byte-identical** to retail (relocated fields masked) with **CodeWarrior 3.0.3-020716 (= 3.0b22-020716) at
  `-O4,p`**, the only candidate matching all four (`SynchroTheme` matches on no other build; GRO/AI also on 3.0 and
  3.0.1; 3.0b22-020926 matches only GRO). 2.4-001213 asks for a FlexLM `license.dat`, so untested. What it took:
  - `GRO_Struct_Init`: first guess. `AI_EvalFunc_OBJ_HierarchyGet_C` (a C function): `void *r = 0; if (...) r = ...;
    return r;` (early returns give other registers). `SynchroTheme`: `CThemeChannel *c = n->channel; n = n->next;
    c->SynchroTheme();` (next loaded before the call). `SetPos__4TPCMFi`: virtual slot 7 (MW vtables: entries from
    +8), and `SND_fn_rMulRealRealSnd` taking **unsigned** arguments (the conversion moves the load). The compiler's
    odd `nop` pairs come out by themselves.
  - So the usual matching-decomp wall (register allocation and scheduling depend on exact types and source shape) is
    there, but at a normal height: three of four needed one to three tries. Small sample, easy functions; the real
    question is the large Jade/AI functions and the class headers.
  - Tooling: `tools/decomp/objcmp.py <elf> <targets.txt> <out dir>` (compare every object in `out/*/` with the
    retail bytes; targets = `<symbol substring> <addr hex> <size>`); the test sources, `build.bat` (each compiler
    x flag set, on the PC: `game\decomp\try\build.bat`) and the compilers stay under `game/decomp/` (git-ignored:
    reconstructed Ubisoft code and Metrowerks binaries). The PC's SSH shell is cmd.exe, not PowerShell.
  - Next steps if the user goes ahead: a whole small translation unit (one `.cpp` from POP1.ELF's file list, e.g. a
    DARE file, with its DWARF types), splitting the ELF into units (splat), a linker check with `mwldps2`; then
    decide the scope (one subsystem first).
- **Started 27-28 Oct 2026 with DARE (byte matching, user's choice): `docs/decomp_dare.md`.**
- **Rules to settle first:** the leaked Jade source stays off limits; Sony library code in the ELF stays assembly or
  is decompiled from our own binary only, never from memory; a decomp reconstructs Ubisoft's code, so it stays
  private unless the user decides otherwise (the ground rules share only our own code).
- **Legal position, compared with OpenGOAL (26 Oct 2026; general background, not legal advice).** The rule above is
  a cautious project choice about *publishing*, not a finding that the work is illegal; personal use from our own
  disc and GOG copy is the same pattern every fan decomp relies on.
  - OpenGOAL (Jak 1-3, `github.com/open-goal/jak-project`, ISC) answers "Is this project legal?" with only "Yes" in
    its FAQ (`opengoal.dev/docs/faq/`), with no reasoning; it ships no assets, needs the user's own PS2 disc, shares
    no ISOs. No court has ruled on any fan decomp (OpenGOAL, SM64, OoT), so a SoT decomp would be in the same untested
    position, not a worse one.
  - Case law (US): disassembly to build compatible software can be fair use (Sega v. Accolade 1992, Sony v.
    Connectix 2000), but those cover the copying done during analysis, not publishing source that reproduces the
    program. EU: decompilation allowed for interoperability only (narrower). Code is copyrighted, and decompiled source
    is arguably a copy/derivative; byte matching aims to reproduce the original exactly, which arguably strengthens
    that point. OpenGOAL may be on slightly firmer ground as a port (GOAL source -> x86-64 via its own compiler, not a
    rebuild of the original binary; our reading, not their claim).
  - These projects survive through tolerance (no assets, own disc required, nothing sold), not permission; the risk
    is a takedown notice from the publisher (here Ubisoft). If the user wants to publish one day: read the published
    decomps' notices first, possibly ask a lawyer. Until then the rule stays. The leaked-code rule is separate
    (provenance) and applies either way.

## 3.0: ray tracing

- **3.0 = ray tracing** (user, 5 Nov; was 1.6): shadows on select objects, ambient occlusion, reflections, diffuse GI, modern atmospheric
  effects, full path tracing. Each a separate toggle, drawn **before** the PS2 post-processing so the PS2 haze stays
  on top (see the RTX Remix notes in `docs/reference.md` for one route).

## Some time (not on the roadmap)

- **DualSense haptics and adaptive triggers** (user, 18 Oct; was 1.7, taken off the roadmap 5 Nov: less interest), optional, mapped to game
  events, designed with Jellybit's DualSense Haptic Designer (MIT; https://github.com/Jellybit/DualSense-Haptic-Designer;
  outputs 4-channel WAV for the grip motors and raw trigger bytes; full haptics need USB, Bluetooth only experimental).
- **DLSS / FSR / XeSS** (dropped 23 Oct, was the old 1.3; back as "some time" 5 Nov, user: not urgent, low
  interest). Dropped because these temporal upscalers and frame generation only work inside a 3D renderer (jittered
  frames + depth + motion vectors, HUD drawn afterwards), not on finished 2D images; after 0.2 our renderer is native
  with depth, so only motion vectors are missing. rac-rs ships FSR + DLSS 4.5 this way (`docs/reference.md`
  "rac-rs"). Higher resolution itself comes from rendering natively at it (1.5).

## Dropped

- **GameCube assets (23 Oct).**
