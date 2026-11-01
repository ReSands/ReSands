# Playthrough saves and bug reports, 20-21 Oct 2026

The user played about two more hours on the PC's current play build (default: GS emulation, scheduled VU1
code, 60 fps; the native world `POP_NATIVE_WORLD` was **not** on). Filed 22 Oct; nothing below is fixed yet
(user: write up only, fix later).

- **Saves** (PC `game\ps2\disc\states\`, copies on the Mac):
  - 20 Oct: 19 kept quick-saves `play_20261020_212329` to `215721` -> `game/analysis/play1020/states/`.
  - 21 Oct: 58 kept quick-saves `play_20261021_194825` to `210818`, plus the crash save
    `crash_20261021_210951.state` (it was `quick.state`, see crash 2) -> `game/analysis/play1021/states/`.
  - Copy script (resumable, per file): `game/analysis/play1021/fetch.sh`.
- **Play logs** (PC `game\recomp-sotc\play_<date>_<time>.log`) -> `game/analysis/play1021/logs/`.
  Frame rate per session (`[pop:fps]` samples, about one per second while playing):

  | Session | Samples | Mean fps | Samples below 55 | Min |
  |---|---|---|---|---|
  | 20 Oct 19:05 | 49 | 58.5 | 6% | 27.5 |
  | 20 Oct 19:12 | 32 | 57.9 | 9% | 28.0 |
  | 20 Oct 21:18 (ends in crash 1) | 277 | 55.5 | 29% | 30.0 |
  | 20 Oct 21:42 | 189 | 53.9 | 35% | 25.8 |
  | 21 Oct 19:39 (ends in crash 2) | 1076 | 58.3 | 10% | 30.0 |

- **Game pictures** of the saves named below (`tools/popstate.py --frame`): `game/analysis/play1021/frames/`.
  Native-vs-game scores of every save (`tools/native/score_saves.sh`): `game/analysis/play1020/native/` and
  `game/analysis/play1021/native/` (`scores.txt`; see the summary at the end).

## Bug 1: camera X reversed in fixed-camera views (closed 1 Nov, 21ab2c7)
User: the saves at 20:43 and 20:46 on 21 Oct (`play_20261021_204309`, `play_20261021_204628`) had the camera's X
axis reversed again, probably because the game was showing a fixed camera angle. The 20:43 frame is a high,
fixed view over a cliff ledge (`frames/play_20261021_204309.png`).

What the override does (`tools/overrides/pop_slus20743.cpp` `updateCameraInvert`): it inverts the right stick's
X everywhere except first person, which it detects as word +0x90 of the prince's camera block = 0 (block =
actor table 0x66D500 -> +0x18 -> +0x4 -> +0x44, + 0x5E4). Read from the saves (22 Oct):

| Save | +0x90 (1 = "normal camera") | +0x94 / +0x98 / +0x9C |
|---|---|---|
| 204133 | 0 | 0xbdee5e6c 0xbf7e4296 0x80000000 |
| **204309** (reported) | 1 | 0 0 0 |
| 204457 | 1 | 0 0 0 |
| 204516 | 1 | 0 0 0 |
| **204628** (reported) | 1 | 0 0 0 |
| 204813 | 1 | 0xbf42a4bd 0xbf26485a 0x80000000 |

So the override inverted X in both reported saves, treating the fixed camera like the orbiting one. In a fixed
camera the right stick does not orbit, so the game's own direction is probably right there. Candidate signal
(unverified, from these six saves only): +0x94..+0x9C are zero in the fixed-camera saves and non-zero around
them; it looks like a direction vector that only the orbiting camera fills in. To check against more fixed and
orbiting saves before relying on it; the proper fix is to find the camera mode the game itself switches on.
Also 204133 has +0x90 = 0 without being first person as far as we know (no picture checked): look at it too.

**1 Nov data point (user: X reversed again in `play_20261031_112025`, the N18 rubble courtyard, Prince on a ledge
seen from high above; copy `game/analysis/saves1031/`):** +0x90 = 1, +0x94 / +0x98 / +0x9C = 0x3cbff282 0xbf7fee03
0x80000000, i.e. **non-zero**, and the user confirms **this view is fixed** (the stick does not orbit): the 22 Oct
candidate signal (zero = fixed camera) is refuted. Same session's `play_20261031_111155`: +0x90 = 0, again not known to be first
person. So +0x90 / +0x94 are not the camera mode; the real fix is still to find the mode the game switches on.

**Fix (1 Nov, 21ab2c7, override `updateCameraInvert`):** the mode is the camera's current zone type. Camera
object = world view 0's object (`*(*(*0x674840 + 0x30) + 0xA0)`, as `AI_EvalFunc_VIEWGetObject_C`); its data
(+0x18 -> +0x4 -> +0x44) + 0x410 = current zone object; zone data + 0x1B4 = type (`FUN_002ebd00`; read by
`Mdl00005261_fn_GetZoneBehavior`, and `Mdl0000263C_fnf_GetFreeCamSightOffset` acts only on type 4). Saves: 204309,
204628 and 112025 (all reported fixed) = **2**; 204133, 204457, 204516, 204813, 111155 = 4; over all 292 saves:
244 x 4, 43 x 2, 5 unreadable (type 3 never seen). X is now inverted only in type-4 zones (and not in first
person); unreadable keeps inverting. **Closed 1 Nov (user check on the PC play build): the three reported fixed views
(204309, 204628, 112025) now pan the game's way; three normal-camera controls (1017 091049, 1024 201248, 1029
185807) still inverted as before.**
Type 3 (not seen in play): `GetZoneBehavior` gives it `Cam_Camera_bhv_mode_zone_circular` (type 4 = free-cam mode
0x31CE40, others = default 0x37FFC0); it gets the game's direction like type 2. How the stick reaches the camera
modes is not traced. If X ever feels wrong in a circular-camera spot, quick-save there and check its zone type
(user, 1 Nov: no zone-change logging for now).

## Bug 2: door flickering with a black web over it (20:50, 21 Oct)
Save `play_20261021_205013`. The game's own picture from the save (`frames/play_20261021_205013.png`) shows
a large wooden door with a dark, cross-shaped web pattern over its texture, so the emulated GS output has it
(not a native-renderer artefact). The user saw it flicker. The native frame of the same save
(`game/analysis/play1021/native/play_20261021_205013.side.png`: native | game | difference) draws the door
clean, without the web, and scores close overall (2.96). So either the GS emulation draws something wrong there,
or the web is a real pass (shadow, decal) that the native renderer does not draw yet. Guess, unverified: two
coplanar layers fighting over depth (the flicker), or a texture/CLUT upload problem. Next: the same door in
PCSX2 or the prototype (is the web real?), then a GS trace of that frame (`PS2X_GS_TRACE`) to find the draw.

## Crash 1 (20 Oct, during combat, ~21:41): a resume at a return address with no entry point
Log `play_20261020_2118.log`, last lines: fps dips (46-52, a fight), then
`[guest-branch:missing-target] kind=DirectJump op=EE scheduler source=0x172844 target=0x172844 ... ra=0x172844`,
then the user's quick-save (`State saved (field 79459)`) and nothing more: the game froze (the "kept as" copy
is written by the game's next frame, which never came). **That save is lost**: it was only in `quick.state`,
which the next session (21:42) overwrote. The nearest kept save is `play_20261020_213553` (6 minutes earlier).

Analysis so far (22 Oct, from the ELF and `functions.csv`): 0x172844 is the return address of
`jal 0x160D40` at 0x17283C. Ghidra ends FUN_00172800 at 0x172840 and makes its epilogue (0x172840-0x172854) a
separate function FUN_00172840 that starts at the jal's delay slot, so there is no entry at 0x172844. When the
scheduler unwinds a thread inside FUN_00160D40 (see CLAUDE.md "Override rules": it can unwind mid-call and
resume later) and resumes at the return address, the target is missing and the thread stops. Likely fix (not
done): correct the function boundary in the Ghidra export (FUN_00172800 to 0x172854) or make the recompiler
add an entry at every call's return address; then look for other functions split the same way. Survey other
PS2 recomp projects first (rule). Related: the nightly check's missing target 0x16AA0C in RPS2::UpdateBuffer.

**Seen again 24 Oct 20:24** (same log line, fight; save `play_20261024_202314.state` a minute before, full dump
`game/analysis/crash1024/crash.dmp` on the PC). **Fix built 24 Oct (3b80b66), in-game check pending:** the override
registers FUN_00172840 at 0x172844 too (its first instruction, 0x172840, is the jal's delay slot, a nop, so running
it from there is the resume). A scan of every function in `functions.csv` for a last instruction that is a call:
this is the only one in the ELF. **25 Oct:** save 202314 fought for 300 s with R1 held (50-56 fps), no
`missing-target` line, no freeze (one run; the 24 Oct crash came after ~16 minutes of play). Not a full fix for the class: the recompiler should add an entry at every call's
return address even when it lies in the next function (`docs/upstream_ps2recomp.md`).
**26 Oct:** the user's 25 Oct session `play_20261025_154156.log` (15:41-16:50, clean exit): no `missing-target`
line. Fighting was short: two low-fps stretches (~15:44-15:47 and ~16:03-16:06), a few minutes in all, against
crash 1's two hits at ~16 and ~23 min of play. Evidence for the fix, not proof.
**30 Oct evening: closed.** The user's fight-heavy session `play_20261030_174337.log` (~20 min, 70,500 frames, normal
`play.ps1`, clean exit): no `missing-target`, no error lines, fps avg 59.7. The class fix upstream stays open.

## Crash 2 (21 Oct 21:10, during combat)
Log `play_20261021_1939.log`: fps 49-58 (a fight), then a quick-save at field 320965 with no "kept as" line and
nothing after it, so again a freeze, but this time **no `missing-target` line** (cause unknown). The save is kept:
`crash_20261021_210951.state` (the PC's `quick.state` at 21:09:51). Its picture: a fight with sand creatures in a
room with cushions, one creature on fire, sand particles in the air (`frames/crash_20261021_210951.png`).
The save was taken after the freeze began, so loading it may freeze at once (a repro) or run on (the freeze then
depended on something not in the save). Next steps (not done): load it on the PC (`PS2X_STATE_LOAD_AT`), read the
guest threads' state from its ESCH section (which thread was running or blocked, at which pc), and check whether
it is the same split-function problem as crash 1.
**25 Oct:** the save's guest threads (`tools/popstate.py <state> --threads`) look like any healthy save: main thread
running (in `rand`, called from the water ripple update FUN_0040EC80 with sane values: 7 iterations left, dt 0.02),
threads 2/4 waiting on semaphores 10/70, thread 5 sleeping, thread 3 dormant; the same picture as save 115837.
Booted on the PC (native renderer on, no R1) it **runs on**: 85 s at 60 fps, no freeze. So the freeze depended on
something the save does not hold or a save-state boot does not run. The plain candidate is sound (save-state boots
have none; the freeze was in a fight with music), else host-side state (GS/VU threads). Next: a fight session with
sound on the current build; if it freezes, take a full dump (as crash1024) before closing the game.
**26 Oct: taken off the 0.1 list (user).** Seen once (21 Oct, ~90 min into a session); nothing has been built
against it, so a clean session cannot show it is fixed. Now a watch item through the 0.1 play-through: on a freeze,
take a full dump (as crash1024) before closing the game.
Ruled out (23 Oct): exhaustion of the runtime's callback-stack pool (the God Hand 30-minute crash, `docs/reference.md`
"God Hand"): the crash save uses 3 of its 22 stacks.

## Native-vs-game scores of the new saves (22 Oct)
`tools/native/score_saves.sh` (offline, Mac; full native frame vs the game's own picture; lower is closer): all 78
saves render without errors.
- 20 Oct: 19 saves, mean difference 15.7; worst 214607 (44.0), 212937 (38.6), 213443 (35.4), 213435 (32.6), 215529 (22.7).
- 21 Oct: 59 saves, mean 11.6; worst 202547 (38.6), 205558 (36.7), 202556 (35.9), 202307 (27.2), 210728 (25.8).
- The saves above: 204309 3.3, 204628 11.8, 205013 3.0, crash save 9.4.
The worst ones have not been looked at yet; they are the next inputs for the renderer's gap list (as the
17 Oct saves were in `docs/saves_1017.md`).
