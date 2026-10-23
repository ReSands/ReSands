# Widescreen (roadmap 1.1; was 1.4 until 23 Oct): plan notes

Collected 23 Oct 2026 so the work can start from here when 1.1 comes up. Sources: our own attempt (16 Oct,
`docs/history.md`), the PCSX2 widescreen patch for our disc, God Hand: Rearmed's any-aspect-ratio code (GPL-3.0,
read at 4e63668; the user likes this approach), GT4Recomp (`docs/reference.md` "GT4Recomp"), rePOP's Xbox hooks.
Nothing here has been built yet unless it says so.

## What we already tried (16 Oct)
`tools/pnach.py` + `rebuild.ps1 -Pnach 'Widescreen 16:9'` compiles the PCSX2 patch's code words into the generated
code. Gameplay, HUD and menus looked right at 16:9. What failed: movies and the loading screen are drawn for 4:3, and
the loading screen is drawn **once** and then left on screen, so switching the display aspect per frame did not work.
Parked until after the native renderer (user decision).

## The PCSX2 patch for SLUS-20743 (nemesis2000), read against our ELF
`game/patches/SLUS-20743_7F6EB3D0.pnach`, section `[Widescreen 16:9]`. Original words read from `SLUS_207.43`
(23 Oct). Each 16:9 constant must become a formula in the aspect ratio A (4:3 = 1.3333) before any-aspect support works:

| Group | Address (function) | Original -> patched | Reading (decoded by hand; check before use) | Any-aspect form |
|---|---|---|---|---|
| gameplay | 0x4B5114 (FUN_004B5000, projection/matrix setup) | `lui v1,0x3F80` (1.0) -> `lui v1,0x3F40` (0.75) | a 1.0 becomes 0.75 = (4/3)/(16/9) | (4/3)/A |
| gameplay | 0x4B5144, 0x4B514C, 0x4B5150 | `div.s f2,f21,f3` / `div.s f13,f0,f3` / `div.s f12,f20,f3` -> `mov.s` of the same source | three divisions by f3 removed | unchanged; work out what f3 is first |
| rfix | 0x4A4E70, 0x4A4EAC (FUN_004A4E00) | `lwc1 f0,-0x79B4(gp)` + `cvt.s.w f0,f0` -> `lui at,0x4455` + `mtc1 at,f0` | a width loaded from a global (int) becomes 852.0 = 640 x (16/9)/(4/3) | 640 x A/(4/3) |
| menu fix | 0x464538, 0x478CBC..0x478D08 (FUN_00478CC0) | `jal 0x478CC0` -> `jal 0x478CBC` (one word early, into an inserted `lui at,0x4456` = 856.0); `lw a3,0x10C(v1)` -> `li a3,-107`; `lwc1 f5,0xF4(v1)` -> `mtc1 at,f5` | menu width 856 and x offset -107 ~ -(856-640)/2 | width 640 x A/(4/3) (+ the pnach's rounding), offset -(width-640)/2 |
| HUD fix | 0x1D8C80, 0x1D8C88 | `lui v1,0x3D66` + `ori a0,v1,0x6666` (0.05625) -> 0xBDDDDDDD (-0.1083) | one HUD x constant moved | derive from 2 values (4:3 and 16:9) once its meaning is known |
| partial bb fix | 0x253110, 0x2BD92C | `jal 0x24D040` -> `nop` (both) | a call (bounding-box test?) skipped at two sites; "partial" = edge culling only partly fixed | find what 0x24D040 tests and widen it instead of skipping |

Needed for any-aspect: the "menu fix" uses a code cave one word before FUN_00478CC0 (the word there is 0). With
instruction hooks (below) no cave is needed.

## God Hand: Rearmed's method (to copy)
Files: `src/godhand/widescreen.cpp` (124 lines), `src/godhand/hud_layout.cpp` (574), `recomp/godhand.toml` (hooks).
1. **The game still renders its 4:3 frame; the renderer shows it A wide** (anamorphic). Only constants change:
   the camera's horizontal view x A/(4/3), font width and health-gauge width / (A/(4/3)). These are the pnach's
   constants **computed from A** instead of hard-coded 16:9, so 16:10, 21:9, 32:9 and "auto" (monitor shape) all work.
2. **Instruction-level hooks in the recompiler**, not word patches: `[[patches.hooks]] address / hook / flag /
   replace=true` runs a host function instead of the `lui`/`ori` that loads the constant, and it sets `$at` to the computed
   float. The flag is read at run time, so 4:3 and wide are the same build (`ps2xSetPatchFlag("widescreen", wide)`).
   Their recompiler change: `ps2xRecomp/src/lib/config_manager.cpp` ~164 (+ the code generator). Porting this into
   our PS2Recomp-sotc is the first job of 1.1; it also serves 1.2 (frame-rate fixes as toggles).
3. **HUD anchoring** (`hud_layout.cpp`): hooks sit in every 2D drawer (God Hand: `Gp_draw_line/poly3/poly4/...`
   + sprite drawers + the glyph drawer), right after the vertices are converted to GS coordinates; the hook narrows the
   x of those vertices. Each HUD widget (player bar, radar, roulette, identified by the caller's return address or
   by the widget's own trans function) is one **group**, narrowed about its anchor: a group narrower than 12% of the
   screen keeps its own centre (button prompts over things in the 3D view), otherwise left edge / centre / right edge
   by the third its centre is in. The anchor comes from the group's x extent in the **previous** frame.
   Layouts: `16:9` (widgets at the edges of a centred 16:9 area, so 32:9 does not throw them to the far edges) or
   `4:3` (where the PS2 had them); F7 switches live. Full-width quads (fades, backgrounds) stay stretched. Other
   gameplay 2D (text windows) is narrowed toward the centre.
4. **Menus pillarboxed by detection, not by a list:** a frame that draws a full-screen background in a non-HUD
   layout is a menu frame. Then nothing is narrowed and the display is forced to 4:3 with side bars (or stretched,
   user option). A **hold counter** keeps the menu state for a few frames without a background, so transitions
   do not flicker between shapes.
5. **Debug aid:** F6 logs every 2D primitive of the next frame (drawer, caller, group, anchor, x before/after).
   That is how they assigned each caller to a group.

## How it maps onto PoP
- **Our failed case (loading screen drawn once) fits point 4:** the 4:3 decision has to **latch** until the next
  3D frame instead of being decided per frame from what was drawn. Same for movies: our native movie player (0.3)
  knows when a movie is up, so pillarbox from that state, not from draw contents.
- **Culling is the part God Hand did not need** (no culling code in Rearmed). PoP does: the pnach's "partial bb fix"
  shows objects popping at the edges. Leads: what `FUN_0024D040` tests; the world->clip matrix at graphics object
  +0x240 (`docs/native_renderer.md`); rePOP's Xbox Hor+ projection/culling hooks (same engine; `docs/reference.md`
  "reSOT"); GT4Recomp's list of what breaks (edge culling/LOD, shadow and reflection sizes, HUD anchors,
  pixel-placed 2D).
- **The native renderer changes step 1:** once the world is native (0.2), it can draw the wider frame directly
  instead of anamorphic 4:3, at the output resolution (1.3). Until then, with the emulated GS on, it is the same
  as God Hand: a 4:3 frame shown wide. The camera constant and the culling change are needed either way.
- **PoP's 2D path still has to be found:** which functions draw the HUD (health/sand tanks, sword prompts), menus,
  subtitles and the loading-screen medallion, and where each converts to GS coordinates. A PoP version of their F6
  primitive log is the tool for that. The pnach's "HUD fix" and "menu fix" are the first two places.
- **GT4Recomp's check applies:** verify every pnach word against our ELF before trusting it (done above for the
  original words; the meanings still need checking in Ghidra).

## Order when 1.1 starts (proposal)
1. Port switchable instruction hooks into PS2Recomp-sotc (shared with 1.2, uncapped frame rate).
2. The pnach's gameplay + rfix constants as hooks computed from A; check the camera at 16:9 and 21:9.
3. Culling: find what 0x24D040 tests, widen it with A; play through to find edge pop-in.
4. 2D primitive log -> HUD groups and anchors; menus and loading screen latched to 4:3; movies pillarboxed.
5. Cutscene framing check (CLAUDE.md roadmap 1.1).
