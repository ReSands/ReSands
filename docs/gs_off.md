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
  opaque gate save 115837 unchanged at 8.04); to confirm live at a rewind's start / end.
- **Water (P6)**: 32 + 12 frames; draws not replayed natively (`water ... draws N, native M`; reason not logged yet).
- **Single calls** (session 2: 9 frames): mesh +0x2c (4), mesh +0x3c, mesh +0x88 (2), sprite +0x88, idx +0xb8;
  session 1: ref +0x2c (1). Need `POP_NATIVE_WORLD_WHY=1` lines.
The other "on the GS" counts (halos 62, P5 157, P9 60, skinned 6, 2D strings 85) may be consequences of an ended
frame rather than causes: to confirm with the why-lines.

**Not yet covered:** non-game screens (title menu, movie, loading screen, load transitions such as marker 832 of
session 1: `046` 684 kicks, `15e`/`14e` on fbp 0x118) were seen only as first sightings, not dumped. Next: a cold-boot
dump (`POP_NATIVE_GS_CENSUS=50`, intro movie + title menu) and a why-line boot over the fallback saves.
