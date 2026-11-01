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

## Step 2: native presenter (1 Nov; closed, user TV check passed)
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
**User TV check 1 Nov (passed):** cold boot, `play.ps1 -Env 'POP_NATIVE_PRESENT=1'`: intro movie, title, loading,
fights, platforming, rewind, time freeze, all right by eye. Log `play_20261101_160837.log`: 0 "no native picture",
6600 frames injected, 0 missed, 0 unexpected; ~60 fps outside boot/load dips. **Next: step 3.**

## Step 3: stop feeding the GS (1 Nov; closed, user TV check passed)
**Switch:** `POP_GS_OFF=1` (`play.ps1 -GsOff`; off by default until step 4), implies `POP_NATIVE_PRESENT=1`. Runtime
patch 46 (`GSHostInject::setDrawsOff`): the GS frontend still runs every register write, image transfer,
FINISH/SIGNAL and its display bookkeeping, but no draw reaches the raster backend. Chosen over consuming whole GIF
packets in the hook (that would drop FINISH/SIGNAL the game syncs on, and transfers) and over skipping the VU1/VIF1
work (in a pure frame that is only driver switches and the alpha-only glow mask: performance, which waits).
- The presenter's fallback (no native picture for 0.5 s: cold boot before the first marker, save-state loads) shows
  the GS's memory, which still gets transfers and loaded states but no draws. 192103's kept loading screen should
  still show (it is in the loaded memory).
- Accepted for now: frames whose purity ended (~1 in 300, mostly load frames; rare single calls) lose what the GS
  drew; step 4 measures them.
- Not checked: no known GS -> host readback in the game; a boot will show.
Build note: the PC build compiles the runtime from its own clone `game\recomp-sotc\runtime-src`; after `git am` in
`C:\tools\PS2Recomp-sotc` check its HEAD out there too (the first `-Fast` build missed patch 46 for that reason).
**User TV check 1 Nov (passed):** cold boot, `play.ps1 -GsOff`: intro, title, loading, fights, rewind; log
`play_20261101_165944.log` (copy in `game/analysis/gsoff1101h/`): "GS off" line present, 10,200 frames injected,
0 missed, 0 unexpected; late lists 10,500 frames, GS calls 0; 2D on the GS 0. One report: the slowed enemy drawn white
(N30, `docs/native_bugs.md`): not GS-off related (offline native and `-Mode gs` boots of the same save match it;
census found no GS colour over the enemy); parked until the real-PS2 reference.
**Next: step 4** (frame-by-frame judging, native vs GS). Default still GS fed: flip `play.ps1` to `-GsOff` by default
when step 4 / step 5 are through (0.2's goal).

## Step 4: frame-by-frame judging (1 Nov, first run)
**Job** `tools/windows/gsoff-ab.ps1` (run as `game\jobs\gsoff1101i.ps1`, 11 min, TV on): cold boot + the 13 GS-only gate
saves + 185602 (fight HUD), 192114 (save menu), 192103 (loading / movie); A = every native switch off (`-Mode gs`),
B = play.ps1 defaults + `POP_GS_OFF=1`; shots 1, 4, 10 s after the load (field = the save's vsync counter, `popstate.py`
GS regs, + 60/240/600). Scores and side-by-sides: `tools/native/score_ab.sh game/analysis/gsoff1101i`.
- **Clean:** no crash, 0 missed, 0 unexpected buffers, 0 "no native picture"; impure frames only the 1-4 at each load
  (counters frozen after). fps as before (fights 47-60).
- **Scenery matches** wherever the view holds still (201540 1.4-1.8, 162621 2.4-3.8, 125052 2.3, 174131 2.7, 145139
  3.1-4.4, the diff only edges / dither speckle). **145139 moon blob: gone** (moon region 224.9 native vs 222.8 GS);
  the torch light by the wall flickers (native 169 / 160 / 162 vs GS 156 / 157 / 164 over the three shots: animation).
  162246's halos: equal at 1 s, native ~10% brighter on the left one at 4 / 10 s (flicker or N23/N26; not judged).
- **192114 save menu (2.5-2.8):** the GS draws faint garbled stripes (text-like ghost rows) into the box under its top
  and bottom edges; native is clean. GS-side artefact (our emulation is not ground truth), not a native bug.
- **Movies / cold boot:** same frames, a few fields apart (movie playback follows the clock; as in step 2).
- **Not judged by this method: fights and effects.** Two boots do not replay the same: by the 1 s shot the fighters are
  already elsewhere (184926 14.8, 134636 13.7, 115837 6-23, 185602 9-15), by 10 s the camera too (115853). A same-frame
  comparison is needed for those (next).

## Step 4b plan: same-frame captures for fights and effects (user 1 Nov: option A; if not useful, build the comparison mode early)
Goal: native vs our GS on **the same frame**, mid-fight and mid-effect, which two separate boots cannot give. Each
capture also keeps that frame's GS trace for the real-PS2 replay later (`docs/roadmap.md` "Hardware reference").
1. **Capture route (decide first, offline where possible).** Preferred: GS-only saves taken mid-fight, scored by the
   proven render-gate route (`tools/native/score_saves.sh`: `pop_state_frame` vs the save's own picture; same frame
   by construction). Make them in one `-Mode gs` boot per fight save (R1 held, `POP_INPUT=r1@1:3600`) with several
   saves a few seconds apart: `PS2X_STATE_SAVE_AT` (seen in the runtime, unverified: check its syntax in
   `C:\tools\PS2Recomp-sotc` first) or quick-saves by `select@<s>` presses as `game\jobs\gsref1028.ps1` does (it
   backs up / restores `quick.state`; each save is "kept as states/<name>"). Alternative if saves miss something:
   `tools/windows/native-scenes.ps1` (`POP_NATIVE_LIVE` + `POP_NATIVE_FRAME=<pnf>:<s>`, export by seconds from start,
   shots by field: find the export's field in its log / `.sub.txt` and pick the best-aligned shot).
2. **GS trace of the same frame:** `PS2X_GS_TRACE=<field>:4:<path>` (>= 4 fields; ~60 MB/s, so 4-8 fields per
   capture) in the same boot, at each save's field. Check the trace's field count lines up with the save's
   `State saved (field N)`. Keep under `game/analysis/gsoff_ab/` with the saves.
3. **Scenes** (fights and effects not judged in the first run): 184926, 134636, 115837, 115853, 202314, 185602 (fight
   HUD, slow-motion dagger), plus effects: rewind (155110), time freeze, the sand haze (115853), halos (162246),
   water (a save from the water session). ~3 saves each, 1, 4, 10 s into the fight. One job, TV on, ~15 min.
4. **Score and look:** `score_saves.sh` per capture set (STATE_FRAME=pop_state_frame_vu1 for sprites / P5 / P6 /
   halos), side-by-sides; every difference explained, fixed or filed as an N-bug. Known offline limits (not bugs):
   no HUD, shadows only if the save precedes the shadow pass (`docs/native_bugs.md` "Shadows missing offline").
5. **Judge the method:** useful = the offline frame shows what live play shows (cross-check one capture by eye
   against a `-GsOff` boot of the same save) and the scores separate real differences from noise. Not useful
   (offline replay misses live-only paths that matter, or alignment fails) -> build the comparison mode early
   (both renderers every frame, `docs/native_renderer.md` "Reference renderer toggle"; several days, estimate).
Then step 5 (parked bugs with the GS off), then flip `play.ps1` to `-GsOff` by default (0.2 done).

## Step 4b, first run (1 Nov)
**Job** `tools/windows/gsoff-capture.ps1` (2.7 min, TV on, no build): 9 scenes in `-Mode gs`, three Select quick-saves
each ~1 / 4 / 10 s after the load (R1 held between; fields e.g. 28508 / 28685 / 29026). 27 GS-only saves, no errors,
copies in `game/analysis/gsoffcap/` (+ boot logs). Scored with `score_saves.sh` (`score/`, `score_vu1/`; vu1 better or
equal everywhere). GS traces not taken (one timed trace per boot; can be taken from these saves later).
- **Effects and scenery: method works, native matches.** Halos 162246 1.6-1.7 (0.1% off), fountain water / light shaft
  202926 5.7-5.9 (0.6-0.8%), sand haze 115853 5.5-6.5 (2.6-4.4%), scenery behind every fight lines up; remaining
  diff = characters + offline limits (no projected shadows, no hint text in 185602).
- **Fights: method fails.** The characters are not on the same frame: offline poses come from the game's current
  matrices, ahead of the saved picture (134636_1, 115837_2, 185602_2, 155110_1: arms, swords and positions differ,
  scores 11-15). The save's other draw buffer (fbp 0x0, displayed 0x8c) is closer but still off (134636_1 13.6 -> 10.8;
  others +-1). So no same-frame verdict on character rendering or on fight-only effects (hit flashes).
- **Next (plan point 5):** the comparison mode (both renderers on the same live frame) as the fallback, or first a
  cheaper check: pose the offline characters from the chain's matrices instead of memory (unestimated). User decides.

## Step 4c plan: the offline frame = the save's last chain (user 1 Nov: next step; groundwork for the real-PS2 reference)
**Why.** A real PS2 can only judge single frames from saved data (`docs/roadmap.md` "Hardware reference": (a) GS
replay, (b) DMA chain replay). For that, every offline native frame must be **exactly the frame of one chain**, so
native, our GS and the real GS all draw the same input. Today the offline tool mixes sources: draw lists and pools from
memory, some packets from the chain, and fills gaps by guessing (stale halo entries). Rule for this step: **where the
save's memory and its last chain disagree, the chain wins**, and the reference picture is the buffer that chain drew.
Goal: as much of the renderer as possible scoreable on the same frame, offline now and against a real PS2 later.

Known gaps, in order of work (from step 4b and `docs/native_bugs.md`): (1) character poses ahead of the picture;
(2) projected shadows missing (count zeroed after the shadow pass); (3) menu / hint text not replayed; (4) halos
replayed from stale array entries (145139's offline-only moon halo); (5) offline glow over-brightens (`--noglow`);
(6) GFX particles of +0x88 (water mist) listed, never drawn (unproven). Live-only paths (GS hand-over) are out of scope.

**Starting points.** `tools/native/src/state_frame.cpp` already walks the save's last chain for particle packets
(`findParticlePackets`: chain start = `0x70000620` +0xE4 / +0xE8, the one +0xF4 is not building). `tools/chainskin.py`
already poses skinned draws from a chain: layout A bone palette at VU 0x388 (4 qw per bone, slot k -> bone k-1),
layout B object matrix at VU 0; row vectors. Live posing: `poseSkinned` in `tools/overrides/pop_native_live.cpp`
(bones from the pool at `*(0x700001D0)`, object matrices at `*(0x70000260)`). Saves: `game/analysis/gsoffcap/`
(27 GS-only, fights + effects; scores in `score/`, `score_vu1/`), gate `game/analysis/gsref1027/states/` (13).

1. **Measure first (Mac, offline; ~1 h).** For 3-4 fight saves (134636_1, 115837_2, 185602_2, 155110_1): dump the bone
   matrices each skinned CALL gets in the last chain (a `statechain.py` / `chainskin.py` mode) and the pool's matrices
   for the same draw. Questions: (a) do they differ (pool ahead) while the object matrices / camera agree (scenery
   lined up in 4b)? (b) which buffer did the chain draw: render `chainskin.py`'s posed glTF or just compare silhouettes
   against the displayed (fbp 0x8c) and the other (0x0) buffer. Write the answer down before changing code.
   **Answer (1 Nov, `tools/chainpose.py`, `popstate.py --frame --fbp 0`):** (a) yes. Object matrices (VU 0) of
   the chain sit bit-exact in the object pool (446/593 in 134636_1, 677/918 in 185602_2; the rest are camera / moving
   objects), bone palettes (VU 0x388+) almost never do (2/645 in 134636_1, 0/612 in 155110_1; 30-40 % on the calmer
   115837_2 / 184926_1): nearest pool bone 0.7-3.4 cm away (median). The chain's bone matrices exist nowhere else in
   RAM (only inside the chain buffer at 0x867000): **the chain is the only source of the drawn pose**, so point 2
   stands. (b) The chain draws the whole frame (scene, post, HUD) into **fbp 0x0** (fbw 10); its 0x8c writes are
   256-wide alpha-only scratch passes (fbw 4, mask 0xFFFFFF). The displayed 0x8c is the frame before: 0x0 vs 0x8c in
   134636_1 differ only in the characters (scenery and HUD identical; mean 6.0, 155110_1 2.2). So the reference
   picture for point 3 is `--fbp 0`, and `score_saves.sh`'s `--prev` (displayed = previous frame) stays right.
2. **Poses from the chain (A; time box half a day).** Offline only: `state_frame.cpp` walks the chain once, records per
   skinned CALL (packet address, in order) the matrices uploaded before it; `poseSkinned` gets an optional source
   (a hook the offline tool sets; live keeps the pool), matched by packet + order. Same for layout B object matrices if
   step 1 shows them ahead too. No change to live behaviour: build the live tool and run the
   render gate (13 GS-only saves must stay pixel-identical, as for N19).
   **Done (1 Nov):** `state_frame.cpp` walks the last chain once (`walkLastChain`, shared with the particle finder),
   keeps per CALL target the VU1 matrices in effect (VU 0, VU 0x388 + 4k) and sets `pop_native_live::g_chainBones`;
   `poseSkinned` (new `packet` argument from the element / skinned-ref call sites) hands it the pool's bones and takes
   the CALL of that packet whose bones lie nearest (instances metres apart, a frame of motion centimetres; over 0.5 m
   mean keeps the pool). Palette draws and rigid no-palette parts (held clubs) both. `POP_STATE_CHAIN=0` = old
   behaviour. Fight saves: 513 of 514 skinned draws posed from the chain (4 saves). Score against fbp 0 (pool -> chain):
   134636_1 10.81 -> 7.14, 155110_1 10.34 -> 9.52, 115837_2 13.79 -> 13.19, 185602_2 14.41 -> 13.99; characters line up,
   what is left is shadows, flashes and hint text (points 4-6). Open: one club in 134636_1 still ahead (an objectSpace
   lit pass: drawn with `poolMatrix(index)`, which has no packet to match by). Render gate: with `POP_STATE_CHAIN=0` the
   13 gate saves are pixel-identical to the build before (live path unchanged: the hook is null in the game); with it
   on, every one scores the same or better even against the old (displayed) reference. Live runtime not rebuilt (the
   change there is a null hook + a default argument; compiled into the offline tools on the Mac).
3. **Reference picture = the chain's buffer.** `score_saves.sh` gets the buffer step 1 found (a `popstate.py --frame`
   option for the other buffer if needed; the scratch test was `fbp 0x8c` displayed / `0x0` other). Re-score gsoffcap +
   gate. Done when fight saves score like scenery (target: 134636_1 well under its 13.6; character edges only).
   **Done (1 Nov):** every one of the 40 saves (27 gsoffcap + 13 gate) is plain double buffering: the last chain
   draws its colour into the buffer not displayed (0x0 <-> 0x8c; 0x118 = the glow buffer). `popstate.py --drawn` takes
   that one; `score_saves.sh` scores against it (`game.png`) and passes the displayed one (`shown.png`) as FixBlend's
   `--prev`. Re-scored (old = pool poses vs displayed, new = chain poses vs drawn; plain / vu1 build): 134636_1 13.76 ->
   7.28 / 13.64 -> 7.14 (target met), 184926_2 9.42 -> 6.10 / 8.42 -> 4.97, 202314_1 9.12 -> 6.16, 115853_2 vu1 6.45 ->
   5.09; 21 of 27 better in both builds. Worse: 115837_2 13.68 -> 14.41 / 12.59 -> 13.19 (the big flash, points 5-6),
   202926 slightly in vu1 (5.81 -> 5.87, fountain). Nightly gate (`tools/nightly/render_gate.txt`) re-based to the new
   reference (+ 0.1): most lines dropped (115837 9.53 -> 5.29, 134636 10.71 -> 7.65); 162246 plain rose 2.37 -> 2.43
   (the plain build lacks the VU1 effects, which move between the two frames; its vu1 score is 1.61). Scores in
   `game/analysis/gsoffcap/score/`, `score_vu1/`.
4. **Shadows from the chain.** The shadow pass's packets are in the chain even when the count is 0: draw them from there
   (or recover the projector list via the chain's order). Done when 184926 / rewind saves show shadows offline.
   **Done (1 Nov):** the list counts are recovered from the chain. The shadow pass sits in it per drawn projector: its
   caster-buffer FRAME (fbw 4, FBMSK 0xFFFFFF), a setup packet, the casters, then the receivers (the FRAME back to the
   frame); each list entry's packet (element +0x20 + 4 x pass / ref + 4 x pass) is a CALL target in list order, chained
   elements add CALLs between. `state_frame.cpp` (`readLastChain`, the same walk as the poses) keeps one CALL sequence
   per projector; `chainShadowCounts` matches a projector to the sequence holding its first caster and sets each count
   to the longest list prefix found there in order (receivers after the casters); a projector the chain did not draw
   gets 0. Checked on 192521 (lists still filled): all 6 drawn projectors' counts exact, the undrawn #6 (memory 6/4)
   0, which the old replay drew. All 40 saves: every sequence matched except one in 115837_1 / _2 (2 of 3).
   `score_saves.sh` now sets `POP_NATIVE_SHADOWS=1` (as `play.ps1` live). Scores (point 3 -> now, vu1 build): 184926_3
   5.70 -> 4.71, 115837_3 6.26 -> 3.93, 202314_3 2.81 -> 2.00, gate 202314 3.59 -> 2.22, 115837 3.90 -> 2.74; 26 of 27
   gsoffcap better in vu1 (185602_2 13.99 -> 14.07, the flash save). Plain build: 115853 worse (gate 10.66 -> 11.28; no
   dust cloud there, so the game's shadows under the dust widen the gap). Rewind saves: only 185807 has the GS shadow pass
   in its chain (the other two were taken with live native shadows): 10.19 -> 9.40; its vertical ramp half gone (64-row
   bands native - game 8.6 / 11.7 -> 7.9 / 7.3), the bottom band still +12.8 (glow? point 6). Gate re-based
   (`render_gate.txt`). Also fixed in the chain walk: DIRECT payloads are quadword aligned (point 2's walk skipped them
   unaligned; no pose changed: gate frames identical). `POP_CHAIN_POSES` renamed `POP_STATE_CHAIN` (0 = poses and shadow
   lists from the save's memory; gate pixel-identical to the build before step 4c). Not done: blob receivers (+0x20),
   mirror projectors, CJIdxMesh entries (none seen in these saves).
5. **Halos and text from the chain.** Halo draws (ALPHA 0x58 with FIX) and 2D text packets taken from the chain instead
   of the stale halo array / not at all; 145139's moon halo must disappear offline (the GS trace has none).
   **Done (1 Nov):** both from the chain, offline only (live hooks null). *Halos:* a drawn halo's texture pass is
   ALPHA 0x58 with FIX 0x80, a sprite in 32-pixel strips; `readLastChain` (now a full GIF register parse: A+D, PACKED
   XYZ2 / XYZF2, REGLIST) keeps each one's bounding box, and `haloRecord` asks `g_haloFilter` with its halo square
   (`ox0..oy1`): kept only if a chain square lies within 2 px, else dropped (stale entry or culled by the game). 145139:
   the moon halo gone (gate vu1 7.02 -> 4.33); 162621: the sun kept, 11 stale entries dropped; 184926 one kept. *Text:*
   each text slot goes out as a CNT record holding one GIF tag (PACKED, ST RGBA XYZ2 ST XYZ2 = 0x52512, nloop = letters)
   and the letters (0x50 bytes each). Offline `textBegin` works out the slots although the string is marked drawn, and
   each slot takes the next chain record with its letter count (`g_chainTextSlot`); the letters go through
   `letterRecord` (now takes the four ST floats; the live callers read them as before). The save's own slot cursors
   are no use: in 185602_2 they point into the next frame's buffer. 185602_1/2/3: the tutorial text back, in place
   (vu1 6.66 -> 5.41, 14.07 -> 12.56, 5.20 -> 3.98). Gate with `POP_STATE_CHAIN=0` (shadows off): pixel-identical to the
   build before step 4c. Open: 174131 has its HUD and text in the chain but list +0xB8 is empty in the save, so
   offline draws no 2D at all (the whole list would have to come from the chain); text inside 3D lists stays as before.
6. **Glow and mist** (smaller): why the offline glow over-brightens (live does not; compare the post inputs), and draw or
   rule out the +0x88 GFX particles.
7. **(Deferred, user 1 Nov: wait until the PS2 is in hand; how data gets on and the picture comes off decide the format.)**
   **Hand-over to the hardware reference.** Per save, one bundle: the chain (+ VU1 microcode / memory for (b)), the GS
   VRAM + registers before the frame (for (a): the GS packets come from running the chain through the VU1 interpreter in
   `pop_state_frame_vu1`, or a GS trace), the native frame, our GS frame. Write the bundle format into
   `docs/roadmap.md` "Hardware reference"; the PS2 player itself is a later job.
After each step: re-score, update this section, commit. Then back to step 5 of 0.2 (parked bugs with the GS off).
