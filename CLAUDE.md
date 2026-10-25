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

## Current status (23 Oct 2026 evening; start here)

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
- **Lit statics native: done and checked in game (22 Oct ~22:00; user: sword right, mirrors no longer flicker after the object-space fix 3937445).**
  Pass 1 = character lighting (P4), pass 2 = env map from FUN_004B6D80's matrix, driver from the game's table (0x700000B0);
  elements and instanced refs; `POP_NATIVE_NOLIT=1` = old behaviour. Offline: 204040 13.04 -> 11.28, 204108 5.02 -> 4.23;
  the mirror room's gold frames, rings and faceted mirrors now drawn; the sword silver-blue with env highlights. In game:
  memory-card slot 04 checked. Small leftovers (save 214633): centre faceted mirror greyer, one hexagon facet darker. Details:
  `docs/native_renderer.md` "Lit statics measured".
- **NEXT (plan changed 23 Oct evening, user agreed): native shadows, then the lists after them + particles.** Why:
  the injected picture replaces pixels, so only a prefix of the game's draw order can be native, and the shadows sit
  between the lightmaps (+0x5C) and +0x88/+0x98/+0xA8, where nearly all GFX particles are (`docs/native_renderer.md`
  "Translucents before the shadows"). Done first: `POP_NATIVE_TRANSLUCENT=1` (lists before the shadows, any blend;
  now **on by default in `play.ps1`** (`-NoNativeTranslucent` = off; 23 Oct night). The cloud "bug" of save 21_202624 was
  the offline judge: `pop_native_view --frame` drew PNF7 frames opaque-first, so the clouds landed on the opaque horizon
  band the game draws after them; the game window was always in game order. Viewer fixed; A/B over 201 saves 19.24 ->
  18.23 (108 better, 14 worse = letterboxed cutscenes). **User check in game (24 Oct): clouds fine**; it showed distant
  buildings as flat grey: fog zones are (max, min, a, b) and the VU1 drivers clamp F to min..max (min 102 = at most
  60% fog); we clamped to 0..255. Fixed (PNF8, fog min/max per draw), offline A/B 191 saves 28 better / 3 worse
  (<= 0.5); **user: palace fix looks good (24 Oct)**.
  **Native shadows: done and checked in game (24 Oct afternoon; user: shadows right on the floors, lever flicker
  gone).** `POP_NATIVE_SHADOWS=1`, **on by default in `play.ps1`** (`-NoNativeShadows` = off). The receiver loop of the shadow VU1 program (0x1D98) decoded and checked against the
  interpreter (alpha within 1 on 32,940 vertices); the shadow buffer is the alpha of the displayed frame. A hook on
  FUN_004C3540 rasterises the casters into a 256x256 alpha image per projector and redraws the receivers from the
  frame's own records (same mesh + model matrix as the base, so the GEQUAL test sees its exact depth). In world mode a
  projector goes native only if all its receivers' bases are native (then FUN_004C3540 is skipped, its lists emptied).
  Save 192521: the Prince's shadow in place and shape, floor x 0.67 vs the game's ~0.74 (game edge a bit softer).
  A/B over 204 saves: only 3 hold filled shadow lists (the game empties them after drawing), all 3 better, none worse;
  so the judge was the game. Details: `docs/native_renderer.md` "Native shadows".
  **Lists after the shadows: built, offline-judged, in-game check pending (24 Oct afternoon).** `POP_NATIVE_LATE=1`,
  **on by default in `play.ps1`** (`-NoNativeLate` = off): +0x88/+0xA8 and the GFX particles go native while no
  earlier draw of the frame went to the GS (otherwise the order would break); +0x98 (alpha-only) stays on the GS.
  Offline 203 saves 18.07 -> 17.58 (54 better, 7 worse <= 0.30). Main limit: sprites (VU1 places them along a curve;
  0.13). Details: `docs/native_renderer.md` "Lists after the shadows".
  **Order for the next session:** (1) done; (2) shadows done (in game, ~60 fps, every projector native in the
  first session); left out so far: mirror projectors,
  blob receivers (+0x20 / FUN_004C2E00), CJIdxMesh entries, FUN_004C2C40's fog fade; (3) open the
  world to +0x88/+0x98/+0xA8 in the game's order (watch the frame alpha: EraseAlphaBuffer runs before +0x98) with
  the GFX particles. Each step offline first, then a user check in game.
- **HANDOFF (24 Oct ~19:00; start here after the context clear).** In-game session 18:41-18:53 on the late-lists
  build (042b0b1, before sprites): clouds and torch smoke fine; fights at 47-56 fps for a minute or more (log
  `game/analysis/saves1024b/play_20261024_184116.log`), save of a slow fight `saves1024b/play_20261024_184926.state`;
  **then a hang at the end of a fight** (fps 0, audio stuttering; one thread 15484 at 100% CPU, the others idle).
  Things to do, in order:
  1. **Hang fix: runtime patches 41 + 42, built on the PC 24 Oct ~19:30, user check pending.** Patch 41 alone
     (built 19:10) froze the game during the rewind power: generated code can return to the scheduler with the
     same pc (Ghidra split the 0x4A6180 loop into pieces that call each other, pc 0x4A61D4 each iteration), 41
     took that for a stub return and jumped to $ra = 0xB35A70 (log: `[guest-branch:missing-target] ... EE
     scheduler ... pc=0xb35a70`; dump `game/analysis/hang1024c/hang.dmp`). Patch 42: only functions replaced
     after registration (overrides, stubs) get pc = $ra; the first 20 uses log `[ee-scheduler] replaced function
     .. left pc`. Look for those lines in the next play log. First hang: Thread 15484 = the game thread, spinning in `EeScheduler::run` -> `processPendingEvents`
     with no guest frames; the main guest thread's context: pc 0x4C7B40 (CJMeshElementRef::Display), ra 0x4B9C88
     (CElementList::Render), a0 0x1931890, a1 0x37, a2 0. Mechanism: our draw hooks skip a call by returning
     without touching pc ("like a stub"); `dispatchGuestBranch` turns that into the fallthrough, but when the
     checkpoint fires at the JAL itself, the call is resumed from `EeScheduler::run`, which dispatched pc 0x4C7B40
     again forever. Patch 41 (`tools/patches/pop-sotc/41-scheduler-stub-return-to-ra.patch`): after a normal
     return with pc unchanged and no unwind/transfer, pc = $ra. Upstream has the same gap
     (`docs/upstream_ps2recomp.md` item 14). Deploy: `git am` it into `C:\tools\PS2Recomp-sotc`, check out its HEAD
     in `game\recomp-sotc\runtime-src`, `-Fast` build (EeScheduler.cpp only). Unrelated to the 21 Oct crashes.
     Method (map-independent): the stack's return addresses matched against the new exe by byte pattern (runtime
     code moved by a constant 0x3910), the guest contexts found by scanning the dump for R5900Context blocks
     with gp 0x6749f0 (Python `minidump`, scripts in the session scratchpad).
  2. **White enemies (earlier session, saves 18330x-183541):** enemies drawn native with another texture (Prince,
     Farah fine; a fresh boot showed them right). Cause believed: `popTextureKey`'s table (filled only by
     PS2_Texture_Load) going stale mid-session; fix 88acfc5 reads TEX_List at draw time and logs
     `[pop:native] texture index N: TEX_List key .., PS2_Texture_Load registered ..` when they differ. **Not yet
     confirmed:** look for those lines in the next play log; if enemies still go white, the theory is wrong.
     One enemy's calves were invisible (same cause?).
  3. **Native sprites (2469ab0, 367e9e2):** in the PC build since 18:54, user check pending (`POP_NATIVE_SPRITES=0`
     = off). Docs: `docs/native_renderer.md` "Native sprites on the VU1 interpreter".
  4. **Save-point glow pops brighter when the camera turns to it** (saves 183302 left / 183305 facing): the light
     column is native in both; the difference is the halo / after-effects pass (FUN_004ABFC0, GS). Asked the user for
     an A/B at a save point with `play.ps1 -NoNativeLate` and `-NoNativeWorld` (pops in both = original behaviour;
     smooth without the native world = its depth fools the halo's visibility test).
  5. **Fight fps:** profile save 184926 (`tools/windows/profile-states.ps1`, or ask for a `-Profile` session);
     the late lists add work per frame (worker 4.4 ms/frame in the earlier log).
- **HANDOFF 2 (24 Oct ~20:45; start here).** Built on the PC and in the game now (user check pending): the black
  one-frame flashes fix (N5, 30ef5d5: kept meshes dropped between generations) and the **crash 1 fix** (3b80b66:
  entry at 0x172844; `docs/saves_1021.md`). Patches 41 + 42 checked in play (no hang since). Next, in order:
  (1) triage of the 20:08-20:24 saves: **done 24 Oct evening** (`docs/native_bugs.md` "Triage of the 20:08-20:24
  reports"): N8/N9/N11 = live-only stale per-address caches (g_fromPacket, g_litBatches, g_skinned, g_skipped not
  reset by ReadElement; fix planned); N10 + save-point glow = the halo's occlusion test reads our Z (A/B with
  ZBIAS=0); N12/N6 = a shadow falls back to the GS, then everything after it, and the sprite interpreter still runs
  (fix planned: purity check first); (2) compress `.glitch` pictures + delete triaged captures (same doc, "Storage");
  (3) native conversion after a save-state load. The user plays meanwhile; ask before a rebuild (exe locked).
- **Native bug list + tooling plan (24 Oct ~20:00): `docs/native_bugs.md`.** N1 slowed enemies not sandy brown
  (highest), N2 fog blips on camera turns, N3 stairs flash, N4 camera "teleport" in combat. User: fix at milestones,
  not now. **Next: build the glitch capture key, then native conversion after a save-state load** (so I can boot
  saves with the live path). White-enemy fix 88acfc5 confirmed working (log caught index 163's stale key).
- **Load flash: FIXED, checked in game (23 Oct ~19:15, 85587ef; user: 2 saves + menu, no flash).** Cause (GS traces,
  runtime patch 40): frame 1 after a load has no clear sprite, so FRAME at our marker was still the loading screen's
  target = the buffer on screen, and the stale-camera picture was uploaded onto the display. Fix: never inject into
  the displayed buffer (DISPFB1); use the other of the pair. In the log it fires once per load (+5 frames during the
  boot logos), `[pop:world] frame N: FRAME fbp .. is on screen`. Debug aids kept: `POP_DISPLOG=1`,
  `POP_DISPLOG_TRACE=<dir>`, `POP_NATIVE_WORLD_DUMP=<dir>`. Full hunt: `docs/history.md` "Load flash (23 Oct)".
- **Open notes:** trouser flicker not seen lately (watch). Torch save `game/analysis/saves1023/st/` (point lights in
  sets 0/1): offline native vs game 4.73, the Prince matches; point lights near a fire still to be looked at in game.
  The -Fast rebuild once recompiled the whole runtime (~2,200 objects): find why.
- **P9 sprites + P6 water VU1 ports: DONE (25 Oct), both pass the whole-program judge** (`port.py --chain p9`:
  598 whole runs; `--chain p6`: 600). Night 24-25 Oct: Qwen 32 of 37 P9 stages + `p6_a_ck_cull`; the rest by hand
  (`known_good/`: p9_c_b, d_save, d_loop_b, f_a, f_b, p6_a_ck_head). Two P6 pieces were unpassable: the case snapshot
  took clip flags before an in-flight CLIP landed (fixed in `replay.cpp`; re-judged, Qwen's round-1/2 attempts pass;
  the passed `b_ck_head` was a cheat, replaced). Speed on the Mac: P9 ~27 us per run vs ~5 ms interpreted (~190x),
  P6 ~3 us vs ~1.8 ms. **Next (daytime): P9 into the game in place of `spriteRun`** (chain of
  `game/analysis/qwen/vu1_p9_*/best.cpp` + `known_good/p9_*`; fall back to the interpreter when the chain stops at a
  pc with no stage: branches never seen in the traces), then measure fight fps; P6 waits for the native water (0.13).
  Details: `docs/llm_jobs.md` "Night of 24-25 Oct results".
- **Character VU1 program P4 for Qwen: set up 25 Oct; tonight `tools/windows/oct25.ps1`** (PC desktop shortcut
  "Qwen overnight (until Stop Qwen)"; cancel with "Stop Qwen"). 79 stages from the new `tools/vu1port/cutscan.py`
  (borders only where nothing unsafe is in flight), cases from three play traces (`tools/vu1port/cases_p4.sh`),
  `known_good/p4_0000` as the example. Ends by itself when all pass (then `--chain p4`) or after two passes with no
  new pass, then shuts down. **Morning:** `game\jobs\qwen-night.log`, copy `game\analysis\qwen\vu1_p4_*` to the Mac,
  hand-port what failed, `--chain p4`. Details: `docs/llm_jobs.md` "VU1 character program P4".
- **Before the next non-VU1 Qwen job:** build in the ReAgent learnings (bounded evidence requests, a bulk
  Ghidra JSON export with `jalr`/constant-address refs, save-state global values, recorded cases as judge):
  `docs/llm_jobs.md` "ReAgent + ghidra-bridge".
- **Qwen candidates for later nights:** standalone tested decoders (`livePacketBatches`, `skinnedFromPacket`, particle
  quads; judged against the Python decoders on DMA captures). User rule: Qwen
  work is overnight only; daytime is Claude + user. GPU skinning only if the profiler asks.
- **Bugs written up, not fixed (user: later), `docs/saves_1021.md`:** camera X reversed in fixed-camera views
  (low priority); a door flickering with a black web (GS output too); **two freezes in combat**: crash 1 = the
  scheduler resuming at return address 0x172844 that has no entry (Ghidra split FUN_00172800's epilogue off at
  the delay slot of a `jal`; seen again 24 Oct 20:24; **fix built 24 Oct, 3b80b66: entry registered by the
  override, in-game check pending**), crash 2 = cause unknown, save kept
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

## Release roadmap (user, 22 Oct 2026; replaces the 15/16 Oct numbering; post-1.0 order revised 23 Oct)

- **0.1 = playable start to finish** with emulated graphics and sound (the two combat freezes of 21 Oct fixed).
- **0.11 native static world -> 0.12 native characters, translucents, shadows + particles** (shadows moved here 23 Oct: everything after them in the draw order waits for them) **-> 0.13 native effects** (water P6, sprites P9, halos, light shafts, flashes) **-> 0.14 native HUD, menus, loading screens, movie display.**
- **0.2 = fully native graphics:** GS emulation off by default, judged frame by frame against the emulated GS output of the same build (target: visually indistinguishable; bit-exact pixels are unrealistic because VU1 and host floats differ, so residual differences must be measured and explained).
- **0.3 = native sound + native PS2 movie decoder:** sound from the engine's DARE/UbiSound calls (no IOP/SPU2 emulation); a **native decoder for the PS2 movie files** behind the game's own player (no IPU emulation; replaces the 13 Oct plan to adapt ps2-web-recomp's IPU). Reference: GT4Recomp's frame pacing + audio rate control (`docs/reference.md` "GT4Recomp").
- **0.4 = fully native,** incl. memory card, pad and rumble. References: GT4Recomp's crash-safe memory-card writer (cheap, could come earlier) and SDL3 pad layer (`docs/reference.md` "GT4Recomp").
- **1.0 = cleaned up, human readable, GS reference toggle; publish.** Plays exactly like the original release, at the original 30 fps.
- **Vibration (user, 18 Oct; later):** (1) faithful rumble: the game's DualShock 2 motor commands (small on/off, large 0-255 via the pad library), ignored by the runtime today, passed through a portable library (SDL rumble: DualSense, Xbox, ...); (2) optional DualSense haptics and adaptive triggers mapped to game events, designed with Jellybit's DualSense Haptic Designer (MIT; https://github.com/Jellybit/DualSense-Haptic-Designer; outputs 4-channel WAV for the grip motors and raw trigger bytes; full haptics need USB, Bluetooth only experimental).
- **Pre-publish clean-up before 1.0 goes public** (user, 18 Oct): a final housekeeping pass over our own code (overrides, native renderer, tools, runtime patches; not the generated code) for human readability, possibly with a stronger model (e.g. Fable). Before it: write a short style guide (naming, file size, comment style, hook layout), with id Software's GPL Doom 3 source as the style reference (style, not code); freeze golden outputs (byte-identical `pop_state_frame` frames for every test save, nightly checks, movie/ADPCM/audio reference outputs). During it: behaviour-preserving changes only, small commits per subsystem, all golden checks after each; comments keep their evidence (move long histories to `docs/`, never drop the why); decide which `POP_*` debug switches stay. After it: a short playtest by the user (input, timing, window). Lighter tidying at each milestone before that (e.g. `pop_native_live.cpp`, ~1,800 lines).
- **Reference renderer toggle in 1.0** (user, 16 Oct): the emulated GS ships as an optional reference renderer, native Vulkan is the default. One key toggles between them; by default the emulated GS is fed only while shown (correct again within a frame or two after switching, since PoP re-uploads its textures each frame); a "comparison mode" setting keeps feeding it every frame for instant switching (costs the VU1 + GS work). Possibly a split-screen view. So Stage D step 16 means "GS emulation off by default", not removed.
- **1.1 = widescreen (Hor+**, 4:3 stays the default): wider projection, widened game culling (findings from the PCSX2 widescreen patch; reSOT's Xbox projection/culling hooks as a reference), HUD anchored, movies/loading screen pillarboxed; check cutscene framing and edge pop-in by playing through. Plan: `docs/widescreen.md` (God Hand: Rearmed's method: constants computed from the aspect ratio via instruction hooks, HUD groups anchored, menus/loading screen latched to 4:3; the PCSX2 patch decoded against our ELF). Also GT4Recomp's any-aspect-ratio work (`docs/reference.md` "GT4Recomp").
- **1.2 = uncapped frame rate** (30 and 60 stay available as caps). Basis: the engine is variable-timestep (`TIM_Clock_Update` scales logic by real elapsed time) and the PCSX2 one-word patch (`POP_60FPS=1`) already gives 59.9 fps on the PC (16 Oct). Work: play-test at several rates (60, 120, 144+) for fixed-rate assumptions (physics, animation, sound, cutscenes, sand rewind, timers that count frames) and fix them; frame pacing. Fallback if logic breaks above some rate: a fixed logic tick with interpolated presentation (Chronicle's `DECOUPLING_PLAN.md`, `docs/reference.md` "Renderer and DLSS notes") (estimate: needs only the native renderer's draw lists).
- **1.3 = native higher resolution + crisp HUD.** Mostly render-target size once everything is native (2D/post-processing must scale too). HUD sprites drawn at output resolution, with upscaled or redrawn HUD textures.
- **1.4 = assets from the PC and Xbox versions** (both owned by the user; GameCube dropped from the plan 23 Oct), substituted by asset key (same keys on all platforms), in order: **textures, static meshes, skinned meshes, audio/sound effects/music/voice, movies** (Xbox Bink: highest bitrate, 5.1). Mesh swaps go through the native renderer; they need a decoder for the Xbox/PC D3D vertex buffers, and skinned meshes only swap if the bone layout matches; collision is separate data, so visual swaps do not change gameplay. Unverified: whether Xbox/PC meshes are more detailed (compare vertex counts per geometry key first; Xbox may be the better source). A per-category selector (texture source, mesh source, ...). The PS2 disc alone must still give the full game.
- **1.5 = Linux / macOS** (the portability rules below apply from now on, so this stays cheap).
- **1.6 = ray tracing:** shadows on select objects, ambient occlusion, reflections, diffuse GI, modern atmospheric effects, full path tracing. Each a separate toggle, drawn **before** the PS2 post-processing so the PS2 haze stays on top (see the RTX Remix notes in `docs/reference.md` for one route).
- **No DLSS / FSR / XeSS (user, 23 Oct; dropped from the old 1.3).** These temporal upscalers and frame generation only work inside a 3D renderer (jittered frames + depth + motion vectors, HUD drawn afterwards), not on finished 2D images. Higher resolution comes from rendering natively at it (1.3).
- **PAL and JP discs (some time):** each is a different ELF with different addresses; overrides hard-code retail US addresses, so new code should take addresses from one table per disc. PAL also brings 50 Hz and its language versions.
- **Publish at 1.0** (user, 15 Oct), as a base for the community; 1.1 onwards follow as updates. Publishing is limited to our own code (overrides, tools, runtime patches under their licences, PS2Recomp is GPL-3.0); users supply their own disc and build it. Never ship game data, generated code or prototype-derived material (GT4Recomp ships its translated code; we do not copy that, only its launcher/manifest ideas).

## Portability principle (user, 15 Oct 2026)

Windows is the target; Linux and macOS (Apple Silicon, via MoltenVK) should stay cheap to add later. Rules for new native code (renderer, sound, movies): Vulkan only, within what MoltenVK supports for the base path (no geometry shaders etc.); ray tracing as an optional extension; window, input, audio via portable libraries (SDL-class, FFmpeg); standard C++ without x86 intrinsics, or behind a wrapper with a portable fallback; no Windows-only APIs; shaders as ahead-of-time SPIR-V. Expected later effort (estimate): Linux days to ~2 weeks; macOS 1-3 weeks. Do not give up a fast x86 path where it matters, but keep a portable one next to it.

## Working reference (distilled 17 Oct 2026; the details and their history are in `docs/history.md` and `docs/reference.md`)

- **Where things are written down.** `docs/history.md`: dated session logs (boot log 11-12 Oct, sound bring-up 13-14 Oct, mesh reader / Stage A-C findings 14-16 Oct, old status blocks and plans). `docs/reference.md`: background that is not a session log (PS2Recomp pipeline/PRs/status, prototypes and symbols, cross-version assets, texture formats, survey of other PS2 projects, RTX Remix, HD packs, renderer/DLSS notes, sources, answered open questions). Topic docs: `docs/native_renderer.md` (Stage B-D, live window), `docs/postfx.md`, `docs/movies.md`, `docs/llm_jobs.md`, `docs/vu1_scheduling.md` (build-time VU scheduling plan), `docs/upstream_ps2recomp.md` (our PS2Recomp bug list), `docs/nightly/`. Before re-deriving a fact, grep these.
- **Builds.** PC (the real target, pop-sotc runtime, `game\recomp-sotc`): `tools\windows\rebuild.ps1 -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc` with `-Fast` (override/runtime-only changes, ~30 s-few min) or `-SkipGhidra` (recompiler or runtime-header changes, full ~30-50 min); boot `tools\windows\boot.ps1 -Play -Out game\recomp-sotc [-Seconds N] [-Env 'A=1;B=2'] [-Shots <fields>]`. Runtime patches live in `tools/patches/pop-sotc/` (`git am` into `C:\tools\PS2Recomp-sotc`, then `git -C game\recomp-sotc\runtime-src checkout --detach` its HEAD; currently up to 41 (39 = Esc quits, 40 = GS trace from an override, 41/42 = scheduler stub return, replaced functions only); remote `sotc` = TaylorNAlbarnaz/PS2Recomp). Compiled VU1/VU0 microcode: record with `PS2X_VU1_CAPTURE=game\recomp-sotc\vu1_capture` (both units since patch 31) or take the images from saves (`tools/vuimages.py <dir> <states>`), build with `rebuild.ps1 ... -Vu1 <that dir>` (all runner files recompile, ~30 min). Never put `vu0_*.bin` in a capture folder an older `vu1_recomp` reads (it expects 16 KB images). The play build links with /MAP (symbols for the profiler). Mac: the sotc base does not build there (x86 headers); the Mac uses the `pop` branch (`tools/rebuild.sh [--fast]`, `POP_BUILD=build-mac-play tools/boot.sh [secs] [log]`, exit 142 = time limit); Mac-only problems (medallion drift, curtain-load stall) are parked. Native tools: `cmake -S tools/native -B game/native-build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build game/native-build` (Mac: Homebrew molten-vk, vulkan-loader, vulkan-headers, glslang, SDL3).
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
