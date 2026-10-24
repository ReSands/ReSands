# Native renderer bug list (started 24 Oct 2026)

Rendering bugs seen in play with the native renderer on. Plan (user, 24 Oct): no stop-the-world fixing; clear the
list at each milestone (first pass when 0.12 is done, before the 0.13 effects). Each entry: what was seen, evidence,
hypothesis (marked as such), status.

## Tooling first (agreed 24 Oct)

1. **Glitch capture key:** a rolling buffer of the last few seconds (final frames + per-frame renderer state: camera,
   which lists went native / where purity ended, fog zone, injected buffer), written on a key/pad combo. For one-frame
   glitches a save state cannot hold (N3, N4).
2. **Native conversion after a save-state load** (convert the elements already in RAM instead of waiting for
   ReadElement), so saves can be booted with the live renderer path and screenshotted with it on and off.

## N1: enemies slowed by the dagger do not turn sandy brown (24 Oct, play 19:23)

Long-press triangle (dagger of time) slows an enemy; on the PS2 it turns sandy brown. With the native renderer the
colour change is missing. Hypothesis: the effect changes something native characters ignore (material constant
colour, a light-set override or a second pass). Needs a save taken while an enemy is slowed (the effect lasts a few
seconds, so a quick-save can hold it; a save-state boot then shows it in `popstate.py --frame`). Priority: highest
of these (gameplay cue).

## N2: fog intensity blips when the camera turns (24 Oct)

Area fog jumps between intensities while rotating the camera. Saves 192947 / 193004 (`game/analysis/saves1024c/`)
do not show it (each save is one frame; offline A/B 11.97 / 6.50). In 193004 the bluish floor layer the native frame
lacks is the pool's water (CJSmWaterElement, P6 not native, always GS in game); the floor mist particles
(tex 2e004fe4, alpha ~0.05-0.075) are drawn and add ~+7 floor brightness. Hypothesis: lists +0x88/+0xA8 flip
between native and GS from frame to frame (the purity rule depends on what is on screen), and native and GS differ
for the fog-heavy draws (mist particles, fogged meshes). Test: run with `-NoNativeLate`; if the blip goes, it is
this. Needs the capture key otherwise.

## N3: one-frame flash walking up stairs, as if the environment mesh vanished (24 Oct, not captured)

Hypothesis: a frame whose world picture was not injected or came from a stale camera (the load flash family,
`docs/history.md` "Load flash (23 Oct)"). The session's counters were clean (play_20261024_192332.log: 25,200 frames
injected, 0 missed, 0 unexpected buffers), so not a missed injection; a stale-camera picture is not ruled out.

## N4: camera "teleported" to another part of the level during combat (24 Oct, not captured)

Seen once, for a moment. Hypothesis: a stale injected world picture from an earlier camera (as N3), or a real game
camera cut. Capture key needed.
