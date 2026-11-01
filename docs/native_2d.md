# 0.14: native 2D (HUD, menus, loading screens, movie display, fades)

Plan written 30 Oct 2026 (late), approved by the user with hooks instead of packet capture. Facts it builds on: CLAUDE.md "Next: 0.14",
`docs/native_bugs.md` "Light shafts" (STR text) and "Flashes survey" (fades, +0xB8), `docs/widescreen.md` (loading
screen drawn once, the God Hand 2D primitive log).

## Where we start
- Live, the native picture (3D, plus Pinch) is uploaded into the GS frame and Z at the 3D marker; everything after
  it (the post chain: glow, MotionSmooth, FixBlend, SmartBlur, ZoomSmooth; letterbox bands; the 2D pass) is drawn by
  the GS on top. The post chain is native only offline so far (checked 31 Oct: `renderOffscreen` runs no `recordPost`). So the HUD looks right today; 0.14 is about
  drawing it ourselves so 0.2 can switch the GS off.
- Frames with no 3D (menus, loading screen, movies) have no marker, so nothing native is drawn in them at all.

## Approach (decided by the user, 30 Oct late): engine-level hooks, no GS packets
**Hook each 2D producer at its source** (STR glyph emitters FUN_0046A980 / FUN_0046AA80, FUN_004B4580 rectangles,
the GFX bar geometry, the movie picture, whatever the survey adds) and take position, size, texture and colour as
plain values; the original's GS packet building is skipped. No GIF parsing in the 2D path, so nothing PS2-shaped
stays in the final product. Each draw carries its producer, which 1.1 (HUD anchoring) and 1.3 (crisp HUD at output
resolution) build on. Cost: one hook per producer: 4 new ones (survey below). Performance is not a factor: a few
hundred quads per frame either way. Rejected: capturing the 2D code's GS packets (the P5 pattern), because it keeps a
GS-format translation layer.

Ordering rule (same as purity today): 2D goes native only if the frame is pure when the 2D pass starts; otherwise
the whole 2D pass stays on the GS for that frame (hybrid case, parks until 0.2).

## Survey results (30 Oct late, Mac, offline)
**Frame order** (`FUN_003E2380`, the frame's display function): GFX_Render (0x3E2EA0 / 0x3E2EB4, world GFX) ->
`FUN_004B4A80` (Before2D: Display3DElements, then the letterbox bands, then draw manager state 10 = 2D draws queue into
+0xB8) -> GFX_Render (0x3E2FA0: HUD GFX objects queue their geometry) -> PS2_After2D 0x4B4A00 (FUN_004BBFC0: +0x28 = 1,
render +0xB8, SetZTest 1) -> PS2_Flip (DMA kick). So every 2D draw happens before the kick and can join the same native
frame (after its post effects).

**List +0xB8, census of 237 saves** (`tools/b8census.py`): kind 6 STR text in 157 saves (1 entry, 2 in one); kind 4
CJIdxMesh in 87 (up to 33 per frame); kind 3 (type 0xC, FUN_00458FC0 -> GFX particles FUN_004584C0) in 2 (both menus:
the rain over the pause menu); kind 11 rectangles in 2. Empty in the other ~75 (HUD hidden, cutscenes). GFX list
(`**0x674D04`, node type byte +0, next +0x14): Gao 0x11 x8, bars 0x17 x7, Interface 0x18 x1 in 233 saves (always
present, drawn only when visible); TransitionFade 0x19 x1; 0x1A x4.

**Producers (what the hooks attach to):**
| Producer | What it draws | Where it comes out | Hook |
|---|---|---|---|
| STR text: `FUN_00469C00` (kind 6) -> letters by `FUN_0046A980` (integer coords) / `FUN_0046AA80` (float coords x display scale +0xF4/+0xF8) | HUD tutorial text, menu text, button prompts | one sprite per letter: UV rect (param 7: u0 +0, u1 +4, v1 +8, v0 +0xC as written), RGBA (param 6), screen x0,y0,x1,y1, font page (param 8) | **new**: the two letter functions + the page's texture (FUN_004B2000 per page) |
| Rectangles `FUN_004B4580`(x0, y0, x1, y1 in 0..1, colour, mode) | letterbox bands (called directly by FUN_004B4A80 when world +0xEA0 > 0, before state 10), Fade / TransitionFade / menu dimming (queued as kind 11, table 0x6DF6E0) | untextured screen quad | **new**: one hook, both cases |
| GDI geometry -> CJIdxMesh in +0xB8 (kind 4) | HUD bars (GFX 0x17 `FUN_00478CC0`: GFX_ul_Bar_Geom* quads through CAM_2Dto3D), Gao (`GFX_Gao_Render`: 3D objects placed at a screen point through CAM_2Dto3D, then GRO_Render: sand tanks), Interface widgets (`GFX_i_Interface_Render`, vtable +0xC; MENU_cl_Dlg::AddGFX: menu panels, icons) | world-space mesh drawn with the 2D pass state (Z test off) | **existing** CJIdxMesh native path; allow +0xB8, after post effects |
| GFX particles in +0xB8 (kind 3) | rain over menus | particles | **existing** FUN_004584C0 path |
| Movie: VIDEO_DrawFrame 0x437CC0 -> `PS2_DisplayVideo` 0x4B4340(frame RGBA pointer from FullMovie 0x19F700, width / height in 16 px blocks, x, y) | the movie picture | 16x16-block uploads straight into the frame | **new**: one hook. Decoding is already native (FFmpeg `ipu` in `pop_movies.cpp`, 16 Oct), so 0.3 only has movie sound left on this side |
| Loading screen: images by `FUN_004CA700` (from FUN_004C9B40 in the world change FUN_00434FC0), drawn by `FUN_004C9C80` | two 256x256 images, bigfile keys 0xF402E7E7 / 0xF402E7E8, loaded straight into GS memory (TBP 0x3480 / 0x3880) | two rotated quads per call (below) | **new**: FUN_004C9C80 |

**Other projects:** nothing to reuse. OpenRatchet has no native HUD, menus or movies yet (stub, "Phase 17");
ProjectCane's 2D code is not visible from its public pages (no licence, read only anyway). God Hand Rearmed's HUD
anchoring is a 1.1 reference (`docs/widescreen.md`).

**New saves (30 Oct evening, `game/analysis/saves1030b/`):** 192020 / 192024 title menu (a normal 3D frame: STR text
kind 6 x2 and the logo as one kind-4 mesh in +0xB8; nothing new), 192103 loading screen, 192106 a movie (letterboxed).
- **Loading screen = `FUN_004C9C80`** (called through a pointer from FUN_004A5E80 while the loader thread runs; the
  main thread and cLoader take turns by WakeupThread / SleepThread, FUN_00434EC0 / FUN_00434F00). Per call: a fade
  (state 0x6751E4: 1 in, 2 held, 3 out; alpha 0x6751E8, +0.01 / -0.05 per call), two angles (0x6751EC += 0.001,
  0x6751F0 -= 0.006), then a clear and two 256x256 quads centred on the screen, rotated about Z: the image at GS 0x3480
  opaque (ALPHA 0x29), the one at 0x3880 blended (ALPHA 0x44). The images: bigfile keys 0xF402E7E7 / 0xF402E7E8
  (`FUN_004CA700`). **So it animates: redrawn per call, not "drawn once"** (the old widescreen note, corrected there).
  Hook: FUN_004C9C80 with plain values (two angles, alpha, two images).
- **Subtitles: none.** No disc movie has any (header flag 0x17 = 0, size 0x24 = 0 in all 11 .INT files) and the ELF
  has no subtitle strings; in-engine text would be STR text (hook above). Nothing to find.

## Steps
0. **Survey: done** (above).
1. **Native 2D layer, offline.** Renderer: a pass after the post effects (no Z test, the blend modes batches already
   support). Hooks for letters and rectangles; CJIdxMesh / particles in +0xB8 into that pass. `pop_state_frame`
   replays the letterbox, +0xB8 and kind 11. Judge against GS-only saves: 185602 (life bar, sand tanks, tutorial text),
   191358 (white-out), 192114 (save menu, half black), 090720 (pause menu), a letterbox cutscene (e.g. 191059).
   **Done 1 Nov (offline, with step 2's live code).** Renderer: `Draw::afterPost` (the 2D layer, a resumed pass after
   `recordPost`, in `renderOffscreen` and `present`) and `Draw::screen` (model = the whole transform); frame format: Z test
   word bit 2 = after post, bit 3 = screen-space model. Hooks: FUN_004B4580 rectangles (`rectangle`; queued calls
   left alone), letters FUN_0046A980 / FUN_0046AA80 (`letter` / `letterF`; registers from the disassembly: x0 a1, y0
   a2, x1 a3, y1 t0 with y up, RGBA t1, ST t2, slot t3; the float one f12..f15, RGBA a1, ST a2, slot a3), slots ->
   font page / texture / flags worked out at FUN_00469C00's entry (`textBegin`, as its first loop counts), GS order kept
   through seq blocks per slot. A native letter's 0x50 bytes in its slot are written as a zero-area sprite (the
   packet's GIF tag counts them). **The 2D pass has its own camera:** GFX_Render(+0x478) runs under camera matrix
   0x6D4800 (identity; FUN_003E2380), so list +0xB8's meshes (HUD bars, Gao, menu panels) sit near the origin. Their
   clip = S x inverse(view gfx +0x420) x (world -> clip gs +0x240) with S: (x, y, z) -> (-x, -z, -y) (fitted: the
   life bar top left; (x, -z, -y) is a reflection and mirrored it), the same from the 3D or the 2D pair (`twoDClip`,
   read at listRender(+0xB8)). GFX particles in +0xB8 stay in display pixels (`gifToBatch`). +0xB8's sort is a no-op
   (FUN_004B7A80): list order is draw order. `pop_state_frame` replays the letterbox, +0xB8 (Z test off) and kind 11;
   **letters can't be replayed offline** (FUN_00469C00 is game code), so text is judged at the boot.
   Scores (score_saves, `POP_VIEW_GPU_POST=1` = the in-game post path; the file viewer's default CPU post runs after
   the 2D layer): 191358 white-out 164.6 -> 0.64 (max 1), 192114 save menu 47.0 -> 11.0 (text missing), 090720 pause
   28.0 -> 5.9, 185602 HUD 7.85 -> 6.77 (text missing), 191059 letterbox bands black on the right rows. Render gate
   (gsref1027, default viewer): the 6 saves without HUD pixel-identical, the 7 with HUD all better.
   Open for the boot: the slot table (`text slot mismatch` log line), any 2D drawn straight to the GS by an unhooked
   path (would land over the native 2D), the HUD under live play.
2. **Live, in-game HUD and letterbox**, switch `POP_NATIVE_2D` (off until the user's check): the hooks record while
   the frame is pure and skip the originals. Why-line when the 2D pass is left to the GS. **Code done 1 Nov** (needs
   POP_NATIVE_POST and LATE; `play.ps1 -Env 'POP_NATIVE_2D=1'`; stats line `[pop:world] 2D:` every 300 frames,
   `POP_NATIVE_WORLD_WHY=1` names what ended purity). **Boot 1 Nov** (`game\jobs\twod1101*.ps1`, 5 saves, native vs +2D,
   shots `game\recomp-sotc\shots1101*`): the frame stays pure through +0xB8 (enders list18 / rect / idx +0xb8 gone
   but 1 per 600 frames), no slot mismatch, fps unchanged, STR text right (tutorial, pause, save menu). Fixed the same
   day (39f6c78): the queuing call of FUN_004B4580 (state 10) also draws (its GS sprite covered the native picture: save
   menu at half brightness); natively it now queues the kind-11 entry itself and draws the rectangle. **Open:** (a) the
   HUD ring above the life bar has 4 gaps and the small ring under the sand tanks shows glyph-like garbage instead of a
   circle; (b) the save menu's 9-piece panel shows its overlaps (edges darker, corners lighter; GS: even). Both are
   +0xB8 drawing, not text.
3. **Movie display**: `PS2_DisplayVideo` hook, picture drawn natively full screen (a frame with no marker: the native
   frame is just the movie).
4. **The loading screen** (`FUN_004C9C80` hook: the two rotating layers and the fade; a frame with no marker).
5. **User check (TV):** HUD in a fight, pause menu, title menu, a loading screen, a movie, a fade, a cutscene. Then
   `play.ps1` default on and close 0.14.

Boots needed: steps 2, 3, 4 (one short stnative job each, user asked first) and 5. Step 1 is offline.

## Not in 0.14
Crisp HUD at output resolution (1.3), widescreen anchoring (1.1), movie sound / the rest of 0.3, performance.
