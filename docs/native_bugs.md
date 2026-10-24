# Native renderer bug list (started 24 Oct 2026)

Rendering bugs seen in play with the native renderer on. Plan (user, 24 Oct): no stop-the-world fixing; clear the
list at each milestone (first pass when 0.12 is done, before the 0.13 effects). Each entry: what was seen, evidence,
hypothesis (marked as such), status.

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

## N1: enemies slowed by the dagger do not turn sandy brown (24 Oct, play 19:23)

Long-press triangle (dagger of time) slows an enemy; on the PS2 it turns sandy brown. With the native renderer the
colour change is missing. Hypothesis: the effect changes something native characters ignore (material constant
colour, a light-set override or a second pass). Needs a save taken while an enemy is slowed (the effect lasts a few
seconds, so a quick-save can hold it; a save-state boot then shows it in `popstate.py --frame`). Priority: highest
of these (gameplay cue).

Second report (save 200418, 20:04, glitch capture too): the game picture shows the enemies pale grey. Offline
the save has empty draw lists (0 draws: saved after the game cleared them), so it cannot be judged offline. Next:
find the code that applies the freeze (dagger "slow" on an enemy) and what it changes per draw.

## N6: sustained frame-rate drop (save 200200, 20:02)

Glitch capture: 60 fps for 3 s, then 51-54 fps steadily (median frame 18.4 ms) while the frame's native draws rise
from ~1,050 to ~1,450. A heavier scene, not a hitch. Needs a `play.ps1 -Profile` session at that spot.

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

## N5: random one-frame flashes = black world frames when the kept meshes were dropped (fix built 24 Oct, in-game check pending)

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
All three bugs are in areas just entered. Fix (planned): in `readElement` erase every per-address cache entry for the
element (g_skinned, g_skipped, g_fromPacket and g_litBatches for passes 0-7). Check: needs live conversion after a
save-state load, or a play session through the same spots (201124's door: just after the staircase climb).

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

**C. N12 (sustained ~30 fps, 20:20-20:24) and N6: the frame falls back to the GS and the fallback costs double.**
Capture 202128: 16.7 ms per frame until frame ~360, then 27-33 ms from exactly the frame whose record changes to
`purity ended by shadow +0x5c, late native 0 GS 29-34` (draws 838 -> 1,138: enemies appearing); 202314 (the fight,
until the crash): 33-43 ms, the same reason throughout. N6's capture (200200): late lists on the GS all along
(`purity ended by ref +0x2c`), 18-19 ms. Two costs:
1. A shadow projector goes back to the GS when a receiver has no record: the log's `list entries without records`
   rises 6,559 -> 9,054 over the slow stretch (the enemies; known gaps: CJIdxMesh entries, blob receivers). Once the
   shadow is on the GS, every later list (+0x88/+0x98/+0xA8, particles, sprites) is too.
2. `spriteRecord` runs the P9 VU1 interpreter on every sprite draw **before** checking whether the frame can still
   be native: in the slow stretch `quads` rose by ~350-390k per log interval while `native` stayed at 45,316. The
   interpreter work is thrown away and the GS runs the same draw again. Fix: check `g_world && g_worldLate &&
   worldStateOk` (purity) first, interpret only then. Then profile 202314 to see what the rest is.

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

## Storage of captures (agreed with the user 24 Oct)

`.state` files (48 MB) are kept: the offline A/B test set. `.glitch` captures (129 MB) are temporary: after
triage, note the finding here and delete the capture once its bug is fixed and checked (or it showed nothing).
Planned: compress the capture pictures in the background thread when written (next rebuild).

