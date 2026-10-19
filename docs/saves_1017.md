# Playthrough saves, 17 Oct 2026 (first ~25 min of play, 60 fps mode)

51 kept quick-saves `game\ps2\disc\states\play_20261017_<hhmmss>.state` (PC; copies on the Mac in
`game/analysis/play1017/states/`). Screenshots + fps of a 20 s boot of each (`tools/windows/state-shots.ps1`,
no sound in save-state boots): `game/analysis/play1017/shots/`, contact sheet `game/analysis/play1017/sheet/index.html`.
Play log: `game/analysis/play1017/play_20261017_0906.log`. Native frames of every save (offline, `pop_state_frame`):
`game/analysis/play1017/native/<#>_side.png` (game | native), scores in `scores_v3.txt`. # = order on the contact sheet. Reasons in quotes are the user's.

## Ranking for the native renderer (most useful first)

| Rank | # | Save | What | Why it matters |
|---|---|---|---|---|
| 1 | 50 | 093051 | "lots of sand enemies onscreen" | new enemy type, sand particles, sand-tank HUD; worst slowdown (3-18 fps), perf repro |
| 2 | 11 | 091049 | courtyard pool, "water effects" | water (not drawn natively yet); 38 fps |
| 3 | 22 | 091734 | healing pool + tutorial text | water close up, 2D text; 33 fps |
| 4 | 46 | 092450 | "rewind effect" | full-screen sand-power post effect |
| 5 | 40 | 092346 | "big glowing fire pits" | braziers: point lights, fire particles, glow |
| 6 | 28 | 091949 | "light from dagger of time" | glow from a light-emitting object in a dark room |
| 7 | 31 | 092046 | "rays of light" | light shafts, green fog; 30 fps |
| 8 | 4 | 090720 | "pause menu" | 2D/menus over the stormy sky (also #37 game over) |
| 9 | 49 | 093011 | "lots of NPCs in in-game cutscene" | many skinned characters at once |
| 10 | 9 | 091010 | "distance" | draw distance, fog, haze |
| 11 | 14 | 091230 | "fire" | fire sprites outdoors |
| 12 | 19 | 091533 | "dust particles dropping from ledge" | falling particles |
| 13 | 26 | 091848 | "prince interaction with curtains" | animated curtain mesh |
| 14 | 42 | 092413 | "small glowing candles" | many small lights |
| 15 | 43 | 092426 | "dagger of time" | dagger glow in a cutscene |
| 16 | 47 | 092542 | torch corridor, cobwebs | wall torches (point lights), alpha cobwebs |
| 17 | 38, 33 | 092300, 092130 | green vault | slow (14-18 fps), perf repro |
| 18 | 18 | 091448 | three guards in a corridor | fight with several skinned enemies |
| then | rest | | tutorials, ledges, letterboxed cutscenes (#1, 2, 17, 48) | coverage only |

Sound marker: #7 (090902) "that's when the audio started to glitch".

## Performance (17 Oct)

- Frame rate falls far below 60 in many scenes, and the drops reproduce from save states (boots without sound), so
  sound is not the cause: 60 in the opening; ~40 from #7; 30-38 in the courtyard pool (#10-11), the healing pool
  (#22-24) and dark interiors; 14-25 in parts of the vault (#33-38) and the dagger cutscenes (#43-47); **3-18 in
  the sand-monster fight** (#48-51; 3.6 fps in play).
- **Audio judder lines up with the slowdown:** it started at #7, the first drop below 60. The user also noticed it
  at the pool #10, which felt smooth but measured 31-40 fps. Working assumption: the sound is starved when a frame
  runs late, so fix the speed first, then recheck the sound.
- **Not the graphics workload** (Mac, from the saves' own last frames, `tools/statechain.py`): the 3-18 fps
  fight frames have no more draw-list entries, DMA chain size, VU1 runs, GS packets, render-target switches
  or transfers than scenes at 60 fps (e.g. 09:22:25 at 60 fps: 9,203 chain records, 6 MB, 4,764 VU1 runs;
  09:29:59 at 3 fps: 4,367, 5.2 MB, 3,132). So the time goes on the CPU side.
- Suspect (not proven): VU0 microprograms run on the cycle-modelling interpreter (`executeVU0Microprogram`
  -> `m_vu0.execute`), used for every object's visibility check (`CGraphicSystem::CheckBoundingBox`,
  `CheckFreeShapeBoundingBox`, `GSentBoundingBoxGroupToVu0`, `CJadeShadowProjector::CheckBoundingBox`) and
  a matrix-maths group at 0x495240-0x495C18: scales with animated characters.
- Profiler ready: `POP_PROFILE=<file>:<start s>:<seconds>` (tools/overrides/pop_profiler.cpp, Windows and
  macOS) + `tools/profsym.py`; the PC play build now links with /MAP. One PC job does it all:
  `tools/windows/start-job.ps1 -Script tools/windows/profile-states.ps1` (fast build, profiles #50, #11
  and a 60 fps reference, writes symbolized reports to game/analysis/prof/). Tested on the Mac.

## Profile result (18 Oct, PC, `tools/windows/profile-states.ps1`, reports in game/analysis/prof/)

- Fight #50 (2.3-16 fps) and pool #11 (29-42 fps): the busiest thread is the **VU1 thread**, mostly in the
  cycle-modelling **VU1 interpreter** (stepPair, execUpper, commitReadyPipelines, FMAC flag code); compiled
  VU1 programs (`fastBlock<hash,...>`) are a small share. 60 fps reference #40: the game thread, with a
  visible share in the same interpreter class running **VU0** microprograms.
- Why: compiled VU1 programs are found by the **whole 16 KB VU1 code image** (ps2_vu1_core.cpp,
  findCompiledVu1Image). PoP swaps microprograms per scene (ExchangeVu1Code), and the compiled set came from a
  12 Oct capture of the opening (9 images), so later scenes' program combinations fall back to the interpreter.
  (Correction to 17 Oct: the VU1 *workload* is not bigger in slow frames, but it runs on the slow path.)
- Fix in progress: `tools/windows/oct18.ps1` captures VU1 images from all 51 saves, compiles them
  (`rebuild.ps1 -Vu1`), then re-measures every save's fps and re-profiles. VU0 microprograms (36 call sites)
  are the next candidate (see docs/reference.md, CPU-side re-survey).

## First-person camera flag (18 Oct, found)

Problem: `PS2X_INVERT_RIGHT_STICK=x` (modern camera orbit) also reverses first-person look, which already
uses the modern direction. Fix idea: invert only when the game is not in first-person view. One
first-person save (play_20261018_202311, tutorial "First-person view ... press R2 to return") against 55
normal saves: no global (0x600000-0x700000) or scratchpad word is the mode (3 hits: two sound-callback
pointers, one -0.0 float). The flag is likely in a heap camera object (addresses move between levels).
Needed: paired saves at one spot (normal, first-person, normal, first-person within a minute), then diff all
of RAM among them.
Found the same evening from four paired saves (21:40:02/07 normal, 21:40:05/09 first-person): `*(0x672B04) + 0x90`
(object at 0xBE36B4) = 1 in the normal camera, 0 in first-person; agrees in all 51 saves of 17 Oct and every
save of 18 Oct (also 20:23:11 and 21:39:57, first-person). Fix: the override sets the runtime's default
right-stick inversion each frame (X inverted only in the normal camera); boot.ps1 no longer forces
PS2X_INVERT_RIGHT_STICK=x. POP_CAMERA_CLASSIC=1 keeps the original directions.
19 Oct: the user found first-person X wrong again. Cause: 0x672B04 is a cache that only
`Mdl0001DF8E_fn_GetPrinceHasEnemies` (0x325500) fills (`AI_ul_RealResolveOneRef(0x28, 0xCC000431)` = the prince,
then `AI_EvalKeyword_Ultra_C1(..)->0x44 + 0x5E4`), so it stays 0 until the first enemy: 16 of the 19 Oct
evening saves (a fresh start, 18:46-18:55) have it 0 while the flag itself toggles correctly (18:47:15/20/23 = 0).
Fix: the override goes through the main-actor table instead, 0x66D500 (gp-0x74F0, slot 0 = prince, written by
0x1B2580) -> +0x18 -> +0x4 -> +0x44 -> +0x5E4 + 0x90; equals the cache in all 111 PC saves that have one.
