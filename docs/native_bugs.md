# Native renderer bug list (started 24 Oct 2026)

Rendering bugs seen in play with the native renderer on. Plan (user, 24 Oct): no stop-the-world fixing; clear the
list at each milestone (first pass when 0.12 is done, before the 0.13 effects). Each entry: what was seen, evidence,
hypothesis (marked as such), status.

**Status after the 25 Oct sweep.** Closed: N1 (palette in memory, `recolouredKey`), N2 fog blips and N10 torch
brightness and the save-point pop (late lists back on the GS; user in-motion checks 25 Oct), N5 (41 over-budget
generation switches in the 25 Oct sessions, no black flash), N9 (user saw the door right). Parked until they recur
(the user captures a save then): N3 stairs flash, N4 combat camera teleport. Watch in long sessions: N8 wall hue,
N11 wall missing (fix 3664837). Still open from the list: N6 (heavier scene fps), N7 (dagger flash, probably original).

**First: the renderer audit's five steps (user, 27 Oct; one at a time; findings `docs/renderer_audit.md`).**
1. **Done 27 Oct (Mac, offline):** shader alpha test (audit 1 + 3) and the one-liners 4, 5, 7; item 2 reverted (see
   the audit). `score_saves` over 22 saves: unchanged within 0.07 (184926 +0.07: a faint ring in the sword glow's
   alpha). Live A/B on the PC (`tools/windows/oct27.ps1`, saves 184926 / 115837 / 115853): counters and fps
   unchanged, screenshots look the same.
2. **Done 27 Oct.** The three targeted reads (results in the audit's last section): one fix (odd AREF turns the
   inverted alpha test into GREATER, as the GS does), one open question for step 3's census (pass 0 / pass 1 share
   the frame-seen key). Validation
   layer (`POP_VK_VALIDATION=1`; Mac: `brew install vulkan-validationlayers`, `VK_LAYER_PATH=/opt/homebrew/share/vulkan/explicit_layer.d`)
   + index bounds check in `FrameLoader::load` (drops the batch, prints it). Over 51 saves (`score_saves.sh`,
   `world_saves.sh` with characters): one defect, a zero-size map/copy in `uploadBuffer` on frames with no per-frame
   meshes (8 saves; fixed: skipped); no index out of bounds; scores unchanged. Live A/B on the PC (`oct27.ps1 -Tag
   oct27b`, the 3 saves; no validation layer there, no SDK): counters and fps unchanged, no dropped or missing batches,
   screenshots the same.
3. **Done 27 Oct** (answers in `docs/renderer_audit.md` items 9, 10 and the last section): texture alpha > 0x80
   none; vertex alpha > 0x80 yes (83 native alpha-tested draws, more in the GS late lists): two fixes proposed in
   item 9; GS features: nothing used beyond what is modelled; pass 0 + pass 1 on one element:
   never. **Follow-up (27 Oct evening):** the alpha-test fix the user chose was tried and reverted: the vertex
   alpha counts are packet bytes. **P1 check done (27 Oct night, audit item 9):** P1 passes vertex alpha to the GS
   unchanged (the "P1 changes alpha" idea was wrong: the save frames used as reference hold native pixels); its
   lightmap mode writes a distance fade as alpha that native lacks (baths saves). GS-only reference saves made the same night
   (`game/analysis/gsref1027/`): the instanced colour clamp (audit item 2) and alpha clamp are dropped, closer to the
   GS on all 5 saves. Lightmap distance fade done too (fog zone + 0x10 as alpha). Blending with As > 0x80: done 30 Oct (`docs/renderer_audit.md` item 9; open: the dagger glow too bright). The plan as it was: census from the saves of palette / vertex alpha > 0x80 reaching native draws, and of DTHE / MXL / PABE / FBA in one
   GS trace per area type (audit 9, 10): decides the fidelity work before 0.2. Measurement only; fix nothing in it
   without the user. Three questions in all:
   a. **Alpha > 0x80** (audit 9): over the 51 saves in `game/analysis/saves*/` (all at once: symlink the
      `.state` files into one scratch folder, as in step 2), count native draws whose texture/palette alpha or
      vertex alpha exceeds 0x80, per kind (static / skinned / instanced / shadow) and render-state blend kind. Where:
      `pop_state_frame` already decodes them (`tools/native/src/state_frame.cpp`; the alpha clamps are
      `assets.cpp:124`, `pop_native_live.cpp` ~3187, `mesh.frag` `min(vColour.a, 1.0)`). A counter printed at the
      end of `pop_state_frame` is the cheapest place, or a `POP_VIEW_LIST`-style side file.
   b. **GS features** (audit 10): which values the game sets for DTHE, TEX1 (MXL, mipmaps), PABE, FBA and the CLAMP
      region modes. One `PS2X_GS_TRACE=<field>:<n>:<path>` per area type (n >= 4 at 30 fps), decoded with
      `tools/gsvram.py`. Needs PC boots (TV on: ask; game closed). Or first try the saves' GS registers
      (`tools/popstate.py`), which needs no boot but holds only the last value of each register.
   c. **From step 2's reads:** does any element get Display calls with both pass 0 and pass 1 (same element, same
      matrix index) in one frame? If yes, the shared frame-seen key in `displayRecord` loses its pass-1 layer
      (audit, last section). Count it from the saves' draw lists (`pop_state_frame` walks them) or with a temporary
      counter in `displayRecord` during a live boot.
   Write the answers into the audit (items 9, 10) and propose fixes only where a count is non-zero.
4. **Done 28 Oct (Mac + one PC job).** The gate: `python3 -I tools/nightly/checks.py --only render` (Mac only, 18 s;
   also in the default run) runs `pop_blend_test` (one quad per blend kind, every branch of SetBlending 0x4B2500
   incl. inverse / destination alpha, against the GS formula from the ALPHA byte: 32 cases, all within 1 step) and
   `score_saves.sh` over the 11 GS-only saves in `tools/nightly/render_gate.txt` (`game/analysis/gsref1027/states/`;
   threshold = baseline + 0.1; real fixes moved scores by 0.1-1.3; lower a line in the same commit as a fix that
   improves it). Checked that a lowered threshold makes the report list the save. Six saves added 28 Oct by
   `game\jobs\gsref1028.ps1` (115837, 115853, 125052 chasm, 145139 save point, 155110 rewind, 184926 fight; TV off,
   display_active Disabled, still ~55 fps). How the saves are made: every native switch off,
   `POP_INPUT=r1@1:9,select@10,r1@11:3600` quick-saves 9 s after the load (inputs run in order); the job backs up and
   restores `quick.state`; `GSREF_NOBUILD=1` skips the rebuild. **The set must be GS-only saves** (lesson 27 Oct).
   The scores are absolute differences: the offline frame draws every list natively (late lists, sprites, particles
   too, unlike live play) and has no HUD, so 8-13 is normal for fights. Seen in the new side-by-sides (not fixed;
   candidates for 0.13): 145139 the moon blown out to a white blob natively (GS: disc with a soft halo; sprite halo
   in +0x88 or its glow mask), 155110 a bright light patch on the floor natively that the GS frame lacks
   (light shaft? no: glow, see "Light shafts"), 115853 the GS's sand particles / haze thicker than native.
   **First look 29 Oct (offline; none of the three can show in play today: +0x88, +0x98 and the glow stay on the
   GS live):** moon blob (145139) and floor patch (155110) are both the native **ColorDiffusion** (CPU reference,
   `postfx.h`) over-brightening a faint +0x98 mask. With `--noglow` the +0x98 draws change no colour (alpha-only is
   right) and the floor region matches the game (71.0 vs 71.5; with glow 74.8). The moon mask is at most 15/255
   (~0.06) yet the glow adds +36 to the region: each convolution level gains ~9 x 19/128 = 1.34 and the levels
   compound. If the GS did the same the moon would blow out there too, so suspect a missing factor in the
   reference (the ALPHA FIX of the tap adds?); check against a GS trace of ApplyConvolution's blits before 0.2.
   Sand haze (115853): the cloud and sparkles are 3 GFX draws in +0x88 (FUN_00458FC0: a 37-quad billboard with a
   64x64 texture, two oriented emitters) that the offline frame does not draw natively; SmartBlur ran with fade 0.
   Tools added for this: `POP_STATE_SKIP_LIST=<hex offset>` (pop_state_frame leaves out one list),
   `POP_VIEW_ALPHA=<png>` (pop_native_view writes the alpha mask before the glow), `popstate.py --frame <png>
   --alpha`. The save's displayed GS buffer alpha is **not** the glow mask (other shapes at save time), so the
   GS-side mask needs a GS trace.
5. **Done 27 Oct (two PC boots, TV off, display_active Disabled).** `tools\windows\profile-states.ps1 -States
   play_20261024_200200 -Start 6 -Duration 40` (the script now turns the native switches on as `play.ps1` does; before,
   it booted GS-only). **N6 does not reproduce from its save:** 60 fps for the whole 40 s, GS-only (report kept as
   `game\analysis\prof\play_20261024_200200.gsonly.report.txt`) and native (`...200200.report.txt`); the Prince stands
   still after the load, so the draws never rise to the slow stretch's ~1,450. Audit 12: worker 3.6-4.0 ms per frame
   (read 0.9, upload 1.7, draw 1.1-1.4) and the GS thread waits 3.4-4.0 ms, i.e. nearly all of it: the worker is on
   the critical path, as the audit said, but costs no frames at this load (60 fps). Audit 13: the stats line now ends
   with `<n> textures, <MB> MB` (86 textures, 3 MB, flat over 3,000 frames here); growth needs a long session, which
   every `play.ps1` session now logs. N6 itself waits for 0.2 with the other fps items, or a `play.ps1 -Profile`
   quick-save in the slow stretch.

**Order agreed 26 Oct (user), revised 26 Oct evening (P5 is done: `--chain p5` PASS 930 whole runs).**
Squash the repeatable bugs (a save shows them) before 0.13; every item below gets at least a quick look, so we do
not build 0.13 on top of broken code.
- **Rule (user, 26 Oct): a bug caused by the hybrid frame (native and GS drawing in one frame) is parked until the
  renderer is fully native (0.2)**, not fixed now: that code is about to go. The deciding check for each: the same
  save with every native switch off (GS only, as `game\jobs\glowab.ps1`). GS-only shows it too -> not the hybrid:
  look into it now. Only with native on -> find out whether it is the hybrid (native and GS interacting: depth handoff,
  late lists, fallback frames) or a bug in the native code itself; the first is parked here with a note, the second
  is fixed now.
**How each repeatable bug is checked (user, 29 Oct):** the same save three times, `tools\windows\play.ps1 -Mode
<m> -Save <name>`: (1) `hybrid` (as first seen), (2) `gs` (the original), (3) `native` (every native part, late lists
too). In 1 and 3, not 2: fix now. In 2: not a bug. In 1, not 3: already fixed. In 3, not 1: our newest code caused
it: fix now. Not in 1: park.
**Done 29 Oct evening: every repeatable bug had its look (user on the TV, `play.ps1 -Mode ... -Save ...`).**
1. **N13 / N14 / N16 flicker: fixed** (baths saves `game/analysis/saves1025e/` 162246, 162442, 162819, 163042; all
   four: seen in hybrid and native, not in gs). Two causes, each checked in game by the user:
   (a) ae260d9 (162246, 162442, 162819): with translucents native, a blended draw of +0x68/+0x78/+0x4C went native
   even after an earlier draw had gone to the GS (instanced walls with a lightmap entry, +0x2C, are not native yet);
   the GS then drew those walls over the native corner-stone overlay (162246 draw 835), which showed per pixel as
   the camera moved. Found by switches (world only / + shadows: none; + translucents: flicker). Blended or Z-less
   draws now need a pure frame, like the late lists. (b) f8daecb (163042, moss decal: draw 711, blend kind 1, no
   depth write): whole patches dropped out for single frames, camera still. Our Z differs from VU1's by tens of
   units, so the decal's GEQUAL against its wall flipped; on the GS it passes. Draws without depth write move nearer
   by `POP_NATIVE_DECAL_ZBIAS` GS Z units (mesh.vert; 1 still flickered in motion, 32 gone, nothing showing
   through: default 32). Not re-checked in game after (b): 162246 / 162442 / 162819 (b only loosens their test).
   Captures: `game/analysis/flicker1029/`.
2. **N15 sun brightness (162621): in the original too** (gs: "if anything even worse"). **User: revisit on real PS2
   hardware** to be sure the original looks like that.
   **N17** (fountain water over the save-point glow, 202926) the same: GS-only shows it too, real hardware decides.
3. **Rewind at 30 fps (155110): parked with the fps items.** Reproduces from the save in every mode (`[pop:fps]`
   lows hybrid 33/30, gs 32/39, native 32/30/30/42), so neither the hybrid nor fixed by going native. Profiles of
   the 12 s before each quick-save: `game/analysis/rewind1029/` (185650 hybrid, 185807 gs, 185943 native + logs);
   gs: VUThread busy (VU1 interpreter); hybrid/native: GameThread, top own cost `rasterAlpha` 9%. The 12 s window
   blurs the 2-3 s dip: a time-sliced look first when picked up.
4. **N7 dagger flash: set aside** (user: seen once, may not be reproducible).
5. **Performance (N6, fight fps 160324 / 160404 / 184926, the rewind): user, 29 Oct: all of it waits until fully
   native, then optimised in one go.**
Then 0.13: put P5 into the game (as P9: the port plus an interpreter fallback at branches the traces never
took; whole-run checks on new traces; plan in `docs/native_renderer.md` "P4 and P5 in the game"; P4 is an offline judge
for the native characters, not a live path), water (P6), halos, light shafts, flashes, then retry `-NativeLate`.

## Tooling first (agreed 24 Oct)

1. **Glitch capture (built 24 Oct evening, not yet used in play):** every kept quick-save (Select / F5) also writes
   `states/play_<date>_<time>.glitch` (+ `.txt`): the last 10 s (600 frames) of the injected native world picture at
   320x224 and one record per frame (state: injected / no picture / not drawn / unexpected buffers; draws; FRAME fbp
   and the target used; DISPFB1; purity and what ended it; late-list calls native/GS; the clip matrix; fog zones 0-4
   with colours). The kept copy is made 1-2 s after the press, so ~8 s before it are in. Read with
   `tools/glitchview.py <file.glitch> <out dir>` (PNGs + report.txt flagging picture changes, camera jumps, missed
   frames, fog and purity flips). Limit: the native world picture only (the GS draws characters, effects and 2D on
   top; the final picture is not readable each frame: GPU presentation). Code: `pop_native_world.cpp` glitchRecord
   / dumpRecent, `pop_native_live.cpp` noteGlitchFrame.
2. **Native conversion after a save-state load** (convert the elements already in RAM instead of waiting for
   ReadElement), so saves can be booted with the live renderer path and screenshotted with it on and off.

## N1: enemies slowed by the dagger do not turn sandy brown (24 Oct, play 19:23) (closed 25 Oct)

Long-press triangle (dagger of time) slows an enemy; on the PS2 it turns sandy brown. With the native renderer the
colour change is missing. Hypothesis: the effect changes something native characters ignore (material constant
colour, a light-set override or a second pass). Needs a save taken while an enemy is slowed (the effect lasts a few
seconds, so a quick-save can hold it; a save-state boot then shows it in `popstate.py --frame`). Priority: highest
of these (gameplay cue).

Second report (save 200418, 20:04, glitch capture too): the game picture shows the enemies pale grey. Offline
the save has empty draw lists (0 draws: saved after the game cleared them), so it cannot be judged offline. Next:
find the code that applies the freeze (dagger "slow" on an enemy) and what it changes per draw.

25 Oct: names from the prototype: `FX_Freeze_Add/Process/End` (retail 0x2735C0 / 0x2E2300 / 0x26F180) keep a stack
of (start, duration, factor) per object and pass the blended factor to FUN_001BED80 (time scale, not colour);
`SFX_LocalFreeze_*` (0x356EC0 / 0x357080) is the screen ripple (FX modifier 0x11 + local alpha), not the tint either.
The tint is not found yet. A scripted boot (184926, long triangle at 7 s, `game/jobs/n1slow.ps1`) slowed nobody, so
the input still needed is a quick-save taken by hand while an enemy is slowed (with its draw lists filled).

**Found and fixed 25 Oct (957ae46 + follow-up; user saw the sandy enemy in a native save-state boot at 14:45).**
User saves 134636 / 134815 (`game/analysis/saves1025c/`). The user noted the clock times the tint showed: only in the
GS-only boots, never native (the screenshots had missed it: two frames per run, see `docs/lessons.md`). The slow is
`FUN_00377E40` (from FUN_00271B00): time scale via FX_Freeze_Add plus an SFX modifier (FUN_00271440 sets its
fade-in / hold / fade-out, FUN_0038A640 starts it: state +0x28C = 1, 4 = stop). That modifier recolours the
enemy's texture through `GAO_ModifierSfx_UpdateMaterialPalette` (0x421100): it **rewrites the texture's 256-colour
palette in memory** (each colour blended towards sand by its luminance) and re-uploads it (driver vtable +0x2C,
op 0x16). Native decoded textures from the packs only, so it drew the disc palette. Fix: `recolouredKey`
(`pop_native_live.cpp`): a paletted texture whose palette in RAM (TEX_List texture +4 -> palette entry +4, +0x60
when its flags have 0x40) differs from the pack's is decoded with the RAM palette each frame under key
`kPaletteKey + texture index` (shadow key range: resent with pixels, updated in place). Log:
`recoloured texture <key> ...`. Ruled out on the way: lighting / constant colour (native reads the game's constant
colour correctly; a fix there, 8732934, was reverted), skinned pass 2 (none). First live try failed: the "no pack
palette" result was cached before the enemy's pack had loaded (packs load late after a state load), and per-frame
keys changed meaning between frames. **Closed 25 Oct**: the user confirmed slowed enemies render properly from a cold boot.

## N6: sustained frame-rate drop (save 200200, 20:02)

Glitch capture: 60 fps for 3 s, then 51-54 fps steadily (median frame 18.4 ms) while the frame's native draws rise
from ~1,050 to ~1,450. A heavier scene, not a hitch. Needs a `play.ps1 -Profile` session at that spot: the save alone runs at 60 fps
(27 Oct, audit step 5).

## N7: flash when the Prince raises the dagger after gathering sand (saves 195613 / 195733)

The glitch captures show nothing unusual in the native world picture (no flagged frames), so the flash is drawn by
the GS (an effect over the world); possibly the original effect. Check in an emulated session or PCSX2.

## N2: fog intensity blips when the camera turns (24 Oct)

Area fog jumps between intensities while rotating the camera. Saves 192947 / 193004 (`game/analysis/saves1024c/`)
do not show it (each save is one frame; offline A/B 11.97 / 6.50). In 193004 the bluish floor layer the native frame
lacks is the pool's water (CJSmWaterElement, P6 not native, always GS in game); the floor mist particles
(tex 2e004fe4, alpha ~0.05-0.075) are drawn and add ~+7 floor brightness. Hypothesis: lists +0x88/+0xA8 flip
between native and GS from frame to frame (the purity rule depends on what is on screen), and native and GS differ
for the fog-heavy draws (mist particles, fogged meshes). Test: run with `-NoNativeLate`; if the blip goes, it is
this. Needs the capture key otherwise.

Second try (saves 194256 / 194309 / 194313, `game/analysis/saves1024d/`): offline native matches the game picture
in all three (1.14 / 1.54 / 1.26), and the two close saves differ little (far wall luminance 47.0 vs 46.4). The one
thing that changes between them is the game's own **fog zone 2** (0x70000270 + 2 * 16), recomputed per frame:
min 24 / 28 / 21, a 292.9 / 286.8 / 298.0, colour 445b78 / 435977 / 465c79; all other zones identical. So the game
animates that zone with the camera, and the native frame follows it. Open: whether the blip seen in play is this
(original behaviour; check with `play.ps1 -NoNativeWorld`) or a native/GS switch the saves missed.

First glitch capture (`game/analysis/glitch1024/play_20261024_195406.glitch`, 600 frames, user: "fog blipping"):
all 600 frames injected, none missed; purity never reached the late lists (they stayed on the GS throughout), so
the native/GS-switch hypothesis is **ruled out** for this one. What moves is the game's own fog table: during a fast
camera turn (frames 256-270, ~230 ms, rotation 0.42 in the first frame after a still one) zone 0's a goes
301 -> 474 -> 318 and zone 2's min 51 -> 4 (higher a = less fog), and the native picture follows (frame means
44.8 -> 39.4 -> 49.5). The fog table is the game's computation (FUN_004BAE80 in Display3DElements), not ours, and the
offline judge matches native to the game picture on single frames. **Likely original behaviour** (camera-dependent
fog). To confirm: the same spot with `play.ps1 -NoNativeWorld` (fully emulated); if it blips the same, close N2.
Open side question: does POP_60FPS change how fast the game moves these values (a per-frame lerp would run twice as
fast)?

## N5: random one-frame flashes = black world frames when the kept meshes were dropped (closed 25 Oct)

Glitch captures 200042 (frame 411, mean 0.5) and 200541 (frame 458, mean 4.5): one native world picture almost
black, same draw count as its neighbours, injected normally, arriving late (30-40 ms). Each follows a
`[native] kept meshes over budget (512 MB): dropped` line in the play log (5 drops that session; one also before
save 200337). The renderer dropped all kept meshes in the middle of a frame whose draws referred to them, so that
frame drew nothing; the next frame resent everything. Fix: the renderer no longer drops by itself in the world
path (autoDropKept false); pop_native_world starts a new kept-mesh generation (frames written from then on send all
their geometry; meshSent ignores frames of an older generation) and drops the kept meshes just before the first
frame of the new generation. Tested with pop_world_test and POP_NATIVE_WORLD_KEPT_MB=1 (every frame over budget,
dropped each time, every picture right). Likely also N3.

## N3: one-frame flash walking up stairs, as if the environment mesh vanished (24 Oct, not captured)

Hypothesis: a frame whose world picture was not injected or came from a stale camera (the load flash family,
`docs/history.md` "Load flash (23 Oct)"). The session's counters were clean (play_20261024_192332.log: 25,200 frames
injected, 0 missed, 0 unexpected buffers), so not a missed injection; a stale-camera picture is not ruled out.

## N4: camera "teleported" to another part of the level during combat (24 Oct, not captured)

Seen once, for a moment. Hypothesis: a stale injected world picture from an earlier camera (as N3), or a real game
camera cut. Capture key needed.

## Triage of the 20:08-20:24 reports (24 Oct evening, offline)

Saves, captures and the session log (`play_20261024_195257.log`, in `game\recomp-sotc\` on the PC) copied to
`game/analysis/saves1024e/`; `score/` = `tools/native/score_saves.sh` (offline native vs the game's picture, `.side.png`),
`gv/`, `gv30/` = glitchview output, `sheets/` = 5x4 contact sheets of a capture (every 30th frame, ffmpeg tile).
The session ran **before** the N5 fix (30ef5d5 was built at 20:45). Three groups:

**A. N8, N9, N11: the live path draws an element wrong that offline draws right (stale per-address caches, hypothesis).**
- N8 (200833): offline has the back wall in shadow; live shows a flat bright panel behind the Prince.
- N9 (**201124**, not 201024-201039: those show the Prince kicking a *different* wooden door to splinters, drawn
  right): offline draws the closed wooden door; the live native picture draws it almost fully see-through (the room
  behind shows; faint door texture in the capture frames).
- N11 (201642 / 201650): offline draws the tower wall; live draws only its features (arrow slit, window frame) on
  the background.
Not texture keys: the session's 16 `texture index 132` warnings are all early (log lines 354-369) and offline reads
TEX_List the same way. What only the live path has: caches keyed by **element object address** that ReadElement does
not reset. `readElement` replaces `g_elements[element]` (and the world keys static meshes by its serial, so those are
fresh), but `g_fromPacket` ((element << 8) | pass: lightmap pass-3 batches, packet-decoded passes), `g_litBatches`
(lit passes), `g_skinned` and `g_skipped` are never invalidated. When a level section streams in and reuses the memory
of an unloaded one, a new element gets its base mesh right but its lightmap / lit pass / pass-2 layer / skinned-or-
skipped status from the old object at that address. Offline runs a fresh process per save, so it never sees this.
All three bugs are in areas just entered. **Fix built 25 Oct (3664837):** `readElement` erases every per-address
cache entry for the element (g_skinned, g_skipped, g_fromPacket and g_litBatches for passes 0-7). The regression
boots (184926 / 115837 / 115853 / 202314) are unchanged. Not covered: g_litBatches entries keyed by a
CJMeshElementRef's address (refs are not created through ReadElement); if a lit instanced object shows the same
symptom, key those by packet address too. In-game check: a play session through the same spots (201124's door: just
after the staircase climb).

**B. N10 (torch saturation with the camera) + save-point glow pop (handoff item 4): the halo is a depth query.**
The halo pass (FUN_004AA240, per halo from FUN_004ABFC0; prototype AE_ProcessOneHalo) calls FUN_004AB500
(ProcessLensFlare): it clears the frame alpha in the light's core rectangle, draws the rectangle at the **light's own
depth** with TEST ZTST GEQUAL (0x50000), writing alpha only where the light is in front of the scene's Z, then
halves that rectangle repeatedly (bilinear) to one averaged value spread over the halo area. The halo is drawn with
ALPHA 0x58 = Cs * Ad + Cd, so its brightness is the fraction of the core not hidden by the Z buffer. Our native
world writes that Z buffer: ~27% of its pixels differ slightly from the VU1's Z, plus POP_NATIVE_WORLD_ZBIAS = 128
pushes all of it back (more halo passes the test). A light set into a sconce or wall sits within those differences,
and the outcome changes with the view angle. N2's capture is separate (the game's own fog table). Test (in game,
one torch, one save point): default vs `POP_NATIVE_WORLD_ZBIAS=0` vs `-NoNativeWorld`. Likely fix: halos query
the GS-equivalent depth (unbiased native Z; or run the occlusion test natively from the light's position).
**Save-point pop, 25 Oct (saves 145139 / 145142, `game/analysis/saves1025d/`): caused by the native late lists.**
The user judged it in motion at 145139: GS only (all native off) no pop; all native but `-NoNativeLate` no pop
("perfect"); all native with `POP_NATIVE_WORLD_ZBIAS=0` still fading a little, much less than default. (My earlier
"GS-only pops too" came from screenshots and was wrong.) Facts: the frame's one halo is the moon (world 32.4, -295.2,
83.9; core 8x8 px at the moon, depth ~5,600; its 256 px quad washes over the column). The flare test
`GFX_i_Flare_TestDepth` (0x466CC0) asks the driver for depth with request 0x1B, but `PS2_l_Request` (0x4B4F40) only
handles 0x16 (ReloadPalette) and 1 (fog), so that test is geometric only. Offline world-mode depth at the moon core is
far (~1,450) with or without late lists. Live, the late lists start pure in nearly every frame and end inside +0x88 at
a GFX draw (~18 native calls, the rest GS; `[pop:world] late lists` line): the translucent lists are depth sorted, so
the camera moves draws across that native/GS boundary. Working explanation (not proven per draw): GS draws after the
boundary that test depth (the +0x98 glow mask, the halos) test against native Z (biased, slightly different precision)
or the GS's exact Z depending on which side drew the save point's parts, so the glow flips with the camera. The pop
goes away for good when the passes after the late lists are native too (halos/glow, 0.13 / 0.2).
Also seen: Esc did not quit the second session (killed with taskkill).
**Decision (user, 25 Oct):** late lists back on the GS by default (`play.ps1 -NativeLate` opts in; 065fc09); revisit if
0.13 (native halos/glow) does not fix it. Fps cost: none. Fight saves, R1 held, 45 s (`game/jobs/latefps.ps1`, 3
samples each): 184926 49.7 (GS) vs 49.5 (native), 115837 53.7 vs 46.2, 115853 58.2 vs 56.3.

25 Oct: tried with the torch saves 201244 / 201248 (`game/jobs/n10halo.ps1`, default / ZBIAS 0 / GS only): the game
resumes and the Prince walks away from the torch, so no shot has the torch in view. Needs the in-game test.
Note for screenshot jobs: `-Shots` counts fields from boot, and the counter jumps to the save's field at the load,
so only the first shot or two fire; give fields after the save's own field.

**C. N12 (sustained ~30 fps, 20:20-20:24) and N6: the frame falls back to the GS and the fallback costs double.**
Capture 202128: 16.7 ms per frame until frame ~360, then 27-33 ms from exactly the frame whose record changes to
`purity ended by shadow +0x5c, late native 0 GS 29-34` (draws 838 -> 1,138: enemies appearing); 202314 (the fight,
until the crash): 33-43 ms, the same reason throughout. N6's capture (200200): late lists on the GS all along
(`purity ended by ref +0x2c`), 18-19 ms. Two costs:
1. A shadow projector goes back to the GS when a receiver has no record: the log's `list entries without records`
   rises 6,559 -> 9,054 over the slow stretch (the enemies; known gaps: CJIdxMesh entries, blob receivers). Once the
   shadow is on the GS, every later list (+0x88/+0x98/+0xA8, particles, sprites) is too. **Fixed 25 Oct (e5ba9a5):**
   the entries were pass-1 entries of an element with no pass-1 packet (they draw nothing); saves 115837/115853 now
   stay native to +0x98 in ~2,095 of 2,100 frames (`docs/native_renderer.md` "Entries without records"). In-game
   check (save-point glow with many enemies) pending.
2. `spriteRecord` runs the P9 VU1 interpreter on every sprite draw **before** checking whether the frame can still
   be native: in the slow stretch `quads` rose by ~350-390k per log interval while `native` stayed at 45,316. The
   interpreter work is thrown away and the GS runs the same draw again. **Fixed 25 Oct (3664837):** with
   POP_NATIVE_WORLD on, spriteRecord returns to the GS before interpreting when the frame is no longer pure.
   (The "P9 not loaded yet ... draws 0" line in save 184926's boots only means that save has no sprites on screen;
   the other loaded saves run the P9 path.)

**Second session (20:36, log `play_20261024_203615.log`, saves in `game/analysis/saves1024f/`):** the N11 wall
drew fine at 20:41 (user), which fits A: a fresh process has a different memory history. Save 205204 (user: 30 fps
again) is a **different case**: the frame stayed native (late native 75, GS 4), the scene was constant (1,229 draws,
Prince + Farah) but the frame time climbed 21 -> 34 ms across the capture and the log's fps ends at a flat 33 (one
vsync missed each frame: the frame costs just over 16.7 ms). The 60 fps word (0x66D044) is still 1 in every save.
Kept meshes reached generation 11 (11 over-budget drops in 16 minutes). Cause unknown: needs a profile at that spot
(a `-Profile` session, or a save-state boot once conversion after a load exists).

**Not bugs of ours / not triaged:** 201751 = camera X in a fixed view (known, `docs/saves_1021.md`); 202314 =
crash 1 (fix 3b80b66 built after this session). 201039 = curtain corridor, offline and live agree. 201244 / 201248 =
torches with halos (game picture only; the halos are not in the offline frame).

## Reported 24 Oct 20:08-20:24 (play session from 19:52, log `play_20261024_195257.log` in `game\recomp-sotc\`), triaged above

Saves on the PC in `game\ps2\disc\states\` (each with `.glitch` + `.glitch.txt`; fetch to `game/analysis/`):
- **N8 wall hue (20:08, save 200833):** a section of wall with a different hue from the rest of the wall.
- **N9 wooden door fully transparent (20:11; the save that shows it is 201124, see the triage).**
- **N10 torch saturation changes as the camera turns (20:15, saves ~2014xx-2015xx).** User asks whether it shares a
  source with N2 (fog) and the save-point glow (CLAUDE.md handoff item 4). Hypothesis: the GS halo/glow pass
  (FUN_004ABFC0) tests visibility against the depth buffer, which now partly comes from the native world.
- **N11 wall features drawn but the wall itself missing (20:16, dungeon, save ~201642 / 201650).**
- **Camera X inverted again in a fixed-camera view (20:18, save ~201751):** the known bug in `docs/saves_1021.md`.
- **N12 frame rate down to 30 fps (20:21, save ~202014 / 202128).**
- **Crash 20:24 (save 202314, the minute before):** crash 1 again; fix built (`docs/saves_1021.md`).
The exact save-to-report mapping above is by time (user's minutes vs save names); check each picture.

## Reported 25 Oct 12:46-12:55 (play log `play_20261025_124621.log`; copies in `game/analysis/saves1025b/`)

Session on the N12 fix build (e5ba9a5): ~60 fps, every shadow projector native (143,782 of 143,782).
- **125052: reference save, looking down into the bottom of a chasm** (user: for checking later that it renders right).
- **125331 / 125417: save point missing on entering the room, then only its white cloud, the light column appearing
  gradually over the fight.** A/B from 125417 (`game\jobs\glowab.ps1`: the same save with all native switches on, and
  all off, screenshots at ~0/3/7/11 s): **the GS-only boot shows the same** (cloud, no column, still none at 11 s). So
  not the native renderer at that moment; most likely the game's own behaviour during a fight (not confirmed from the
  code; the column's fade is still to be found). 125331 (save point completely missing) not A/B'd.
  **Closed 25 Oct:** a PS2 playthrough video (user) shows the save point in a fight as the sand glow on the floor
  with no column, the column fading in over the fight: original behaviour. (The GS-only A/B alone could not show this:
  game code, VU and GS emulation are shared by both runs.)

## Reported 25 Oct 15:51-16:30 (play log `play_20261025_154156.log`; copies in `game/analysis/saves1025e/`)

Session 15:41-16:50 on the 25 Oct evening build (late lists on the GS), mostly the baths level; clean exit, no
`missing-target` line. **User (26 Oct): filed first, now first in the agreed order (top of this file);** many may come from the hybrid
native/GS frame and go away as it becomes fully native. Cheap first check when picked up: the same save with all
native switches off (GS only, as `game\jobs\glowab.ps1`); if GS-only shows it too, it is not the hybrid. Save-to-report
mapping is by time (the user's minutes vs save names); check each picture first.
- **Rewind at 30 fps again (15:51, save 155110).** The first capture of the older "rewind at 30 fps" item
  (115853 did not reproduce it in a 45 s boot).
- **No sound from the spinning spike columns (15:56, save 155620)** and **no sound from a door opening (16:00,
  saves 160002 / 160031);** user: "sound from this entire section is kinda messed up". Emulated sound, so for 0.3
  (native sound); save-state boots have no sound, so these saves only locate the place.
- **Fight with low fps (~16:05, saves 160324 / 160404).** The log shows ~46 fps average (min 30) over the fight
  stretch after 16:03:24, ~60 samples after 16:04:04. Same family as the "fight fps" item.
- **N13 flickering texture on a vertical wall corner (16:22, save 162246).** Fixed 29 Oct (above).
- **N14 another flickering wall texture (16:24, save 162442).** Fixed 29 Oct (above).
- **N15 sun brightness differs a lot between two camera rotations (16:26, saves 162621 / 162625, 4 s apart).**
  29 Oct: in the GS-only run too; revisit on real PS2 hardware (user).
  Compare N10 (torch brightness with the camera turn, gone with the late lists on the GS).
- **N16 flickering texture along the line of a ledge (16:28, save 162819)** and **more flicker (16:30, save
  163042).** Fixed 29 Oct (above: two causes, translucent order and decal depth).
- **Water test set for 0.13 (P6):** the baths saves of this session (~16:12-16:31) have a lot of water.

## Reported 29 Oct evening (P5 live check, `play.ps1 -Mode native -Save ... -Env POP_NATIVE_P5=1`)

- **N17 fountain water drawn in front of the save-point glow** when the fountain is behind the save point (fight
  save 184926; the user's post-fight save `play_20261029_202926`, Mac copy `game/analysis/saves1029/`). **GS-only shows it too**, so not native
  and not the hybrid: the original or our GS emulation. **User: check on real PS2 hardware** (with N15). P6 will not
  change it; when water goes native, its order should match GS-only, not fix this.

## Reported 30 Oct evening (saves `game/analysis/saves1030b/`)

- **N18 flickering edge (19:25, save 192544).** The save's picture (the live hybrid frame) has a thin dark seam along
  the lower ledge's edge (about x 20-240, y 360-370); the offline native frame has none (`pop_state_frame`, score 1.48).
  Not yet: the user's circle, the three-mode check.
- **N19 flicker (19:26, save 192645).** The save's picture has an orange scalloped band across the wall at frieze
  height, in front of the grass (x ~150-640, y ~180-200); the offline native frame has none (score 4.77; the rest is
  the missing offline shadows). Looks like a wall-band texture drawn over the native wall (a GS draw over the injected
  picture, or a native decal's Z live, as N16 b). Not yet: the user's circle, the three-mode check.

- **Offline pass over N20-N25 (30 Oct late, `pop_state_frame_vu1` + `score_saves.sh`, sides in the session scratchpad;
  the "game" picture of a save = the live hybrid frame the user saw).** Live log of the session:
  `game/analysis/saves1030b/play_191958.log` (copied while the game ran).
- **N20 dagger glow through a wall (19:31, saves 193136 "normal" / 193139 "glow through the wall").** Offline native
  draws a cyan-green glow column beside the hanging Prince in both (193139: x ~330-420, y 60-300), the live pictures
  differ in the window's light shaft (193136 much brighter). Which one the user means: needs the circle. Lead: the
  dagger-glow billboard (`docs/renderer_audit.md` item 9) drawn natively without the occlusion the GS gives it, or the
  glow mask (+0x98) letting ColorDiffusion bloom it through the wall.
- **N21 candle glow blowing out (19:32, save 193240).** The live picture has a huge white bloom on the floor; offline
  the save replays almost nothing (lists emptied by the save's timing), so offline can't judge. Likely the same family
  as N24 (over-bright glow / halos). PC check.
- **N22 water reflection on the wall swapping between two animations (19:33, save 193355).** Offline matches the save's
  picture (0.97), so one frame shows nothing. Lead: the caustic layer drawn natively in pure frames and by the GS in the
  others (purity ending in `+0x88` by refs in this area: the session's stats end it by "ref +0x88" in 18k frames); if
  our UV animation phase differs from the game's, the layer would jump each time purity flips. Check: `-NoNativeLate`.
- **N23 steady ~45-55 fps (19:35, save 193529).** The log: 47-48 just before the save, 50-55 for about a minute after.
  In that window the game draws ~10-15 halos per frame (all native; 4-6 before, ~1 after). From the running averages:
  worker ~6.5 ms per frame (normal 4.6), the game thread's wait for the picture ~5.7 ms (normal 4.2). So a halo-heavy
  spot; each native halo costs a frame copy. Perf work waits until fully native (user), but the GS-only fps at the
  same spot decides whether this is ours (a regression) or the game's.
- **N24 lights too bright (19:36, save 193636; same hall as N23).** Offline native halos are slightly bigger and
  brighter than the live picture's (diff around each lamp and the two braziers); live they were too bright to the
  user. Compare against GS-only at the same spot (also N15 / N17: the original may look like that).
- **N25 dagger glow far too bright when the Prince first takes it (19:40, save 194005, with a `.glitch` capture on the
  PC).** Offline native: a huge white bloom over the blade; the save's live picture shows a modest glow (the capture
  frame may not be the brightest). The capture's 600 frames are all injected and pure to the HUD, so the native
  path ran. Same draw as `docs/renderer_audit.md` item 9 (its ~8% live gap), much worse in this close-up.

- **N26 fps drops in the earthquake (19:41, saves 194134 / 194136, `.glitch` captures on the PC).** The log: 50, 44,
  52, 49, 55 fps over ~25 s (60 before and after); the capture's frame times agree (47-57 fps). In that stretch the
  frame has ~2500 draws (1800 in the dagger scene), all pure and native to the HUD, halos 3-16 per frame, particles
  40-55; worker and GS-thread wait went up to ~6 ms (estimate, from the running averages). Not decided whether it is
  ours: the same spot in `gs` mode decides. Both saves landed mid-frame (the save's picture holds only the halos, as
  193240), so offline can't show them.

- **PC results (job `game/jobs/bugs1030.ps1`, 30 Oct 19:48; GS-only vs native boots at each save's field + 60 / + 120,
  R1 held; shots PC `game\recomp-sotc\shots1030b\`, logs `bug_<save>_<mode>.log`):**
  - **Ours (GS-only clean):** N18 (a bright sliver along the lower ledge edge, x 250-430 y 380-395), N19 (the orange
    scalloped band at frieze height is native only), N20 (193136: the window's light shafts far too bright native, a cyan
    column beside the Prince and a white floor patch; the GS draws a faint shaft; 193139 matches: so the "glow through the
    wall" is most likely that shaft), N24 (lamp / brazier halos much bigger and brighter native), N25 (dagger glow and the
    ceiling light brighter native).
  - **Fps, ours:** N23 193529 GS 55 60 60 60 60, native 43 46 52 56 55 (~9 halos per frame; worker 6.66 ms: read 2.15,
    upload 3.31, draw 1.18; GS thread waited 6.31 ms); N26 194134 GS 54 60 60 60 60, native 45 51 52 52 53 (~14 halos
    per frame, worker 5.94 ms), 194136 the same. Lead: each native halo adds image records (kHaloAlphaKey -3 /
    kHaloRestoreKey -4, "the frame copy"): check what each one sends.
  - **Static shots match (in-motion only):** N21 193240, N22 193355. N26's shots diverge (the shake is random).
  - **Post chain (`POP_NATIVE_POST=1`): glow native 0** on every save: in world mode the after-effect hooks were never
    installed (only on the offline path). Fixed in a6feee4; rerun `game/jobs/post1030.ps1` (20:00): glow native 596 /
    600 (202926), 590 (115837), 596 (194005), 598 (193636), 112 (145139: purity ends in +0x88 there). Against GS-only:
    202926 7.36 -> 6.55, **194005 9.61 -> 6.66, 193636 7.63 -> 2.82**: N24 and N25 were the GS's glow over the native
    picture (a hybrid effect); with the native post chain the halos and the dagger glow match the GS. Draw +0.3-0.5 ms.
    Next: the user's look with `-Env 'POP_NATIVE_POST=1'`, then default on in `play.ps1`.
  - **User look, 193636 with `POP_NATIVE_POST=1` (30 Oct ~20:10): N24 fixed** ("much smaller and less bright").
  - **User look, 194005: N25 fixed** ("much less bright"). `POP_NATIVE_POST=1` is now the default in `play.ps1`
    (`-NoNativePost` opts out).
  - **N22, user's in-motion check: ours** (native with the post chain: still hops between two animations; `gs` mode
    193355: no hopping). The user circled x 165-245, y 360-430 of the 193355 frame: the foot of the left wall at the water, under the left ledge.
    **Cause (offline, 30 Oct):** draw 418 of the save's frame, the canal-wall caustic (image 83, flags 0xC0040207: U and V
    scroll from sub +0x14 / +0x18, advanced by FUN_0044A840 inside DoComputeAndSetUVMatrix, once per frame per sub).
    `currentMaterial` took the UV matrix from the render state's sub (0x70000034), which CElementList::Render resends
    only when the flags / AREF words change; the UV matrix is per element (FUN_004B5EC0(0x70000080, element sub) when
    the mode word is 2, via FUN_004BA5C0 or directly). Two caustic layers with equal flags and other scrolls next to each
    other in the sorted list: the second got the first's scroll, and the order changes as the camera moves. Fix: a hook on
    FUN_004B5EC0 keeps the element's sub (`g_uvSub`), used when 0x70000080 == 2; `state_frame.cpp` feeds both offline.
    Render gate unchanged (offline always had the element's own sub). **User recheck (30 Oct ~20:25): NOT fixed** (the
    fix stays: it is right by the code, harmless, gate unchanged). New clue from the user: **the animation changes only
    while the Prince moves; standing still it is steady** (so: tied to the camera / Prince moving, not to the scroll timer).
    Glitch captures made while moving: `game/analysis/glitch1030/play_20261030_202309` / `_202313` (.glitch, .txt, .state;
    `_202313_all/` = all 600 frames via `glitchview.py --every 1`; mean of the circled spot at half size (82-122,
    180-215) moves smoothly 34.7 -> 30.9 -> 36.9 with the camera, nothing visible at half size yet). All frames pure.
    **Fixed (31 Oct, 4a8c5bf; user in-motion check: "reflection is consistent as I move", two overlapping animations
    as the original).** A/B first: `POP_NATIVE_NOUVM=1` (scroll frozen) had no hop, so the scroll was involved. Cause: the
    native "drawn this frame" key (element or ref << 16 | matrix index | pass bits) left out the entry's layer.
    CElementList::Render (cases 0x24-0x2B) picks the sub-material by the entry's layer byte (+0xC) and sets 0x70000034 per
    entry, so one element is drawn once per layer. Draw 418's element has two caustic layers (U+V scroll blend 0x600, V-only)
    and four caustic refs beside it have two each; only the first in the depth sort was kept, and that order changes as
    the Prince moves. Fix: `drawKeyBits` (was `alphaPassBit`) adds a 14-bit hash of 0x70000034 at bits 42-55. Offline
    202313: 481 -> 488 draws (both layers), render gate unchanged (16 saves). The 724edd9 UV-sub hook stays (right by the code).
- **N20, 31 Oct (user looks + offline):** with the post chain on, the cyan column beside the Prince is gone and the
  window's shaft brightness is right. **Left: rays native only** (diagonal up the wall to the left, horizontal across the
  floor); `gs` mode shows only a much fainter general glow on the wall that pops in and out as the camera turns (**real-PS2
  check**, with N15 / N17 / N27). Native with `POP_NATIVE_HALOS=0` looks the same as `gs` to the user: **our halo is the
  cause.** Offline 193136: the rays are halo 2 (draws 543-547) lighting up the colour under the glow-mask shafts, which the
  glow (ColorDiffusion, after the halos in the game too) then blooms; `--noglow` shows the halo itself as a soft round
  glow. So our halo is visible where the GS's mostly is not. Halo 2 has a huge core square (half size 208 px; halo 1 at
  nearly the same position: 18 px; offline entries are stale replays, live values unknown). ProcessLensFlare (0x4AB500):
  clear alpha over the core, test sprite ZGEQUAL with alpha = param << 24, then halving from the core rect into a square
  of the shorter side at its top-left, repeated (bilinear, +-8 subpixel insets) down to 2x2, then the spread over the
  halo square; our 8x8-sample mean is the same idea, so the occlusion test result itself is the likely difference
  (native depth vs GS Z under the core). Native shaft meshes (flags 0x1400F / 0x1600F, alpha only) do write depth (flag
  0x200 clear). **Next:** a GS-only trace (`PS2X_GS_TRACE`) of 193136 to read the GS's test sprite, its Z buffer under the
  core and the alpha after halving, against ours (debug print of the halo's screen box: add back in `haloRecord` before
  `onScreen`).
- **N20 + N21 fixed (31 Oct, fbf650f; user checks: 193136 "no rays up the wall, glow on the wall looks good", candle save
  `play_20261031_111155` (Mac copy `game/analysis/saves1031/`, first-person view at the wall candles) "identical to the GS
  render"; before: a near-white square over the candles).** Cause: the halo's restore read the copy the spread took, after
  the clear and the test had written alpha into the core square, so the core kept the test's alpha instead of the glow mask
  and ColorDiffusion bloomed it (N21: an 18 px core = a white square; N20: a 208 px core = rays where the test passed).
  Fix: each halo first draws a save marker over its square (image -6, writes nothing; `Draw::saveFrame` copies into a
  second image `kFrameSaveImage`), the restore (-4) reads that save. Gate unchanged, blend test ok. Still for the
  **real-PS2 check:** the faint wall glow at 193136 popping in and out as the camera turns (GS and native alike now).
- **N18 rubble seam (closed 1 Nov: native ~ GS; real-PS2 check wanted).** The user's save
  `play_20261031_112025` (+ `.glitch`; Mac copy `game/analysis/saves1031/`): a ruined courtyard with stone blocks, the
  Prince standing. **What the user sees:** along the foot of the big blocks (where block meets ground) a dark line that
  blinks out (flickers LIGHT) while the Prince or camera moves, and keeps going ~0.5 s after. **`gs` mode: no flicker, and
  the block-to-ground transition is much softer** (no hard dark line). What we found and changed, in order:
  1. **The frame was hybrid there** (`purity ended by ref +0x88`, ~85 late draws on the GS each frame). Cause: texture
     600001ce (8x8, format 3, white) refused by `assets.cpp` texture8: it is sent at its own size (TRXREG 8x8), so it is
     linear PSMT8, not the PSMCT32 half-size swizzle the decoder assumed (refused for being under a 16x16 block). Fix
     (e4cf885): TRXREG width == w -> linear; `jadetex.py` the same.
     Live after: pure at +0x88 in 3,899 / 3,900 frames. **Flicker unchanged** (so the hybrid was not the cause).
  2. **The dark line is the lightmap pass + our decal depth bias.** Offline (draws 0-347 vs 0-460: material flags
     0x30284 = CLMElementList's lightmaps) the seams at the blocks' feet are lightmap darkening. Live switches:
     `POP_NATIVE_DECAL_ZBIAS=256`: no flicker with the camera, slight with the Prince; a slope-scaled bias
     (added then reverted: 0346867 / 9a1505f) made it subtler; 256 + slope 2: no flicker but the seam "judders".
     **Stills** (job `game/jobs/n18shots.ps1`, PC `game\recomp-sotc\shots1031\n18_{gs,native,nobias}_field-25262.png`):
     native with the default bias has a hard dark line along the big block's foot (y ~0.81 of the frame, left two
     thirds) that GS has not; **native with ZBIAS=0 looks like GS.** Reading: the bias (f8daecb, 29 Oct, for the N16 moss
     decal) also moved the lightmaps nearer, so the part of a block's lightmap just under the ground showed through as a
     line, blinking with depth precision. N18 was first reported 30 Oct, the day after that change.
  3. **Fix in place (2fdf0fa): no decal bias for lightmaps** (`native_renderer.cpp`, `mat->fogAlpha` = flags 0x30284).
     Gate and blend test unchanged. **User check: still a faint flicker** (not described further; the session ended).
  4. **1 Nov (user at the TV, annotated `game/analysis/saves1101/n18_side_by_side_GS-left_NATIVE-right.png`):** the
     faint flicker is at the foot of the lower big block (two spots along it, the left more often), **only when the
     Prince moves, never with the camera.** That points at the shadow receivers (the Prince's projector follows him and
     looks straight down; receivers redraw the base's own triangles, GEQUAL, no Z write, so they still had the decal
     bias). Fix 527fca4: no bias for `clampUv` materials (= receivers only). **GS shows the same flicker once in a while**
     (user saw one): the original has the same mechanism at the GS's rounding scale; the bias widened it. Real-PS2 check
     wanted for that one. The thin diagonal light line in the native still is not visible in play (user): dropped.
     A/B boots (user at the TV, same save, Prince running along the upper ledge): `-NoNativeShadows`: same rate as
     native (so the receivers were not the main cause); `POP_NATIVE_DECAL_ZBIAS=0`: about the same, perhaps a bit rarer.
  5. **`invariant gl_Position` in `mesh.vert` (e51a848; blend test 0 wrong):** user: "maybe a LITTLE bit more than GS
     but hard to tell". **Closed 1 Nov (native ~ GS at that spot).** Left for the real-PS2 check: whether the original
     flickers there too (our GS does, rarely). If it ever comes back: the decal bias (32) is the remaining suspect.
  **Next, with fresh eyes:** (a) ask what the faint flicker looks like now (the seam line, or something else at the same
  spot) and get the user's circle on a still (send `n18_native` / `n18_gs` shots to annotate on the Mac); (b) the
  lightmaps now draw at exactly their base's depth with GEQUAL: that is only stable if the base and lightmap vertices
  give identical depth. Check that the pass-3 positions equal the base packet's (offline: same element's batches) and
  consider `invariant gl_Position` in `mesh.vert` (different pipelines may round differently); (c) a GS-only trace of a
  seam pixel would show whether the GS itself draws that lightmap texel there; (d) check the other no-depth-write draws
  over this spot (list of no-Z-write materials in the 112025 offline frame: kinds 0x10604-ish decals, receivers) with
  bias 0 vs 32, since the remaining flicker may be a real decal there; (e) re-check N16's moss save 163042 is still
  clean (unchanged for decals, but confirm). Captures: `game/analysis/saves1031/`, offline scratch: rerun
  `pop_state_frame_vu1` + `POP_VIEW_LIST=1 pop_native_view` on the save.
- **N19 (orange band on the wall at frieze height, 192645): closed 1 Nov (afa2244; user TV check: band and moss brightening gone).**
  **Fix (1 Nov):** `refDisplayRecord` poses skinned refs like skinned elements: `poseSkinned(element, index)` (the ref Display
  decompile, 0x4C7B40, uses the element's palette at ref +0x20 -> +0x48 and the same first-bone table), records them in
  `g_framePosed`, replays the driver switch. Ref packets are REF tags (3 qw per instance at ref +0x10, then the element's
  shared 25 qw block), so `skinnedPacketBatches` flattens REF/CNT tags into one buffer for the skinned decoder (the
  element path uses it too). Bind cache per element then packet, cleared in `readElement` (streaming reuse, as N8/N9/N11).
  Skinned refs left on the GS (characters off, pass 2/3) now print a WHY line. Offline 192645: the 14 refs now pose
  (83 skinned draws, was 69) and the picture is pixel-identical to before (offline drew them as statics already; live,
  the refusal sent them to the GS a second time). **Boot 1 Nov (`game\jobs\n19boot.ps1`, WHY on, 45 s each):** 192645 and
  162621 both 2399/2400 frames pure at +0x88, `ref +0x2c` gone from "ended by" (was ~26% of frames), 0 skinned refs on
  the GS, 60 fps; the rest end at the HUD (+0xB8, 0.14). One-offs left (1-2 frames in 2400, not N19): `ref +0x88`
  (ref 01693bc0, 192645), `mesh +0x2c` / `mesh +0x88` / `gfx +0x88` (162621).
  Render gate after the fix: the 13 GS-only saves in `gsref1027/states/` pixel-identical to the build before (the shared
  `skinnedPacketBatches` changes nothing for elements).
  **What the user sees (native only; GS mode clean):** a soft orange scalloped band across the wall just above the
  grass, coming and going, and **the moss/grass below it brightens at the same moments** (user's annotation:
  `game/analysis/saves1101/desk_114347_8.png`, top box = band, ellipse = moss). Not the post chain: still there with
  `-NoNativePost`. **Cause:** `POP_NATIVE_WORLD_WHY=1` boot (log PC `game\recomp-sotc\play_20261101_114159_native.log`):
  purity ended by `ref +0x2c` in 4,400 of 16,800 frames, every time `ref on the GS: ref 016b2900 pass 0 list +0x2c
  stateOk 1` with no per-record reason. In the save, ref 016b2900 -> element 01ada6e0 with **bone palette +0x48 =
  0169dbe0** (chain of 7 refs): a **skinned instanced mesh** (likely the moss/grass tufts). `refDisplayRecord`
  (`tools/overrides/pop_native_live.cpp` ~1388) drops skinned refs silently (`worldOk = false; // a bone palette`), so
  the GS draws it and everything after it over the injected picture: the moss twice (brighter) and the fringe on the
  wall without our depth (the band). Same silent break as the sun save 162621's `ref +0x2c` (comment ~1464).
  **Plan (fresh session):** (1) add a why-log line at the skinned-ref drop; (2) give `refDisplayRecord` the element
  path's skinned handling (`elementDisplayRecord` ~3270-3380): bind pose from the ref's pass packet
  (`skinnedFromPacket`, as for elements never seen at load), `poseSkinned(rdram, el, index, ...)` with the ref's matrix
  index (bones from the palette + first-bone table indexed by `index`), `g_framePosed`, the VU1 driver switch replay
  (`worldReplayDriver`, the element path's `worldPalettes`) and the purity bookkeeping (`worldCount0`); key the bind
  cache per element (refs share it). (3) offline: `pop_state_frame` on 192645 (does it replay refs' skinned draws?) vs
  the GS frame; (4) stnative boot with WHY: `ref +0x2c` gone from "ended by"; (5) user's TV check on 192645 and the
  sun save 162621. Boot wrappers on the PC: `game\jobs\n19native.ps1` (WHY on), `n19gs.ps1`, `n19nopost.ps1`.
- **N27 (parked, needs a reference): the halo nearest the Prince flickers a little** (193636, native post chain on).
  Not known whether the original does it; our GS emulation is not ground truth. Check against PCSX2 / a video / a real
  PS2 when available.
- **N28 (parked, minor; user 1 Nov): objects pop back in at the end of the fade to black after Retry.** Step 5 boot
  (`POP_NATIVE_2D=1`): on 1 of ~3 die / Retry fade-outs, at the last fade step some objects (enemies, torches) showed
  while the rest was black. User saves just after: `play_20261101_135939` / `140020` (PC states). Capture
  `game\analysis\retryfade1101` (`POP_NATIVE_WORLD_DUMP`, 3 frames before / 12 after each load): the 3 frames before
  each retry load are black; after load 03 (frame 4171-4172) 2 injected pictures show a close-up wall from a stale
  camera (the 23 Oct load-flash family, N3), the other retries black throughout. Not obviously what the user saw
  (objects on black, before the load). Not known: GS-only (`-NoNative2D`) does the same? Next: a GS-only retry A/B.
- **N29 (not a bug for now; user 1 Nov): white flash at the start of every rewind.** Seen in native play from 155110
  (blended Pinch native, 6054f84; `play_20261101_155413.log`: pinch 1721 calls, 1718 native) and **in `-Mode gs` too**
  (user A/B, same save), so it is the game's own effect, not ours. The four user saves (`play_20261101_155605` ..
  `155619`, copies in `game/analysis/saves1101/`) are mid-rewind, flash not caught, Pinch +0x74 0xFF in all. Re-check on a
  real PS2 / video with N15 / N17 / N27.
- **N30 (parked until the real-PS2 reference, user 1 Nov; GS off report): enemy slowed by the dagger drawn bone white, "no skin".** Save 170233 (user circled
  the knife enemy right of centre; `game/analysis/gsoff1101h/`, also 170118), `play.ps1 -GsOff`, log
  `play_20261101_165944.log` (all draws native, 0 missed). Offline `pop_state_frame` shows the same white, so it is in
  the native path, not the GS-off switch. The 25 Oct save 134636 (GS still running) already differs: GS picture keeps
  the orange sash / dark trousers, native all pale (`gsoff1101h/refout/crop_*.png`). The RAM palette (N1 recolour) is
  bright tan (mean 174/145/113 vs disc 98/79/64) and the GS's VRAM holds that same palette, so the colours match on both
  sides. User boots of 170233 (1 Nov): hybrid with census (`play_20261101_171954.log`): no GS colour draw over the enemy
  (only 14e/15e alpha, 006 clear, 05e zero-area), the texture recoloured natively every frame; user saw "standard
  texture in black and white". `-Mode gs`: **the same** (user). So native matches the GS live at this save; the
  "sandy" look remembered from 25 Oct was not seen in either. Open: the bone white of the GS-off session (save
  picture, offline render) vs the GS-only grey; the 134636 save (native whiter than the GS picture). Next: a fresh slow
  (not from a save) in `-Mode gs` and in `-GsOff`, side by side by eye; a real-PS2 / video reference with N27.

## Storage of captures (agreed with the user 24 Oct)

`.state` files (48 MB) are kept: the offline A/B test set. `.glitch` captures (129 MB) are temporary: after
triage, note the finding here and delete the capture once its bug is fixed and checked (or it showed nothing).
Planned: compress the capture pictures in the background thread when written (next rebuild).

**Clear-out 25 Oct evening (user asked):** kept `.glitch` captures only for bugs still open or on watch: 200200 (N6),
200833 (N8), 201642 / 201650 (N11), 202128 (N12); the others deleted on both machines (`.glitch.txt` kept). States
deleted: near-duplicates (several saves seconds apart in the same spot: kept every save the docs name, else the last of
the burst; 65 saves, e.g. the 18 Oct 21:39-21:40 camera pairs except 213954 / 214002 / 214005), closed-bug extras
125331, 134528, 135513, 135609, the N1 capture saves (`n1_*`, `n1cap_*`), and the Mac's exact copies (`game/states_pc/`,
`prof1019b/`, the `in/` and `ref/` copies). Freed 7.6 GB on the PC (left: 10.6 GB), 8.2 GB on the Mac.


## Side jobs and small open items (moved from CLAUDE.md on 30 Oct 2026, verbatim)

  Then light shafts (not FUN_00469C00: see "Light shafts" below; the mirror beam is
  something else), flashes (full-screen after-effects: survey first), retry `-NativeLate`. Parked: N3, N4 (until they recur), N7.
  Watch: N8 / N11 in long sessions. Side jobs: `pop_world_test` dies with exit 138 on the Mac (seen 30 Oct, before the key-0 change too), `.glitch` compression, a `-Fast` rebuild that recompiled the whole
  runtime, Esc / Alt+F4 sometimes not quitting, `POP_NATIVE_SKIP` is ignored with the native world on. Lower: camera
  X reversed in fixed views, door flicker with a black web, trouser flicker, torch point lights. Missing sounds: 0.3.
  Mist in water: **resolved offline (30 Oct)**: the mist is P5 GFX particles, which `pop_state_frame` only drew with
  `POP_NATIVE_P5=1` (live has them on by default). Now on by default offline too (`=0` off): 162819 9.28 -> 4.31 (the haze
  cloud is there), gate `vu1` lines 8.28 / 8.20 / 2.62. Live the +0x88 particles are on the GS unless `-NativeLate`.

## Light shafts: FUN_00469C00 is the HUD; the shafts are glow-mask meshes, already native (30 Oct, Mac, offline)

- **FUN_00469C00 (list type 0x18) is the Jade STR text renderer**, not light shafts (decompiled 30 Oct:
  `STR_pst_GetFontDesc`, `STR_ComputeFrame`, `STR_Effect_TreatAll`; per font page FUN_004B2000 sets the render state,
  then a CNT + DIRECT + GIF tag `0x502F4...` (PRIM 0x5E: sprite, textured, alpha-blended; REGS ST RGBAQ XYZ2 ST XYZ2);
  letters by FUN_0046A980 / FUN_0046AA80 at screen coordinates, Z 0, into per-page buffers at 0x6CC430). Its one entry
  sits in list +0xB8 in 255 of the saves (`pop_state_frame` does not replay +0xB8). 185602: the life bar, sand tanks and
  tutorial text are exactly what the native frame lacks. So it is **0.14 (HUD)**; the 16 Oct "light-shaft texture"
  note was wrong. Natively it fits the P5 pattern (run the original, `ctx->pc == ra`, read the DMA range, rewind).
- **The light shafts' path:** 092046 ("rays of light", 17 Oct) has empty lists in the save (0 draws offline: no use).
  191059 (mirror cutscene): the beam is missing natively although every list entry is replayed (kinds 3, 4, 9, 10 in
  +0x2C..+0x98), and no replayed draw lands there. So either a draw outside the element lists, or an element drawn
  natively with nothing visible.
- **Resolved 30 Oct (Mac): the light shafts are glow-mask meshes, already native.** The "missing" beam came from
  rendering with `--noglow` (and from the 19 Oct score, before the native glow). Per-draw renders of 191059
  (`POP_VIEW_ONLY=i-i`) + each draw's projected screen box found 36 draws covering exactly the beam (x 128..415, y
  146..316) that change no colour: texture 2e0057e8 (UV-animated subs, flags c001405f / c020040f), one pass with FBMSK
  ffffffff, the +0x98 pass with FBMSK 00ffffff (alpha only: the glow mask). ColorDiffusion blooms that mask into the
  white shaft. With the glow on, 191059 scores 5.26 against the game (beam shape and brightness match; small
  differences inside it and at its edges). So light shafts need no new draw path: live they are native once +0x88 /
  +0x98 and the glow are (`-NativeLate`); judge in motion there. 155110's floor patch and the moon blob (ColorDiffusion
  over-brightening, above) are the same mechanism, so the GS check of the glow's tap FIX matters for shafts too.

## Flashes survey (30 Oct, Mac, offline; retail addresses, names from `game/proto/analysis/retail_names_from_proto.tsv`)

Flashes come from two places, neither drawn natively today (live they stay on the GS).

**1. Full-screen fades = the GFX module's Fade (type 0xB, GFX_i_Fade_Render 0x46FCC0) and TransitionFade (0x19,
0x46EC80)**, dispatched by GFX_Render 0x463F80 (called from FUN_003E2380, not from the element lists). Both call the
GDI rectangle pointer `*(0x674B00) + 0x18` = **FUN_004B4580**(x0, y0, x1, y1 in 0..1 of the display, colour, mode): an
untextured GS sprite, TEST 0x30000 (Z always), ZBUF masked, ALPHA 0x29 (mode 0; FIX 0, so Cs: opaque, decode worth a
second look) or 0x44 (mode 1: (Cs - Cd) * As + Cd). Fade lerps two colours over in / hold / out phases (FUN_00476700);
TransitionFade ramps the alpha (top byte, 0..0xFE) in, holds, ramps out. While the draw manager is in state 10
(`0x700000D8`), FUN_004B4580 does not draw: it stores the rectangle in the table 0x6DF6E0 (0x18 bytes: x0, y0, x1, y1,
colour, mode; 16 slots, count at +0x180) and queues a **kind 11** entry (type 0x2C..0x2F) in list **+0xB8**, which
CElementList::Render draws by calling FUN_004B4580 again. `pop_state_frame` neither replays +0xB8 nor kind 11.
Census over 231 saves: two have one. 191358 (save-point white-out) full screen `7fffffff` mode 1; 192114 full screen
`40000000` mode 1 (black at half). Natively this is one alpha-blended screen quad after the world, before the HUD;
it belongs with the HUD layer (+0xB8 is also where the STR text sits, see "Light shafts"). **+0xB8 is the 2D pass:**
PS2_After2D (0x4B4A00) -> FUN_004BBFC0 sets draw manager +0x28 = 1 and renders +0xB8, after the after-effects (glow,
SmartBlur). The native renderer has no layer after its post effects (its overlay replaces the picture), so native fades
need that 2D layer first (0.14 groundwork). Live, the GS draws +0xB8 over the injected native picture, so fades look
right in play today; a native fade only matters once the GS is off (0.2).

**2. CAfterEffect (0x6DF170; dispatcher FUN_004ABFC0, end of Display3DElements).** Retail keeps 9 of the prototype's
effects (flag byte.bit -> prototype name -> retail): +0x120.7 ZoomSmooth(+0x18, 0) and +0x121.0 ZoomSmooth(+0x1C, 1)
FUN_004AD900; +0x121.4 PinchEffect FUN_004AEA00; +0x121.5 ColorDiffusion 0x4AE100 (glow, native); +0x120.5
MotionSmooth; +0x120.0 an inline blend of buffer 1 over the frame, ALPHA (Cs - Cd) * FIX + Cd with FIX = +0x20 * 128
("FixBlend"); +0x122.2 Spiral FUN_004AC740; +0x121.7 SmartBlur 0x4A93C0 (native); +0x122.0 VisionEffect FUN_004A7980.
Gone in retail: BrightnessContrast, DepthBlur(Near), Blur, Remanance, RadialBlur, ClearBorderExtend,
ComputeWaterReflection. Census (231 saves): glow and SmartBlur always; **Pinch 8** (sand-haze saves 115837, 115853,
155110, 1029 185650 / 185807 / 185943, ...), **MotionSmooth 9**, **FixBlend 2** (24 Oct 200337 0.52, 19 Oct 185803
0.70); ZoomSmooth, Spiral, Vision in none. **Triggers (30 Oct, from the ELF):** all flags go through SetEffectStatus 0x4AD300 (effect id ->
bit: 1 -> +0x120.0 FixBlend, 4 -> .5 MotionSmooth, 5 -> .7 ZoomSmooth 0, 6 -> +0x121.0 ZoomSmooth 1, 0x11 -> .4 Pinch,
0x16 -> .7 SmartBlur, 0x18 -> +0x122.2 Spiral; no id sets +0x122.0), called by FUN_004AB440 (id, on) from the engine and
Gsp_AE_MASTER 0x4AB480 (id, on, param, value; via FUN_001C2DC0) from compiled AI code and from the event system
(FUN_001AC140, event function 0x3CF0, id from track data; reached from EVE_AddTime). Call sites with a constant id:
**ZoomSmooth 0 = Haste** (the late-game "power of haste", user 30 Oct: in a fight, freezes the nearby enemies and moves the Prince from one to the next; uses every power tank, only when power tanks = sand tanks; save while it runs): on in FUN_00485700 (the GFX type 0x1A object, 2 saves), strength ramped by
GFX_SOT_DoHasteEffect 0x485B40 (param 0 = 0.1 x time); **FixBlend = slow motion** (probably the Delay power) (GFX_SOT_DoSlowMotionEffect 0x485EC0,
FIX up to 0.7); MotionSmooth (id 4) only switched off in code; Pinch from the rewind, sand waves / rings, LocalFreeze,
Teleport_Pinch. **Spiral (0x18) is only ever switched off** (0x485674, 0x48661C; FUN_00486000 sets its params, never
its status), **ZoomSmooth 1 (id 6) has no call site**, **VisionEffect (+0x122.0) has no writer at all** (the
initialiser at 0x4B0CF0 clears bits 0-14, sets 15 = SmartBlur; the only +0x122 store is SetEffectStatus case 0x18):
dead in retail. Spiral and ZoomSmooth 1 could still come from event-track data (function 0x3CF0); not scanned, and
none in 231 saves, so both are parked as unused. MotionSmooth's switch-on is also data-driven (event tracks or AI). **The Pinch saves are the rewind saves** (155110, 115853, 1029 185650 / 185807 /
185943): Pinch is the rewind's warp. The sand visions are movies (39 `vis_*.int` in MOVIEPS2.BIG, `docs/movies.md`), not an
after-effect (triggers of ZoomSmooth / Spiral / VisionEffect: above). Not native:
ZoomSmooth 0 (Pinch, MotionSmooth, FixBlend: sections below). Pinch is a likely part of the "sand haze thicker on the GS" note (115853).
**Judged 30 Oct evening** (user's GS-only saves `game/analysis/saves1030/`, flag +0x120.7 read from each: 174127, 174131, 174137 on, 174148 off; `pop_state_frame_vu1 --noglow`): 6.16 / 7.45 / 1.33 (letterbox) vs 6.09 off; the side-by-side shows the same radial blur, the difference is the HUD (0.14). 174131 is a render-gate line.

**Light-shaft lead ruled out (30 Oct):** GFX type 0x12 GFX_Sun_Render 0x464C40 (GFX_NeedGeom, GFX_Sun_ComputeRay) is
not the beam. GFX_Render keeps its list head pointer at 0x674D04 (node: type byte +0, flags u16 +2, id +4, data +8,
next +0x14, lifetime +0x18). Over 229 saves the list holds only the HUD: Gao (type 0x11) x8, type 0x17 = FUN_00478CC0
(the HUD bars: GFX_ul_Bar_Geom*, screen quads through CAM_2Dto3D) x7, Interface x1; plus TransitionFade in 191358 (the
white-out) and type 0x1A (FUN_00485700, unnamed) in 2 saves. No Sun or Flare object in any save, 191059 included.
So the beam is an element-list draw (resolved below).

**Not done:** survey of other projects (`docs/ps2_projects.md`) for these effects; the PC capture of 191059.
Suggested order: fades (tiny, but needs the 2D layer: do with 0.14's HUD or as a single quad now), Pinch (8 saves,
sand areas), MotionSmooth / FixBlend, then ZoomSmooth 0 (the Haste power; Spiral, ZoomSmooth 1 and Vision are unused, see the triggers above).

## Native Pinch (30 Oct, offline; not yet live-checked)

**Live (eve1030k, 30 Oct 18:42, `-NativeLate` build d5bf53c; read 31 Oct):** 115837 pinch 488 calls, **478 native**; 115853
3 calls, 0 native, no why-line (the +0x74 / grid refusal never fired), all within the first stats window: the save's
rewind ends right after the load, so its few calls fall in the first frames, before the frame is open / pure (115837's
10 GS calls likely the same). So Pinch is native live; 115853 cannot test it. GS / native shots
(`game/analysis/shots1030k/`, separate boots): 115837 8.1, 145139 4.0-4.8, 202926 6.1-8.2, the differences only moving
things (fighters, flames, the fountain's column). Live, the rest of the post chain (glow, MotionSmooth, FixBlend,
SmartBlur, ZoomSmooth) is still the GS's, drawn over the injected picture (the world worker runs no `recordPost`).

`pinchRecord` / `pinchEffect` in `pop_native_live.cpp` (hook on FUN_004AEA00, `POP_NATIVE_PINCH=0` off; live only while the
frame is pure, as the halos, so with `-NativeLate`): the game's N x N grid of triangle strips (formula in the code comment;
`pow` = FUN_00134980, a wrapper of __ieee754_pow) through `gifToBatch`, reading a fresh frame copy (PNF image -5,
`frame_loader`); Z any value inside the view (no depth test). `pop_state_frame` calls it between the halos and the glow
when +0x121 bit 4 is set. Offline, with the glow, against the game picture (Pinch off -> on): 115837 4.07 -> 2.91,
115853 28.01 -> 25.20, 155110 29.67 -> 28.76, 1029 185650 32.32 -> 30.63, 185807 32.98 -> 32.27, 185943 35.79 -> 33.98
(the high ones are an even brightness offset over the whole frame, the sand haze, not the warp).
**Rewind look resolved (30 Oct, Mac, offline):** during the rewind the game switches the glow to **mask 0, FIX 36-45
(ramping), strength 0** (normal frames: mask 1, FIX 128, strength 4): the whole frame glows, which is the hazy rewind
picture. Ours was ~1.4x too strong because `postfx.h` / `color_diffusion.comp` worked in floats; the GS truncates every
blit (each convolution tap `texel * 15 >> 7`, each bilinear halving, each `Cs * FIX >> 7`), ~6% per level at these
dark values, compounded over 5 levels. With the truncation, using the game's own FIX: 115853 25.20 -> 7.94, 155110
28.76 -> 10.72, 1029 185650 30.63 -> 9.64, 185807 32.27 -> 13.28, 185943 33.98 -> 11.12. Every glow gate line got better
(145139 the moon 10.76 -> 6.12: the glow-only moon blob was the same missing truncation). GPU shader = CPU reference
(`pop_postfx_gpu_judge diffusion`, 6/6 exact). **Edges (30 Oct, later):** ApplyConvolution's taps (FUN_004AF340, t2/t3 =
+-0x10) offset the destination XY, not the UVs: each neighbour tap is the level drawn one pixel off, so a level's edge
pixels get no tap from outside (edge 6/9 of the weight, corner 4/9); ours clamped (9/9), so the rewind's whole-frame glow
was too bright towards the borders. Fixed in `postfx.h` and `color_diffusion.comp` (GPU = CPU, 12/12): 115853 7.94 -> 7.10,
155110 10.72 -> 7.09, 185650 9.64 -> 4.83, 185807 13.28 -> 8.44, 185943 11.12 -> 5.08; gate (masked glow) unchanged.
Not modelled: the pixels the shifted taps write just outside the level's rectangle (into the neighbouring rectangle of
the shared buffer). **Shadows missing offline = a replay limit, not a renderer bug:** `pop_state_frame` replays DisplayAllShadows only for
projectors whose caster / receiver lists are filled, and a save taken after the frame's shadow pass has every count 0
(FUN_004B8080 zeroes the count after drawing; the rewind saves and the gate's 184926 alike; 1022 192521 was caught
before it, so it has shadows). The entries stay in memory, but stale entries of earlier frames follow them, so the
count can't be recovered (dump of the list memory, 30 Oct). Live shadows are native and user-checked (24 Oct).
So no offline score includes shadows. **Edge strip: not a glow defect (30 Oct, Mac, from the code):** the frame's last
column / row is brighter in the scene itself (185807 x 639: 76 vs 62 at x 638 with `--noglow`, the GS picture too), and
native's excess there is the same ratio as beside it (x 638 / 639: 4.2% / 5.0%; y 446 / 447: 10.4% / 11.3%). The
layout, for the record (prototype ColorDiffusion `0046f680.c`, RectBlitter `00471d40.c` = retail FUN_004AF340): every
level sits at x 0 of the shared buffer, level 0 (320x224) at y 0; level n's halved copy C(n) at y(V(n-1)) + h(n-1) + h(n),
convolved into V(n) = the h(n) rows just above it. RectBlitter: CLAMP_1 REGION_CLAMP = the source rect exactly (our
GS clamps both bilinear taps, so nothing is read past it), SCISSOR_1 = dest x..x+w, y..y+h (inclusive: one pixel
more; the +1 convolution taps write C(n)'s top row while later taps read it, ~2% of a bottom row into the level's top
rows: not modelled), XY -0.5 px on both corners, UVs without inset (= our centre-to-centre `resample`). **Vertical
ramp (185807, native -3 at the top to +12 at the bottom) = the missing shadows (30 Oct, Mac):** only 185807 has it
(185650 / 185943, same room: no trend, within +-5 per 32-row band). The unglowed native floor is the same in all three
(104-105) and the native wall matches the GS (ratio 1.005), but the GS floor is darker in 185807 only (lower-left
199 vs native 217, under the characters 115 vs 129): a GS / native ratio image shows the characters' hard shadows plus
a large soft patch with diagonal streaks on the lower-left floor (a projected shadow), all on the floor, which fills
the bottom of the frame. `POP_NATIVE_SHADOWS=1` changes nothing (8.43: no filled projector in the save, see above).
Not a renderer bug; judge it live or on a save caught before the shadow pass. Not checked live (the glow runs natively only with
`-NativeLate` purity). Unrelated, seen in the same run: 1029 190506 48.57 and 195158 56.73 (not rewind saves): **empty draw lists in the save** (0 draws offline, saved after the game cleared them), cannot be judged offline; nothing broken (30 Oct). The warp is smooth,
content pulled towards the centre. Gate: `20261025_115837.gs 8.59 vu1` (8.49 with, 9.26 without). Live counter:
`[pop:world] pinch: calls, native`. Blended Pinch (+0x74 != 0xFF) native since 1 Nov (ALPHA 1 with TCC 0 = InvBlend, As = +0x74 >> 1; material `rgbTexture`, shader bit 1024); no save has one, not yet live-checked.

## Native MotionSmooth and FixBlend (30 Oct, Mac, offline)

From the disassembly (the decompile drops the ALPHA FIX and the vertex colours on the stack). Blit helpers: FUN_004AFFC0(this,
source, target, FBMSK, bilinear, TCC, ...) with buffers **0 = the frame** (FetchSystemDrawEnv(true)), **1 = the other draw
buffer = the previous displayed frame** (FetchSystemDrawEnv(false), 0x4A4800 picks +0xC30 / +0xDA0 by +0xBCC bit 0), **2 = a
frame-sized temp** (env +0x10), 3 = untextured; FUN_004AF940 draws the full rect at an integer pixel offset (a1, a2) with UVs on
texel centres (a pure shift), the colours of the two vertex rows at sp+0 / sp+8.
- **MotionSmooth 0x4ADD80** (+0x120 bit 5; args = the camera's turn since the last frame: the previous forward 0x6DF150
  through the current camera +0x1B0, x * 0.2 * width, y * 0.25 * height): n = halvings of max(|dx|, |dy|) >> 1 until 0, at most
  6. Clear the temp (colour 0, ALPHA 0x29); per pass two copies at colour 0x40 (half), shifted by -(d >> 1) (ALPHA 0x29: Cs)
  and +(d >> 1) (ALPHA 0x68 FIX 0x80: Cs + Cd), d halving, frame -> temp -> frame; an odd n copies the temp back. So an
  iterated 2-tap blur along the motion; **n = 0 below 2 pixels: nothing visible**.
- **FixBlend** (inline in FUN_004ABFC0, +0x120 bit 0): the previous frame over this one, colour 0x80, ALPHA 0x64
  ((Cs - Cd) * FIX >> 7 + Cd), FIX = min(+0x20 * 128, 128): an afterimage. The previous frame includes the HUD.
- Natively: `pop_postfx::motionSmoothBlits` / `fixBlendBlit` / `runBlits` (`postfx.h`, CPU), the same blits in
  `Renderer::recordPost` after the glow, before the haze (`gs_sprite.comp` flag bit 2 = add; temp in postSnap; the last
  finished frame kept in postPrev, no HUD in ours). GPU = CPU exactly (`pop_postfx_gpu_judge sprite`, 3 new cases). Frame
  trailer `AFTR` (flags, dx, dy, FIX) after HAZE: MotionSmooth hooked at 0x4ADD80 (records, the original still runs), FixBlend
  read in the ColorDiffusion hook. Viewer: `--prev <png>` = FixBlend's previous frame offline; `score_saves.sh` passes the
  save's displayed picture.
- Offline: MotionSmooth is always dx = dy = 0 in a save (0x6DF150 already holds the current camera), so it cannot be scored;
  the 9 saves are unchanged. FixBlend: 1019 185803 (FIX 89) 8.93 -> 2.72, 1024 200337 (FIX 66) 5.25 -> 3.14, but **circular**:
  the previous frame offline is the reference picture itself. No GS-only save has either, so no gate line. Live the GS still
  draws both over the injected picture (the world frame sends no post effects), so they matter for the viewer and 0.2.
  Not modelled: the alpha these blits write (FBMSK 0 in the game; nothing after them reads it), the half-width mode.

## Native ZoomSmooth 0 (power of haste; 30 Oct, Mac, offline)

From the disassembly of 0x4AD900 (recompiled listing `game/recomp/output/FUN_004ad900_0x4ad900.cpp`): (this, mode a1,
strength f12). Called first in the dispatcher FUN_004ABFC0 (ZoomSmooth 0, ZoomSmooth 1, Pinch, **then** the glow,
MotionSmooth, FixBlend, Spiral, SmartBlur). FUN_004AF940's floats: f12..f15 = the source UV rect as fractions of the frame
(times +0xE0 / +0xE4, truncated to texels), f16 = a rotation angle (sin/cos of the corners; mode 1 only).
- **Mode 0** (+0x120 bit 7, strength +0x18): s = p * 0.25 / 2; TEST 0x30000; four passes frame -> temp -> frame -> temp ->
  frame (FUN_004AFFC0 bilinear, FBMSK 0xFF000000), each: the source full-rect at colour 0x40 (ALPHA 0x29), then UV rect
  [s, 1 - s] stretched over the whole frame at 0x40 (ALPHA 0x68 FIX 0x80: add); s halves per pass. A radial zoom blur, brightness kept.
- **Mode 1** (no caller): angle = p * pi/4 / 4, the same passes with the copy rotated by -angle and +angle. Not ported.
- Natively: `pop_postfx::zoomSmoothBlits` (`postfx.h`), run by `Renderer::recordPost` before the glow (the sprite loop is
  now a lambda shared with MotionSmooth / FixBlend) and by the viewer's CPU path. Frame trailer `ZOOM` (float strength, 0 = off)
  after `AFTR`, from a hook at 0x4AD900 (records mode 0, the original still draws on the GS). GPU = CPU exactly
  (`pop_postfx_gpu_judge sprite`, 2 new cases). `pop_state_frame` replays it when +0x120 bit 7 is set;
  `POP_STATE_ZOOMSMOOTH=<p>` forces it (no save has it). Forced p = 1 on 1024 184926: a strong centred zoom blur, looks plausible;
  **no GS reference yet**: needs a GS-only save while the power of haste runs (the live ramp's range of p is not known).


- **Progressive scan crash (1 Nov, parked by the user):** choosing progressive scan in the title menu (step 5 boot,
  `POP_NATIVE_2D=1`) -> black screen + looping garbage audio. Log `play_20261101_134251.log` ends with
  `guest-branch:missing-target` JALR from 0x16AA0C to 0x3F091440 (a float, not code; a0 = 0xC22DA2F1 also a float):
  a corrupt function pointer, game-side. Not yet known whether it happens on the GS-only build (`-Mode gs`) or on a
  real PS2; check that first.
