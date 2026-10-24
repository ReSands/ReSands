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

## Reported 24 Oct 20:08-20:24 (play session from 19:52, log `play_20261024_195257.log.stderr`), not triaged yet

Saves on the PC in `game\ps2\disc\states\` (each with `.glitch` + `.glitch.txt`; fetch to `game/analysis/`):
- **N8 wall hue (20:08, save 200833):** a section of wall with a different hue from the rest of the wall.
- **N9 wooden door fully transparent (20:11, saves 201024 / 201029 / 201039).**
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

