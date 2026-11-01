# 0.2: fully native graphics (GS off by default)

Started 1 Nov 2026. Goal (`docs/roadmap.md`): the emulated GS off by default, the native renderer draws and presents
every frame, judged frame by frame against the GS output of the same build (visually the same; residual differences
measured and explained).

## Steps
1. **List what still reaches the GS** (census; below). Then make each item native or show it never draws colour.
2. **Native presenter:** show the native picture without the GS, including the circuit merge from PMODE, DISPLAY1/2,
   DISPFB1/2 (`docs/native_renderer.md` "Display stage"; ~half a day, estimate; the ALP 255 case still unexplained).
3. **Stop feeding the GS** when it is off (skip the VU1 / GS work), kept as the reference renderer (`play.ps1` switch;
   a comparison mode that keeps feeding it, `docs/roadmap.md` 1.0 "Reference renderer toggle").
4. **Frame-by-frame judging:** native vs GS over many saves and screens; explain what differs. Marked "before 0.2":
   the native glow's moon blob (145139, `docs/native_bugs.md` audit step 4: check against a GS trace of the
   ApplyConvolution blits).
5. **Re-check the parked bugs with the GS off:** hybrid-frame bugs (`docs/native_bugs.md`, rule of 26 Oct), audit
   item 11, N28, the progressive-scan crash (GS-only first).

Not 0.2: performance (waits until fully native), the real-PS2 reference (N15, N17, N27).

## Step 1: GS census (1 Nov)
**Tool:** `POP_NATIVE_GS_CENSUS=<n>` (`tools/overrides/pop_native_world.cpp`, GS host-inject hook, patch 38: sees every
GIF packet). Counts vertex kicks per marker-to-marker interval by PRIM and FRAME fbp, zero-area sprites apart; every
600 intervals a summary line `[pop:census] intervals ...`, the first sighting of each signature `[pop:census] new:`, and
every n-th interval (1 = 3000) printed in draw order as runs with FBMSK and box (`[pop:census] interval after marker`).
An interval = frame N after its marker + frame N+1 before its own (the clear is there). The purity counters
(`[pop:world] late lists ... ended by`) see only hooked paths; the census sees everything.

**Runs:** two user sessions, `play.ps1 -Env 'POP_NATIVE_GS_CENSUS=1'` (new game ~5 min, `play_20261101_151449.log`;
rewind save 155110 ~5 min, `play_20261101_152147.log`), and a 45 s dump boot from 155110 (`game\jobs\census1101c.ps1`,
`census1101c.log`); copies in `game/analysis/gs_census1101/`.

**Result: a pure game frame draws no colour on the GS.** After the marker, every dumped interval of 155110 holds only:
- `15e` (textured sprites, full screen, both frame buffers) and `14e` (untextured, full screen) with FBMSK 0x00FFFFFF:
  **alpha only** (the +0x98 glow mask left on the GS, `worldOnGs` lets alpha-only +0x98 through; EraseAlphaBuffer).
- `006` (sprites 0,0-640,448, FBMSK 0): the next frame's clear, before its marker (replaced by the upload).

With the GS off these only matter if something reads the frame's alpha: the native post chain uses its own mask, and
the display merge uses PMODE's ALP (`docs/native_renderer.md` "Display stage"). Check MMOD when writing the presenter.

**What does draw colour on the GS: frames whose purity ended** (from then on the late lists go to the GS). Causes
over the 1 Nov sessions (~22,000 game frames; ~1 in 300):
- **Pinch, blended** (`+0x74 != 0xFF`): the `pinchRecord` shortcut, hit in play (session 1: 15 calls; the 13:46
  session 22). Native Pinch only does the opaque case. **Native since 1 Nov** (InvBlend, As = +0x74 >> 1, TCC 0 from FUN_004AFFC0's t1;
  opaque gate save 115837 unchanged at 8.04). Live 1 Nov (user, 155110): 1718 / 1721 native (the 3 at the load); the
  white flash at the rewind's start is in `-Mode gs` too (N29, not a bug).
- **Water (P6)**: 32 + 12 frames; draws not replayed natively (`water ... draws N, native M`; reason not logged yet).
- **Single calls** (session 2: 9 frames): mesh +0x2c (4), mesh +0x3c, mesh +0x88 (2), sprite +0x88, idx +0xb8;
  session 1: ref +0x2c (1). Need `POP_NATIVE_WORLD_WHY=1` lines.
The other "on the GS" counts (halos 62, P5 157, P9 60, skinned 6, 2D strings 85) may be consequences of an ended
frame rather than causes: to confirm with the why-lines.

**Follow-up boots (1 Nov, job `gsoff1101d`, logs in `game/analysis/gs_census1101/`):**
- **Cold boot 150 s** (intro movie, title menu, no input; census every 50): colour on the GS only from the 640x448 clear
  (`006`, FBMSK 0); otherwise `14e` alpha only and `05e` zero-area. Intro movie and title menu draw no colour on the GS.
  Image transfers (movie frames) are many: they matter for step 3 (stop feeding the GS), not for colour.
- **155110 / 162819, 45 s with why-lines:** pure 2098/2100 and 2398/2400; the two impure frames are the load itself
  (`07c` / `076` / `07d` / `05c` and every "skinned call on the GS" line sit in the first frames after the load, counters
  frozen after that). 155110's Pinch calls (3) also fall there, so the blended Pinch needs a live rewind check. No water
  drawn in 162819's view.
- **Water session (user, cold boot + own memory-card save, `play_20261101_160253.log`):** water looks right to the user;
  draws 9101, native 8984, not replayed 0; frames 13200, pure 12797, **ended by water +0x88 206**. The 117 non-native
  water draws were the silent `b.indices.empty()` path (all triangles clipped: the GS got an empty colour pass that still
  closed the purity). Fixed: skipped as native, as particles already were. A 45 fps dip seen once: performance, later.
  Re-check (build 4e7a046, user, `play_20261101_160837.log`, looks right): water 9294 / 9294 native; frames 6300, ended by
  ref +0x2c 1 only (the rest not pure at +0x88 = menu / load frames, "ended within 0").
So the steady game, title and movie are covered; the open fallbacks are the ones only seen in play (blended Pinch, now
native; water; single calls).

**Not yet covered (older note):** non-game screens (title menu, movie, loading screen, load transitions such as marker 832 of
session 1: `046` 684 kicks, `15e`/`14e` on fbp 0x118) were seen only as first sightings, not dumped. Next: a cold-boot
dump (`POP_NATIVE_GS_CENSUS=50`, intro movie + title menu) and a why-line boot over the fallback saves.

## Step 2: native presenter (1 Nov; built and booted, user check pending)
**Switch:** `POP_NATIVE_PRESENT=1` (off by default). Runtime patch 45 (`GSHostPresent`, `gs_host_inject.h`): at each
presentation a hook gets the display registers and may return its own frame; the backend's Present is then skipped and
the window loop does not take the GS's shared GPU texture. Override (`pop_native_world.cpp`): each native picture also goes
into a private 4 MB GS memory (a `GSCpuBackend`, same buffer as the frame's target fbp), and the hook runs the runtime's
own CPU display merge (`PresentFromLocalMemory`: PMODE ALP, DISPFB1/2, DISPLAY1/2) on it. So the line blend is the GS's
formula exactly, ALP 255 included. MMOD is 1 (fixed ALP: PMODE 0x7f23), so the GS's alpha-only draws after the marker
change nothing on screen. Until the first native picture the GS's picture is shown.
**Known limits (review 1 Nov, deliberate for step 2):**
- A frame with no native picture ("no native picture" log line; normally 0) repeats the picture from two frames
  earlier in that buffer (a stutter), where the GS showed the frame without its static world.
- Frames whose purity ended (~1 in 300, mostly load frames) lose the colour the GS drew over the native picture. Step 4
  measures this; step 3 must make those frames native or accept them.
- The merge runs on the CPU every refresh (4 MB copy + 640x448 merge on the window thread; ~1-3 ms, estimate). A GPU line
  blend later (performance waits until fully native).
**Boots 1 Nov** (patch 45 applied on the PC; jobs `game\jobs\gsoff1101e/f/g.ps1`, shots `game/analysis/gsoff1101e/f/g/`;
A = play.ps1 defaults, B = + POP_NATIVE_PRESENT=1): no crash, fps and native counters unchanged (0 missed, 0 unexpected).
- Save menu 192114 (static): B vs A mean 0.5-1.3 / 255, < 0.1% of pixels off by more than 8.
- Fight 185602 (HUD, tutorial text) and the cold-boot intro movie: right by eye (the runs are not in step, so no pixel score).
- Save 192103 (loading screen kept in the save, not redrawn): B first showed the title's last "Loading options" box (our
  memory does not come back with a save state). Fixed (221548a): the GS's picture is shown when no native picture came for
  0.5 s; rerun: identical to A (0.00). The movie after it: the same movie frames in A and B (0.0 apart), shown a few
  fields apart (movie playback follows the clock; not a presenter error).
**Next:** the user's check on the TV: `play.ps1 -Env 'POP_NATIVE_PRESENT=1'` (title menu, a movie, a loading screen, play,
a quick-save load), then step 3.
