# Native renderer (Stage B), plan

Decided 14 Oct 2026: Vulkan; PS2 assets only; the emulated GS (SotC fork, OpenGL) stays for
everything not yet native. Format knowledge comes from the Python reference converter
(`tools/jade2gltf.py`, `tools/jadetex.py`), checked against captured frames.

## Steps

1. **Engine-level draw list per frame.** (15 Oct: solved for meshes; see CLAUDE.md "Stage B step 1 solved". Left: Ref packet addresses, particles, shadows, VU driver switches.) Which elements the game draws, with which matrix and
   material, read where the game decides it (the draw manager's element lists), not from the DMA
   chain. Open question first: the frame capture saw only 11 `CJMeshElement::Display` calls while
   the DMA chain held 1,440 geometry uploads, so the static world reaches VU1 by another route
   (`Display` chains to the next element through +0x4C, possibly as a tail call the hook does not
   see; the lightmap list; or a batch path). Find it by decompiling `CElementList::Render`
   (0x4B9440) and reading the list entries directly. Judge: the per-frame list reproduces the DMA
   chain's draws (`tools/dmacmp.py`).
2. **Load-time conversion in C++.** (16 Oct: static world done offline, `tools/native/src/assets.*`, pixel-identical to the Python converter; ReadElement hook live on the Mac, `POP_NATIVE_LIVE=1`: every drawn static element's positions match its load-time conversion; next: skinned elements.) Port the reference converter's decoding (inline and
   reference packets, instanced meshes, skinned meshes, UV scales, baked colours, lightmap UVs,
   textures incl. PSMT4 lightmaps, material blend bits) into the runtime, hooked at
   `CJMeshElement::ReadElement` / the visual loader, producing vertex/index buffers and textures.
   Judge: identical output to the Python converter on every world pack (a natural Gemma job:
   precise spec = the Python code, strict judge = byte comparison).
3. **Vulkan "native view" window.** (15 Oct: offscreen viewer done, `tools/native/`, 0.870 vs the game frame; next: window + live draw list.) A second window in the runtime process that draws the frame's
   static world from (1) + (2) with the game's camera. No interop yet. Judge: `tools/imgscore.py`
   against the GS frame.
4. **Composite into the game window.** Draw the native world into the main presentation (Vulkan
   presenter, or GL/Vulkan interop) with the emulated GS drawing only what is not native yet.
5. Then: characters (GPU skinning with the game's bone matrices), effects, post-processing, 2D,
   GS off (Stages C-D).

Build: Vulkan headers + `volk` (loader) via CMake FetchContent, pinned versions; shaders compiled
to SPIR-V ahead of time and embedded. No Vulkan SDK install needed on the PC.

## QA plan for 1.0 (draft, 15 Oct 2026)

Status: proposal. The final decision is made once the game can be played end to end natively.

Goal: find the effects that are wrong only in some places (fog in one level, a water type, one
particle) without relying on someone noticing while playing. The game is played once, normally, on
one screen; the checks run in the background and leave a list of suspect frames.

Principle: one game, several renderers. While the emulated GS still runs (until step 16), every
frame is drawn natively and by the emulated GS from the same game state, so differences are
rendering differences, never timing or random-number drift.

### Tier 1: always on during play (native against the emulated GS)
- A background thread gets the GS reference image and the native image of every Nth frame, computes
  a difference score (heat map, perceptual metric, tolerance for float noise at triangle edges) and
  never slows the game: if it falls behind it skips frames.
- A frame above the threshold is saved with the world name, player position, draw list, and the
  GS command stream of the last few seconds (ring buffer). A button combo on the pad does the same
  by hand (save state + frame capture + both images).
- **Unsupported-feature log (no pixels needed):** every time the native renderer meets an element
  type, material flag, blend mode or fog/state it does not handle, it records it with the world
  name. After one playthrough this gives "fog: levels 9, 14" style lists. Same idea as the nightly
  coverage counts over the 566 packs.

### Tier 2: second opinion on flagged frames (PCSX2 software renderer)
- The emulated GS is not the truth (it had the glow/Z-buffer sprite and LoadImage bugs). PCSX2's
  software GS renderer follows the hardware rules most closely, so it arbitrates:
  - PCSX2 agrees with the emulated GS, native differs: native is wrong.
  - PCSX2 agrees with native, the emulated GS differs: the emulated GS has the bug.
  - PCSX2 differs from both: look closer (our stream, or a case where references disagree).
- Preferred form: live, as other recomps do (NBA Street V3 feeds PCSX2's GS in a separate process
  through a ring buffer). Fallback: replay the saved GS command stream through PCSX2's GS dump
  runner. The software renderer is slow, so it works on a sample or lags behind the game.
- Licensing: PCSX2 is GPL-3.0. Development tool only; never part of a release.
- Early check, before committing: how hard is it to build PCSX2's GS as a library or helper
  process, and can our GS command stream (`PS2X_GS_RECORD` format) be converted into its input.

### Unattended coverage
- Save states at the start of each level/area, each loaded and run for a few seconds in the overnight
  job, with the same scores and logs, so levels are covered without playing them.
- Scripted or recorded input runs for the areas that save states do not reach.

### Limits
- Tests the renderer against the GS's behaviour, not game logic (physics, AI): that would need a
  whole-game comparison with PCSX2.
- Movies and the loading screen are drawn differently and need their own pass.
- A real PS2 on a capture card is the ultimate reference but not worth the setup; spot checks with
  real-hardware screenshots are enough.

## QA plan for sound (draft, 15 Oct 2026)

Same principle as the graphics plan: checks run in the background during one normal playthrough and
leave a list of flagged moments. Judge sound from WAV files (`PS2X_AUDIO_DUMP`), never through
Moonlight (it added its own pops).

- **Render twice and compare.** While the IOP/SPU2 emulation exists, the game's sound calls drive
  both it and the native mixer; both outputs are written to WAV and compared in blocks of about
  100 ms after alignment by cross-correlation (latency and interpolation differ). Compare levels and
  spectra per block with a tolerance, not exact samples. Flag blocks that disagree, with the sound
  being played and the world.
- **Glitch detectors on one output (no reference needed).** Sudden sample jumps, runs of silence
  inside a sound (underruns), exact repeats of earlier stretches (stale-buffer replay, how the
  stream bug was found), clipping, DC offset, the roughness metric used on the music stream. The
  native mixer also counts late refills and underruns itself: many pops are scheduling, not data.
- **Every sound alone.** Trigger each sound ID in the banks by itself and compare the native mixer
  with an offline decode (vgmstream decodes the game's banks correctly). Covers rare one-off sounds.
  Unattended, overnight. It checks samples, not mixing (volume, pan, reverb).
- **Limits.** Exact matches are unrealistic (SPU2 interpolation and reverb differ; whether our
  current emulation does reverb at all is unchecked), so the tolerance needs tuning. Second
  opinion for disagreements: PCSX2's SPU2 code or the plain vgmstream decode.

## Work order to a full playthrough (user, 15 Oct 2026)

1. Native renderer (Stages A-D above).
2. Native movie decoder for the PS2 movie files.
3. Native sound.
4. Then the game can be played end to end natively with all the QA checks (graphics and sound plans
   above) running in the background during one playthrough.
5. Then fix the flagged issues one by one. Decide the final form of the QA tooling (incl. the PCSX2
   second opinion) at that point.

## Reference renderer toggle (decided 16 Oct)
1.0 keeps the emulated GS as an optional reference renderer next to the native Vulkan one (native
by default). A key toggles the displayed renderer. Default: the emulated path (VIF1/VU1/GS) is fed
only while shown, and is correct again a frame or two after switching. Setting "comparison mode":
both run every frame, so switching is instant (and a split-screen view becomes possible), at the
cost of the VU1 + GS work. The hybrid wiring built for the frame-by-frame comparison during the
migration is kept for this instead of being removed.

## Live textures: the route (16 Oct)
`CElementList::Render` (0x4B9440, decompile `game/analysis/native/decomp/004b9440.c`) sets the
render state manager (scratchpad 0x70000000) before each element's Display: entry +8 = material
(object), its sub-material list at material +0x10 linked by +0x20, entry byte +0xC = index; the
chosen sub-material object is stored at **0x70000034** (current material at 0x70000030), and
`FUN_004b2000(0x70000000, flags)` applies it when its +4 / +0x10 differ from the state's
0x70000000 / 0x70000004 (texture / blend state). So the Display hook can read the current
sub-material. Next: identify the sub-material object's texture field (+4, likely the texture
manager index) and map index -> asset key through `PS2_Texture_Load` (0x4B4E80, first word of the
16-byte `TEX_tdst_Data` entry = key; the override's POP_TEXLOG path), then decode the texture from
the pack holding that key (PS2 bigfile reader as `pop_hdtextures.cpp` does for the PC file, plus
`pop_assets::decodeTexture`). Blend state: the sub-material flags (record +0x14/+0x1C/+0x28) as in
the offline converter.

Measured the same day (Mac, opening balcony frame, `POP_NATIVE_FRAME` side file `<file>.sub.txt`):
the sub-material object (at 0x70000034 during Display) has **word 0 = texture manager index**
(-> key via `popTextureKey`, the override's PS2_Texture_Load registry), word 1 = flags (0x07,
0x17, ...; bit 0x10 likely alpha test), +0x10 = the alpha-test reference in its top byte (as the
record's +0x28). 43 of the frame's 52 draws map to a texture key: 16 are texture records of
ff02b51e (in the local test set), 27 are in packs not unpacked locally; 9 draws have no key.
Next: a PS2 pack cache (load + LZO-unpack the world pack the game looks up, as
`pop_hdtextures.cpp` does for the PC bigfile; `BIG_ul_SearchKeyToPos` hook), then
`pop_assets::decodeTexture` by key, and the sub-material's blend flags.

## Blend state from the game's own table (16 Oct)
The sub-material's flags word (record +0x1C; sub-material object word 1 at draw time, which
`CElementList::Render` compares with 0x70000000 before calling `FUN_004B2000`) drives all GS state
(decompiles in `game/analysis/native/decomp/`: 004b2000, 004b2500 `SetBlending`, 004b23c0
`ExtractLocalAlphaAndConstantColor`):
- bits 16-19, blend kind -> GS ALPHA: 0 opaque (0x29, FIX 0), 1 blend 0x44 (0x01 with 0x80; Ad
  variants 0x54/0x11 with 0x1000), 2 Cd*As+Cs (0x09/0x19), 3 Cd*As (0x89/0x99; 0x46/0x56 with 0x80),
  4 additive Cs+Cd (0x68, FIX 0x80), 5 Cd-Cs*0.375 (0x62, FIX 0x30), 6 Cs*As+Cd (0x48/0x58), 0xF
  Cd*(1-As) (0x46, lightmaps), others opaque.
- 0x10 alpha test, AREF = (+0x10 word top byte) / 2, ATST GREATER (LESS with 0x80); 0x100 AFAIL
  RGB_ONLY; 0x200 ZMSK (no depth write); 0x1 / 0x2 clamp; 0x8 / 0x20 / 0x40 FBMSK (alpha / colour
  write masks).
- bits 13-15: 2 = constant colour = current material (0x70000030) +8 (RGB, 0x80 = 1.0), 4 =
  0x808080; 0x400 = local alpha = sub-material word 0 >> 25. `CJMeshElement::Display` and
  `CJMeshElementRef::Display` (passes 0 and 2) then send VIF STROW + STMASK (0x15151515 RGB,
  0x40404040 alpha), so the packet's vertex colours are replaced.
Fight area survey (1,051 sub-materials): kind 0 x713, 1 x220, 4 x19, 6 x13, 2 x1.
Implemented: `pop_native::materialFromGsFlags` (shared by the offline `--pack` path and the live
export), per-state pipelines in the renderer, frame format PNF3 (flags + AREF word per draw;
constant colour / local alpha applied to the exported vertices). Balcony frame: 88 opaque, 20
blend, 2 additive halos (torch, lamp), 1 additive rain sheet (kind 6, drawn camera-relative with
an identity pool matrix: an effect for Stage D); the curtains now get their warm constant colour.
Not modelled: FBMSK, Ad blends against our framebuffer alpha (no 0x1000 materials seen), AFAIL
RGB_ONLY (approximated: no test, no depth write), ZTST off lists.

## Fog, Z test and draw order in the live export (16 Oct)
`Display3DElements` (0x4BC000) first builds the fog table: `FUN_004BAE80(start, end, 255|127,
.., table 0x70000270, id, colour)` -> per zone id 16 bytes (255, 0, a, b) with F = a + b * w
(zone 0xF / 0x1F: start 100 km = no fog), colours at 0x70000470 + id * 4. Each list entry carries
its zone, `(u16 at +4 & 0x3FF) >> 6`; `FUN_004B7200` uploads the zone to VU 0x10 and keeps it at
0x70000084; `DoChangeFogColor` puts the zone colour in render state +0x20 and `SetBlending`
copies it to +0x24 (FOGCOL), or 0 for kinds 4/5/6/0xF (additive and darkening blends fog to black).
List order: +200, +0x2C, +0x3C, then (when both used) +0x68 and +0x78 with `SetZTest(0)` (render
state byte 0x70000038 = 0: GS ZTST ALWAYS), +0x4C, lightmaps +0x5C, shadows, +0x88, +0x98, +0xA8.
Frame format **PNF4** records per draw fog a/b, FOGCOL and the Z-test byte, all draws in the
game's order (the viewer then draws one pass in that order, `Options::gameOrder`).
**Correction (24 Oct): the zone entry is (max, min, a, b), not (255, 0, a, b).** The VU1 drivers load it into vf31 and
clamp F with `MINIx.w` / `MAXy.w` (static driver 9d41512c, e.g. 0x0F68/0x0F88). Night palace (save 21_202624): zones
0/3/6 have min 102 (fog never above 60%), zone 1 min 127.5; zones 0x10+ have max 127. Without the minimum, distant
buildings came out as flat fog colour (user, 24 Oct, palace at night). Fixed: fog min/max per draw (`DrawMaterial`,
PNF8, push constant fog.zw, `mesh.vert`); A/B over 191 saves 14.46 -> 14.33 (28 better, 3 worse by <= 0.5, 160 same;
21_204628 11.80 -> 5.36, 21_202624 3.00 -> 0.84). **PNF8** = "PNF8", the layout (5, 6, 7), then that layout with fog
min, max after fog b per draw; all writer modes use it.
Balcony frame: world fog (300.3 - 5.667 w), sky dome and backdrop (2643 - 6.467 w), FOGCOL
RGB 52,78,95. Clear sky pixel: game (50,78,94), native (54,82,97). No draw used the no-Z-test list
there. `~/Desktop/pop_renders/27_live_fog_order_game_vs_native.png`.

## Draw paths not converted yet (balcony frame, 16 Oct)
The frame export now counts the other `CElementList::Render` paths and logs their textures:
- **`CJIdxMesh::Display` (0x4BDB80, list type 0x10): 18 draws = the palm trees** (frond texture
  128x64, flags 0x10117) **and the torch fires** (additive kinds 4/6). Display CALLs a ready DMA
  packet (low 28 bits of the mesh object's first word; objects in the table at *(0x6DF870)) with the
  pool matrix and the static VU1 driver; a UV matrix is set first when the sub-material's +8 is
  nonzero (UV animation, not modelled yet). **Drawn live since 16 Oct**: the packet decodes like the
  instanced ones, with **UVs at 1/512** (1/4096 sampled only the texture's corner).
  `~/Desktop/pop_renders/28_live_idxmesh_palms_game_vs_native.png`: palms, torch fires, fog.
- `FUN_00458FC0` -> `FUN_004584C0` (types 0-0xF, VU program 4, GFX): 14 draws: rain streaks,
  smoke puffs, the torch flame sprite sheet, sparks, star field.
- `FUN_00469C00` (type 0x18): 2 draws with a light-shaft texture.
- `CJadeSpriteElement::Display` (0x4C6200, types 0x1C/0x20) and `CJSmWaterElement::Display`
  (0x4C77C0, type 0x14): none in this frame.
The moon disc itself was not found among these; candidates: the additive disc glow above, or the
after-effect halos (`docs/postfx.md`).
The rain sheet (CJMeshElement, kind 6, identity pool matrix, vertices around the origin) is drawn
camera-relative: effect work.

## Texture UVs are always 1/512 (16 Oct)
The VU1 drivers compute texture ST as `ITOF4(raw) * I` with `LOI 0.03125` (static driver 0x4CDE80
at 0x0EE8/0x0F08; the character drivers likewise); no driver uses ITOF12. Lightmap UVs use ITOF15
(1/32768). The earlier rule "inline packets 1/4096, block-referencing 1/512" sampled 1/8 of the
texture for inline, instanced and CJIdxMesh draws (the balcony's moon was drawn, but from the corner
of its atlas, the gradient strip). Fixed in `assets.cpp` (convertElement, convertInstanced),
`jade2gltf.py` and the live instanced path. Balcony frame:
`~/Desktop/pop_renders/29_live_uv512_game_vs_native.png` (moon, dome texture, arch filigree,
curtains). The 0102 judge frame scores the same (brightness correlation 0.976 either way; the
changed areas there sit under the HUD text).
Halos: `AE_AddHaloToDisplay` (0x4AB1C0; array 0x6DF2C0, 32 bytes each, count 0x67518C; drawn by
`AE_ProcessOneHalo` as additive screen quads, ALPHA 0x58 = Cs*Ad + Cd with Ad an occlusion mask
written by `ProcessLensFlare`) is not called in the balcony scene at all.

## Character lighting live (16 Oct)
`CVuManager::DoSetSkinLightEnv` (0x4B64C0) uploads the VU1 lights per light set: `SetSkinDriver`
(from `CElementList::Render`, list entry bits 42-49) leaves the set index at 0x70000088; set =
`*(0x700004F0) + index * 0xF0`: +1 point-light flag, +4 ambient RGB (0x80 = 1.0; alpha = local
alpha), +8 light count, +0x10 + 16i directions (world), +0x50/+0x90 point-light data, +0xD0 + 4i
colours, +0xE0 + 4i types. Without lights and ambient the global environment is used (gp+0x140
directions as matrix columns, gp+0x180 colours as rows); a constant colour (render state 0x39)
replaces the ambient and drops the lights. The frame export bakes ambient + sum max(0, n.d)*colour
per vertex (normals now kept by `pop_assets::convertSkinned`, rotated by the bone matrices);
point lights (types 1/2) are skipped. A/B: `POP_NATIVE_NOLIGHT=1`.
`~/Desktop/pop_renders/30_live_prince_game_unlit_lit.png`: game, unlit, lit (trousers lose the
warm cast, as in the game; the game's warm rim light from the curtain fire is a point light).

## Glow (ColorDiffusion) natively (16 Oct)
Pass dump (`POP_GIF_TAP=1 POP_PASSDUMP_AT=135`, now also writing each buffer's alpha as
`*_alpha.png`) showed the balcony's curtain glow is post-processing: `CAfterEffect::ColorDiffusion`
(retail **0x4AE100**, prototype 0x46F680; decompile `game/analysis/post/decomp/0046f680.c`) with
`ApplyConvolution` (retail 0x4AE640):
1. half-resolution copy of the frame, ALPHA 0x88 (Cs * As) when `mask`: the frame's **alpha
   channel is the glow mask**;
2. `levels - 1` times: bilinear halving into the next rectangle of the half-res buffer (it shares
   memory with the Z buffer), then (with `convolution`) a 3x3 box: every weight 15 (retail table
   0x63F418) + `strength`, as vertex colour / 128; centre tap copied (0x29), eight neighbours at
   +-1 pixel added (0x68);
3. levels `levels-1 .. 1` added onto the frame, bilinear, ALPHA 0x68 with FIX = `fix`.
Balcony: levels 6, FIX 128, mask on, convolution on, strength 4 (args a2, a3, t1, t3, stack).
The mask: `Display3DElements` erases the frame alpha just before list +0x98, which runs with render
state 0x44 = 0xFF000000 (FBMSK: alpha only). Elements in that list are drawn a **second time**,
writing only As where they pass the depth test (ZTST GEQUAL) against the finished scene: here the
light quad behind the curtains (flags 0x440F: 0x8 = alpha written; local alpha 0x5C), visible only
below the curtain hem. Implemented: per-draw FBMSK (frame format **PNF5**, plus a `GLOW` trailer with
the parameters), renderer colour/alpha write masks, GEQUAL depth test, the alpha-only pass kept as
its own draw, and `tools/native/src/postfx.h` = CPU reference of ColorDiffusion (viewer applies it;
`--noglow` to skip). `~/Desktop/pop_renders/31_live_glow_game_vs_native.png`.
Next: the same pass as shaders (a CPU reference to judge a GPU version against: Qwen-sized), the
other effects when a scene uses them (DepthBlur, BrightnessContrast, SmartBlur, halos).

## GFX particles (16 Oct, first version)
`FUN_004584C0(element)` (called by `FUN_00458FC0` after it sets render state, fog zone and the
matrix index 0x70000090 from the list entry): element +0x14 -> particle object P, description
D = *P (+0x14 flags, +0x60/+0x68 axis sizes, +0x88 growth, +0xE0..+0xEC colour keys, +0xF0/+0xF1
sheet columns/rows, +0xF5/+0xF6 axis choice), generators P[10] = (array, count) of 0xD0 bytes
(+0x40 matrix, +0xC2 u16 count, +0xC4 particles of 0x30 bytes: +0 flags (bit 0/1 colour key pair,
bit 3 oriented, bits 8-14 sheet frame), +4 position, +0x10 quaternion, +0x1C rotation, +0x20 *
+0x24 life, +0x28/+0x2C size). Colours: `FUN_00460100(t, a, b)` = byte-wise lerp (a below 0.02, b
above 0.98); `FUN_00460040(t, c)` = c * t (alpha fade). Axes: `PAG_GetGeneratorAxis` (0x459840):
camera right/up (`*(gp+0x110)` +0x160/+0x170) or the generator/object matrix rows by flags
0x4000000/0x80000/0x8000000. The frame export builds billboard quads (position +- axis1 * sx/2
+- axis2 * sy/2; `POP_PARTICLE_FULL=1` for full extents; positions through matrix-pool entry
0x70000090, `POP_PARTICLE_WORLD=1` to skip it). Balcony: rain (2 x ~65 streaks), smoke puffs, the
torch flame sheet (4x2): flames now sit on the torches. Not done: oriented particles (flag 0x1000 /
particle flag 8, quaternions), sizes with random ranges (+0x64/+0x6C), sprite-sheet V direction and
half/full extent unverified, rain visibility. `~/Desktop/pop_renders/32_live_full_game_vs_native.png`.

## Torches (16 Oct, user report: right torch drawn over the palm, both too prominent)
Two causes, both modelled exactly now:
1. **AFAIL RGB_ONLY** (flag 0x100 with the alpha test; the palm fronds, 0x10117): pixels failing the
   test still write colour but not depth. It had been approximated as "no test, no depth write", so
   the fronds never wrote depth and the later additive torch fire drew over them. The renderer now
   draws such materials twice: passing pixels with depth write, then the failures (test inverted)
   colour only (`Material::afailRgbOnly`).
2. **UV animation**: when the sub-material's +8 word is set, `CElementList::Render` runs
   `DoComputeAndSetUVMatrix` (`FUN_0044A440`): scale = the +8/+0xC words with the low 17 bits
   cleared (as floats), offset = their low 16 bits << 16 times the scale; flags 0x40000000 /
   0x80000000 (word 1) replace the offsets by the running scroll at sub +0x14/+0x18
   (`FUN_0044A840`); byte +0x11 bit 7 adds the running angle at +0x1C, else a fixed flip/rotation
   from the sin/cos table at 0x4F0930 (`FUN_0044A540`). The fire glows (0xC0066207) scroll their
   fire texture; we had stretched the whole texture over the mesh (a chunky orange blob). The frame
   export applies the matrix to static, instanced and CJIdxMesh UVs (`POP_NATIVE_NOUVM=1` to
   compare). `~/Desktop/pop_renders/33-35_*`.
Left: the flame colour is still more saturated orange at its edges than the game's; some near palm
fronds render light grey where the game's stay dark (present before these fixes; next to look at).

## The haze (16 Oct, implemented)
After ColorDiffusion the frame runs `AE_ProcessSmartBlur` (retail **0x4A93C0**, prototype 0x466240,
`game/analysis/post/decomp/00466240.c`) -> `AE_BlurBackBufferIntoZBuffer` (retail **0x4A9740**,
prototype 0x4666C0, `decomp_proto/004666c0.c`), the game's soft/hazy look. Eleven GS sprites, all
`DrawRectToScreen` with 12.4 positions (XYOFFSET 1728, 1824) and 12.4 texel UVs; blitter args
(src, dst, FBMSK 0xFF000000 = alpha never written, bilinear, TCC 0):
1. frame 640x448 -> work buffer (FBP 280) 256x256 at (0,0), ALPHA 0x29 (Cs);
2. black border (256..448, 0..192), untextured, colour 0;
3. 256 -> 128x128 at (288,32); 4. black border (288..384, 192..288); 5. 128 -> 64x64 at (303,207);
6-10. ALPHA 0x44 with vertex alpha 0x40 (half strength): the 128 square into the 64 one shifted
   (+2,+1) and (+1,+2), the 64 one back over the 128 one at (-1,-1) (half-texel inset), and the
   128 square onto itself shifted (+1,0) and (0,+1);
11. the 128 square (half-texel inset) over the whole frame (-10,-10)..(639,447), ALPHA 0x44,
   vertex colour = effect object **+0x124** (RGB, strength in the top byte) with the strength
   times (1 - fade); fade (+0x144) = (1 - n/total)^2 while a fade runs (+0x140, +0x148/+0x14C).
   It draws when +0x128 == 0 or a fade runs.
`tools/native/src/postfx.h` `smartBlur()` (blit table `smartBlurBlits()`, generic `gsSprite()` =
the reference CPU rasteriser's sprite rules), judged by `pop_postfx_check smartblur
game/recomp/screens 56 51 57 [colour]` against the balcony pass dumps: blits 1-8 within rounding
(mean <= 1.4/255, max 3); blits 9-10 read their own target: with a snapshot of the source (what a
GPU does) mean 1.7/2.4, reading already-written pixels 2.5/3.3 (the reference rasteriser is in
between; the real GS's texture cache differs again). The final blit alone from the game's own work
buffer, with the live colour **0x35808080** (title/balcony): **mean 0.2, max 2**; the whole chain
vs the game's frame: mean 1.2 (no haze at all: 3.9). Live export: hook at 0x4A93C0 (reads at
entry only), `HAZE` trailer after `GLOW` in PNF5 (calls, colour, fade bits); the viewer applies it
after the glow (`--nohaze` to skip; scales with the render size).
GPU versions: `tools/native/shaders/gs_sprite.comp` (one sprite per dispatch, self-reads get a
copy) and `color_diffusion.comp` (RESAMPLE / MASK / CONVOLVE / ADD_UP on float planes), run by
`src/gpu_compute.{h,cpp}` (minimal Vulkan compute: storage buffers, push constants). Judge:
`pop_postfx_gpu_judge sprite|diffusion <spv> <frame.png>...` = **bit-identical to the CPU
references** on MoltenVK (and it fails deliberately broken shaders; a frame with an empty glow mask
hides convolution errors, hence the extra no-mask and synthetic-mask cases).

## Light-grey palm fronds: instanced packets with colours after positions (16 Oct, fixed)
The bright fronds and hanging net of the title scene (and the "near palm fronds light grey" on the
balcony) were one bug in the live packet decoder (`livePacketBatches`, instanced and CJIdxMesh
draws): it closed each batch at its V3-32 positions upload with the UVs/colours seen so far, but
packets send a batch's uploads in either order: `bbox(V4-32 x3 @0), UVs(@67), positions(@3),
colours(@131)` or `colours, UVs, positions, bbox` (`POP_NATIVE_UNPACKLOG=1` logs them). In the
first order every batch got the previous batch's colours and the first batch none (white). Now a
batch is the group of uploads between two uploads of the same format. Found by bisecting the
frame's draws in the viewer (`POP_VIEW_ONLY=<a>-<b>`, `POP_VIEW_LIST=1`) with the export's new side
file `<frame>.draws.txt` (per draw: kind, element, flags, texture, mean vertex colour).
Point-light sets (light set +1 != 0) take another path in `DoSetSkinLightEnv` (retail 0x4B6A40,
also for sets with no lights): it uploads `count` slots from +0x10 (vectors) and +0xD0 (colour
bytes) without checking types; the title scene's set 0 holds floats there (a position and a
matrix), not colours, so reading them as directional lights gave huge colours. The export now
uses the ambient alone for point-light sets (unverified approximation; `POP_NATIVE_PTLIGHTS=1` =
the old reading); the VU1 side of point lights is still to decode.

## Live native window, first version (16 Oct night)
Two processes, so the native window needs nothing from the runtime's own presenter (on macOS a
second window toolkit in the game process would fight raylib/GLFW for the main thread):
- **game side** (`POP_NATIVE_WINDOW=<port>` with `POP_NATIVE_LIVE=1`): the frame capture of the
  export runs every frame (Display3DElements to Display3DElements, `resetFrameCapture()` between),
  `writeFrame(true)` serialises it in memory as **PNF6** (= PNF5, but each image as texture key +
  w + h + RGBA, the pixels only the first time this viewer connection gets that key) and
  `tools/overrides/pop_live_client.cpp` sends it from its own thread over TCP 127.0.0.1 (message
  "PNFM", u32 size, payload; `tools/native/src/live_socket.h`, POSIX or Winsock). While a frame is
  still being sent the game builds none (a built frame is never dropped: it may carry a texture's
  only copy); textures decoded once per key (`g_liveImages`). Reconnects every second; a new
  connection resets the sent-texture set.
- **viewer side**: `pop_native_view live - <shots.png> --live <port> [--live-shots S] [--scale N]`:
  a thread accepts the game and keeps the latest frame; the window loop rebuilds the scene from
  each new one (`Renderer::reset(true)` keeps the textures, `upload()` adds only new ones in one
  command buffer) and draws it with the game's camera (F: free camera from the current view, G:
  post effects on/off, 1-4: internal resolution). Measured on the Mac (title scene, 682 draws):
  **5.8 ms rebuild and 1.4 MB per frame** (was ~50 ms / ~4 MB with every texture each frame); the
  game's own speed is unchanged (1.9 fps title scene, CPU GS bound).
- **post effects on the GPU in the window**: `Renderer::setPostEffects` (glow and haze parameters
  from each frame's trailers); `present()` copies the frame into a storage buffer, runs
  ColorDiffusion (`color_diffusion.comp` + `rgba_float.comp` conversions) and SmartBlur
  (`gs_sprite.comp`, self-reads from a copy) as compute dispatches in the same command buffer, and
  copies it back before the blit. Presented frame (`--frames N --present-shot <png>`,
  `Renderer::readPresented`) vs the CPU references on an exported frame: **bit-identical**.
- Not yet: the live window on the PC (tonight's job `tools/windows/live-window.ps1`), its own
  frame pacing / interpolation (it shows each game frame until the next), in-process drawing
  replacing the GS output (the reference-renderer toggle), 2D/HUD, and everything the export lacks.

## Live comparison against the game's GS picture (16 Oct night)
`POP_NATIVE_WINDOW_GS=<d>` (Mac `pop` runtime: `GS::copyLatchedHostPresentationFrame`, detected at
compile time, so the PC build is unaffected): each live frame carries the GS pictures latched at the
next d frame boundaries ("GSFB" trailers); the game waits for the viewer in this mode so no frame
is dropped. The viewer's T key shows the GS picture instead of the native one
(`Renderer::setOverlay`), and `--live-shots S` saves native + GS pictures and prints the mean
difference. **One boundary later lines up best** (every run, 1..4 scored). Balcony (Mac, scripted
new game `POP_INPUT='start@55,start@65,cross@85,cross@95,cross@105'`), mean absolute difference
per channel / share of values off by more than 24, best frames:
- first run: 4.2-5.3 / 1.8-3.1%
- oriented particles + sheet rows from the bottom: 4.0-4.5 / 1.8-2.3%
- **UVs and colours persist across a packet's batches** (as in VU memory; the grouping fix of the
  same evening had cleared them, so batches without their own UV upload were flat: the curtain
  panels): **3.4-4.1 / 0.6-1.3%**
Later the same night: pass-2 draws (a material's second layer; ~8 static draws per frame, 2,064 in
a balcony run, all dropped before) are now captured from their own packets (element +0x28):
balcony 3.3-4.0 / 0.3-1.2%. The fire glow at the window base is ~10-15% wider than the GS one
(brightness profile), not a big error. The torch bracket is drawn by no captured path (not a
skinned draw without a palette: 0 of those; not pass 2; the CJIdxMesh draws with its texture are
the birds; not an alpha-tested draw failing its test either): next, find its path. Rain: the two
rain draws (texture 2e005781, ~66 streaks each, AddAlpha) are degenerate (all four corners of a
streak at one point, i.e. zero axes) and their positions project far below the view: the rain
generator's axes/positions are read wrongly (flags 0x8000000: generator-matrix axes). Cutscenes:
the game's letterbox bars are 2D (not native yet), so those frames score ~15.
Fixes it found, in order: light sets read as `DoSetSkinLightEnv`'s point-light path does (no type
check; the ambient-only guess left a backlit curtain panel black), PAG_GetGeneratorAxis sizes
(fixed sizes pre-scale the axes, otherwise per particle), oriented particles (FUN_004584C0's other
branch: per-particle axes, the angle at +0x1C via the sine table at 0x6D3400, quaternion rows for
particle flag 8), sprite-sheet rows counted from the bottom, skinned elements without a bone
palette posed by the object matrix (they were dropped). Remaining visible differences on the
balcony: the fire glow at the window base (native larger and whiter), dark smoke puffs missing,
rain streaks missing, the torch bracket.
Tool: `tools/livecmp.py` (score, side = native | GS | |difference| x 4, crop = zoomed crops side by
side, mean = region brightness; pure Python).


## Native frames from save states, offline (17 Oct)
`pop_state_frame <state> <out.pnf> [--bigfile DATA/PRINCE.BF] [--packindex packindex.txt]` (run from
game/ps2/disc; build with `-DPOP_RUNTIME_SRC=<repo>/game/recomp/runtime-src`) makes a native frame from
any pop-sotc save in about half a second, with `pop_native_live.cpp` compiled in unchanged; render with
`pop_native_view <pnf> identity.clip <png> --frame` (any 16 numbers as the clip file; the frame has its
own camera). The game's own picture of the same moment: `tools/popstate.py <state> --frame <png>`
(displayed framebuffer from the saved GS memory, 640x448, no overlays). Judge: `tools/livecmp.py score`.
- A save holds the last frame's draw lists: draw manager at scratchpad **0x700000C0** (the a0 of
  Display3DElements; lists +200, +0x2C, +0x3C, +0x68, +0x78, +0x4C, lightmaps +0x5C, +0x88, +0x98,
  +0xA8). The tool stands in for the runtime (records the hooks `install()` registers) and replays
  Display3DElements -> CElementList::Render, setting the render state Render sets per entry: sub-material
  0x34 and material 0x30 (entry +8, list +0x10 by +0x20, index byte +0xC), fog zone 0x84 ((u16 +4 &
  0x3FF) >> 6), FOGCOL 0x24 (zone colour table 0x70000470; 0 for blend kinds 4/5/6/0xF, as SetBlending),
  Z test 0x38 (off for +0x78), alpha-only 0x44 (+0x98), light set 0x88 (entry bits 42-49).
- Mesh entry: element = low word << 4, matrix index = bits 28-37, pass = byte +0xD & 0xF.
- **Elements unknown at load** (every element after a save-state load): static ones decoded from the drawn
  pass's packet (`livePacketBatches`, UVs 1/512); **skinned** ones (bone palette at +0x48) from the drawn
  pass's packet too (`pop_assets::skinnedFromPacket`; characters often have only a pass-1 packet at
  +0x24), then posed with the game's bones as before. Live on the PC this is the save-state fix.
- **Lightmaps** (new, live too): CLMElementList::Render (0x4B7B80), 8-byte entries (element; kind 9/10
  in the low nibble; matrix (u16 +4 & 0x3FFF) >> 4; texture index (u16 +6 & 0xFFF) >> 2), each drawn as
  pass 3 with flags 0x30284 (GS ALPHA 0x46 = Cd * (1 - As), no depth write, fog off: UseLightmapPRMode
  PRMODE 0x58) and vertex colour 0x80 (SetLightmapColor). Pass-3 packet (+0x2C): V3-16 lightmap UVs at
  1/32768, V3-32 positions. **The lightmap CLUT** (GS block 0x3EC4 in the saves): index i -> grey 17 i,
  alpha 0x80 i / 15, so index = darkness; the decoder now sets alpha = grey (it was 255: all black).
  Instanced lightmaps (kind 10) are not drawn yet.
- Textures: index -> key from the engine's texture list (TEX_List: count 0x6CBC50, 16-byte entries at
  *0x6CBC58: key at +0, index at +10), packs from `tools/packindex.py` (texture key -> world pack over
  all 566 packs; 2,386 keys; 93-98% of a save's keys).
- After effects from the CAfterEffect object 0x6DF170 (FUN_004ABFC0): ColorDiffusion (glow) if bit 5 of
  +0x121, arguments +0x94 +0x98 +0x9C +0x90 +0x88 +0x8C +0xA0 and +0xA4 on the stack; SmartBlur (haze)
  if bit 7. GFX entries through FUN_00458FC0's state (0x90 = entry bits 28-37) into the particle hook.
- **Water** (CJSmWaterElement, kind 5): Display CALLs a ready packet that sets up (12 qw V4-32 + GS
  registers) and CALLs one sub-packet per patch; a patch uploads only V4-32 data (7 qw at VU 0, 16 at 8,
  2 at 30, 1 at 32) and runs VU1 program 7: the surface is tessellated on VU1 from control points, so it
  needs that VU1 program ported. With +4 set the game first copies the framebuffer (FUN_004A76C0) and
  draws the water twice (refraction). Not drawn yet.
- **Water packets, read from a save (29 Oct evening, fountain save `saves1029/play_20261029_202926`, codes 3328-3510 of
  its last chain; P6 runs in 21 of the 66 Mac saves, `tools/vu1inventory.py`):** three passes per refracting water.
  (1) **Mask:** FRAME_1 with FBMSK 0x00FFFFFF (alpha only), a 101-qw sprite strip DIRECT clears destination alpha
  to 0 over the water's screen box, then TEST ZGEQUAL, ALPHA 0x2A (Cs), TEX0 0x620013480 (256x256 CT32 at TBP
  0x3480 = FBP 0x1A4) and one P6 run per patch: per patch STCYCL 4/4 and V4-32 with FLG (TOPS-relative) 7 qw at
  +0, 20 qw (15 on the last) at +8, 2 at +0x30 (GIF tag + 0x80 colour), 1 at +0x32 (GIF tag, count 0x12, PRE,
  tri strip, ST RGBAQ XYZF2), MSCAL 0. (2) **Refraction texture:** FRAME_1 0x401A4 (the 256x256 buffer), ALPHA 0xA8
  FIX 0 sprites clear it to black; TEX0 0x668028000 (the frame itself, 640 wide), TEST ATE GEQUAL 0x7F, ALPHA 0x44:
  five sets of strips copy the frame into the buffer, tinted (0x67, 0x70, 0x7F), only where (1) wrote alpha. (3)
  **Colour:** FRAME_1 back to the frame (FBMSK 0xFF000000: RGB only), TEX0 = the buffer, TEST ZGEQUAL, ALPHA 0x44,
  XYOFFSET set, and the patches again through P6 (next VU1 run from code 3505). **Natively this needs what the
  renderer does not have yet: a copy of its own frame in mid-frame** (colour + the alpha (1) wrote; the native
  frame is only read back at the end, `native_renderer.cpp`), alpha-only and RGB-only writes, and an alpha test.
- **Water step 1 done (29 Oct night): P6 geometry offline, flat.** `tools/p6packets.py ELF STATE [--sig]` lists each
  water's passes: all 148 waters in the 95 Mac saves with P6 runs have the same structure (only the frame's FBP 0 / 0x8C
  differs). CElementList::Render (0x4B9440) draws a water with element +4 set as FUN_004A76C0 (mask set-up: VU qw 0 =
  (1, GIF tag), 0x3E0 a GIF tag), Display (returns to 0x4B9FF8), FUN_004A6F40 (the copy into the 256x256 buffer; VU qw 0.x
  = 0), Display again, FUN_004A6F00; P6 takes path `b` (0x1648) for qw 0.x = 1, `a` (0x268) for 0. Display only CALLs
  the element's packet (`*element & 0x0FFFFFFF`): BASE 0xD / OFFSET 0x1D5 (double buffered), REF 12 qw from 0x6C5BC0
  (= graphics object +0x200, +0x240, then the viewport: 320, 224, Z, 2048 centre) at VU 1, a DIRECT, then per patch
  REF 7 / 20 / 2 / 1 qw at TOPS +0 / +8 / +0x30 / +0x32 and MSCAL 0. `vuVif` now double-buffers (OFFSET resets TOPS to
  BASE, each run swaps; every P5/P9 packet sends BASE 0x18, OFFSET 0, so they are unchanged) and skips DIRECT.
  `waterRecord` (`POP_NATIVE_P6=1`, off by default; `=interp` the interpreter) replays the colour pass through the port
  and draws its triangle strips untextured. **Port bug found and fixed:** `a_g1_row` dropped `LQ vf4, 4(vi0)` at 0x310
  (the traces always had vf4 from the mask pass; `known_good/p6_a_g1_row.cpp`, `--chain p6` still PASS 600). Check: all
  2,987 P6 runs of the 59 distinct water saves byte-identical to the interpreter; 53 draw water offline (6 have P6 in the
  chain but no water left in the saved draw list); the fountain's two waters fill their basins exactly, behind Farah.
  The saved frame's alpha cannot outline the water (later passes clear it), so the GS check is the GIF output itself.
- **The refraction copy, read from the fountain save (FUN_004A6F40):** FRAME 0x1A4 (256x256), sprites with ALPHA 0xA8
  clear it to black (alpha 0 too); then TEX0 = the frame (640x448, CLAMP 5, bilinear TEX1 0x60), TEST alpha GEQUAL
  0x7F, ALPHA 0x44, vertex colour (0x67, 0x70, 0x7F, 0x80): five full-frame copies scaled to 256x256 in 32-pixel strips,
  four shifted by a few pixels (dest y +2; UV v +3, dest y -2; UV u +3, dest y +2; UV u +5 / v +3, dest y -2) and last
  the plain one. Only texels the mask pass gave alpha 0x80 pass, so the buffer is the tinted water area, grown at its
  edges by the shifted copies. The colour pass (ZBUF ZMSK, FBMSK RGB only, ALPHA 0x44, TEX0 = the buffer, TCC, MODULATE,
  bilinear) maps P6's ST into it; where the buffer is black (alpha 0) the frame stays as it was.
- **Water step 2 done (29 Oct night): mid-frame copy in the renderer.** `Draw::copyFrame` copies the frame as drawn so
  far (colour and alpha) before that draw; a material with `baseImage = kFrameCopyImage` samples it (clamped,
  bilinear). The scene pass ends, the colour target is copied into `frameCopy` (same size, made on first use), and the
  pass resumes in `renderPassLoad` (colour and depth loaded; both passes carry the same two dependencies, which
  render pass compatibility requires). Copies happen in the first pass, so in practice with `Options::gameOrder`.
  Check: `pop_blend_test` case "frame copy" (alpha 0x40 red copied and blended back: 158 60 100 vs the GS's 157 60 100),
  clean under the validation layer; frames without a copy are pixel-identical. Not in the PNF frame format or the live
  path yet (step 3).
- Score, mean |difference| over the 51 saves of 17 Oct (`game/analysis/play1017/native/scores_v*.txt`):
  19.96 (geometry, textures, lightmaps, characters) -> 17.44 (+ glow, haze) -> 16.85 (+ particles).
  Plain scenes 3-7; worst: full-screen effects (rewind flash, white-out, explosion), 2D (pause menu),
  and 4 saves whose draw lists were empty at the save. Missing: HUD/menus/text, water, shadows, halos,
  weaker fire/smoke/flash than the game, a dark-vs-bright doorway at the first gate (#3).


## Restart, 19 Oct evening: baseline, VU1 inventory, the frame-rate prize
Back to the native renderer after the VU1 speed work (the fights are VU1-bound: the VU thread is ~88% busy).
- **Water step 3 done offline (29 Oct night): the full water.** `waterRecord` (colour-pass call) records three draws per
  refracting water from one P6 run: the frame's alpha cleared (a sprite over the display, alpha only, no Z test), the mask
  (the same triangles, colour 0x80, alpha only, ZGEQUAL) and the colour pass (PNF image -2 = `kFrameCopyImage` +
  `Material::refraction` + `copyFrame`: mesh.frag rebuilds the 256x256 buffer per texel from the frame copy, the five
  copies of FUN_004A6F40 with their boxes, maps, tint and alpha test, then reads it bilinearly at P6's ST), then one more
  alpha clear (lists before +0x98 never write alpha natively). Fog: P6 writes its own F per vertex (225-255, nearly no
  fog); the zone's line (F ~90-160 there) fogged the already fogged copy again (091915 washed out): now a least-squares
  F = a + b * w per water, clamped to P6's range (residual < 5, 18 on one fountain water). The game's own buffer in the
  fountain save (TBP 0x3480, FBW 4) matches the formula (no systematic error; later passes change the saved frame).
  Sweep of the 53 water saves vs their GS picture: 22 better, 6 worse, mean -0.09; GS-only 184926 8.29 -> 7.70, 145139
  11.33 -> 10.75, fountain 8.88 -> 7.62. The worse ones (193004, 192947 +0.6) have a native base frame darker than the
  game's under the water, which the tint (0.80, 0.88, 0.99) darkens further. User sheet review: the water itself is
  fine; the mist around feet and pillars standing in water is missing from both native columns (likely the GFX particles
  of +0x88, which the offline tool lists but never draws; unproven).
- **Water step 4 done (29 Oct night): live behind `POP_NATIVE_P6=1`** (needs the late lists, `-NativeLate`: the water is
  in +0x88). Two live-only fixes the offline runs could not show: `worldFinish` refuses draws without a real texture, so
  waterRecord marks its draws native itself; and the mask call (returns to 0x4B9FF8) is skipped when the colour pass can be
  native (on the GS it closed the frame's purity: "ended by water +0x88" in 1,929 of 2,400 frames without P6). Fountain save
  202926, 45 s hands-off: water draws 6,099, native 2,232, not replayed 0, 60 fps; the rest stay on the GS because a GS GFX
  draw earlier in +0x88 ends the purity first (2,285 frames; the prefix rule, 0.2). The user watched it: looks good. **On by default since 30 Oct** (`POP_NATIVE_P6=0` off; still only with `-NativeLate`).
- **Review 30 Oct** (5c9733e..4e0debf). Fixed (3170ab5): water records went to the renderer with a content key, so the
  kept-mesh cache stored ~200 KB of one-frame geometry per frame at the fountain (estimate) until the 512 MB budget
  dropped everything and the whole world was resent (the N5 path). `DrawMaterial::frameOnly` now sends them with key 0,
  as posed characters; the same for halos, P9 sprites, P5 and CPU particles, and (cddb051) the lit statics' environment-map
  pass (its UVs follow the camera; pass 1's baked light stays cached). Not verified live yet. Open: no render-gate save has
  water (162246, 162621, 155110 checked): make a GS-only fountain save on the PC and add it as a `vu1` gate line.
- **Baseline** (`tools/native/score_saves.sh <states> <out>`: pop_state_frame + viewer + popstate --frame +
  livecmp, ~1 s per save on the Mac): all 49 saves of 19 Oct render (`game/analysis/play1019/native/`,
  `scores.txt`), mean difference 18.6. Plain scenes and fights 2-10 (the fight 185602: 9.8, only the HUD
  missing). Worst: white-out/flash frames (191358: the game's frame is all white), the light pillars and the
  sand-cloud flash (native too weak), cutscene letterbox bars (2D), the mirror's light beam (191059: no
  type 0x18 draw in that frame, so another path). Nothing broke since 17 Oct.
- **VU1 program inventory** (`tools/vu1inventory.py ELF [--native DIR] STATES`: each run of a save's last
  chain attributed to an ELF program by tracking MPG uploads; `game/analysis/play1019/vu1inventory.txt`,
  `play1017/vu1inventory.txt`). Numbered by ELF order (P0..P10):
  | Program | ELF | Runs per frame | Draw path |
  |---|---|---|---|
  | P1 | 0x4CDE8C | 500-3,800 | static world + lightmaps (driver 0x4CDE80) |
  | P4 | 0x4D548C | 7-520 | characters, first pass |
  | P7 | 0x4DD30C | 25-650 | characters, second pass (ALPHA 0x46) |
  | P8 | 0x4E0F0C | 8-300 | some characters (MSCAL 0x7F0) |
  | P5 | 0x4D948C | 6-86 | follows the GFX particle count |
  | P9 | 0x4E338C | 0-48 | 2 per CJadeSpriteElement draw |
  | **P6** | **0x4DAC8C** | 40-60 | **water** (all 22 water saves of 17 Oct run it; 3 more saves do too) |
  P0, P2, P3, P10 never ran in 100 saves. Earlier notes' "VU1 program 7" for water = P6 here (counted from 1).
  **Why (26 Oct, static, from the ELF + prototype symbols; prototype driver labels follow the same order):**
  P0 = `ps2MATH_VU0_MicroCode` (VU0 maths: `VU0_CompteDirLight`, `VU0_ComputeParticules`) and P2 = the VU0
  driver (`FREE_SHAPE_BOUNDING_BOX_CHECK`; `FUN_004a2200` sends it down VIF0 at graphics init) are **VU0** programs,
  in the list only because they load at 0 (the game's VCALLMS sites are at 0x495300-0x495C18). P3 = `MULT_CHANNEL_DRIVER`, driver slot 3 in
  `Vu1TagInit` (entry 0x196 like slot 0), **unreachable**: `ExchangeVu1Code` is only reached via `DoSetVUProgram`,
  whose fixed callers pass 0/1/4/5/6/7, and the per-draw table at scratchpad 0x700000B0 (filled only by
  `FUN_004b7640`) holds slots 0/1/2. P10 (+ the small MPGs at 0x400/0x200) = the SCEE screen shrink/clear packets
  (`gvSceeDispShrink*`, `DoShrink`, `DoClear`), 0x4E4740-0x4E5200 with no reference from code: dead library data.
  So nothing left to port on VU1 after P5; `vu1inventory.py` would still flag any of them in a save.
  The native renderer already covers what P1, P4, P7, P8 draw, i.e. nearly all VU1 runs.
- **Prize measurement** (`POP_NATIVE_SKIP=static,skinned,lightmap,gfx|all`, measurement only: the covered
  paths return without building DMA, so the picture is wrong): `tools/windows/oct19.ps1 -Native` measures fps
  per group on the fight and slow saves, profiles the fight with nothing/everything skipped, and records VU1
  traces (`PS2X_VU1_TRACE`, already in the runtime: registers + VU data per run) of two water saves as inputs
  for a P6 port.
- **Results (PC, 19 Oct 21:31-21:57, TV on; `game\analysis\native\prize\fps_<group>.txt`, 8 saves: the
  fights 185602/185837 and the six slowest saves of 17-19 Oct):**
  | Skipped | Fights | Other slow saves |
  |---|---|---|
  | nothing | 46-56 | 47-60 |
  | **static** (non-skinned CJMeshElement + CJMeshElementRef + CJIdxMesh) | **60** | **60** |
  | skinned (only calls that switch no driver; most character calls stay) | 48-55 | 52-60 |
  | lightmap (CLMElementList::Render) | 49-60 | 33-60 (one 9 fps second) |
  | gfx (FUN_00458FC0 particles) | 44-53 | 48-60 |
  | all | 60 | 60 |
  Fight profiles (`game/analysis/native/prize/prize_{none,all}.report.txt`): nothing skipped, VU thread 88%
  busy and the game thread 46% in sceGsSyncPath; all skipped, VU thread 53% (the kept character draws), game
  thread 35% idle in processDueDeadlines (waiting for vblank), PS2_Flip 8.6%. **The static world's VU1/GS work
  is the fight dips;** native static geometry alone gives a steady 60.
- **Pitfalls hit on the way** (also in `docs/lessons.md`): (1) the mesh Display paths switch the VU1 driver
  (CVuManager::DoSetShadowDriver / FUN_004B7480, state byte 0x700000a8) before skinned draws, so the skip keeps
  any chain that would switch; (2) with the PC's TV off the GPU has no active display and every boot runs at
  ~4 fps (OpenGL Present stalls): the first two evening runs were invalid; (3) the VU1 trace counts GS fields,
  which a save load restores to the save's field and which advance only with presented frames.

## Display stage: the game's line blend, 480i vs 480p (26 Oct 2026)

Settled so it is not re-asked (all static reading of the retail ELF + 51 saves; no boot):
- **The game blends each line with the next itself.** Its flip routine `FUN_004a3fc0` (called from `PS2_Flip`
  0x4A20C0) writes PMODE = (ALP << 8) | 0x23 (both read circuits on, MMOD: fixed ALP) and DISPLAY1/2 one raster line
  apart, both reading the same frame buffer: output = ALP/255 x line y (circuit 1) + the rest x a neighbouring line
  (circuit 2: 480i DY 0x33/0x32 -> line y+1; 480p DY +0/+1 -> line y-1). ALP = `+0xbb4` of
  the graphics object (127 in all 51 saves: PMODE 0x7f23, DY 51/50, MAGH 3, DW 2559, DH 447), or **255 (no blend)**
  when bit 0 of its second argument is set (flag from its callers `FUN_004c9c80` / `PS2_Flip`; meaning not read yet).
- **480p exists and changes nothing we compare.** `FUN_004a3080` (video mode; callers `FUN_004a3740`,
  `FUN_004b4280`) calls `sceGsResetGraph(0,1,2,0)` NTSC, `(0,1,3,0)` PAL or `(0,0,0x50,1)` 480p (progressive flag at
  gp+0x77c). NTSC and 480p both use a 640x448 double buffer; nothing outside the mode switch, the screen-position
  settings and the flip reads the flag. The flip's 480p branch sets the same ALP blend with the neighbour on the
  other side (line above instead of below: a half-line shift, not a different look). So the rendered frame, the
  post-processing and the blend are the same; otherwise only TV timing differs. All our saves
  are 480i and fully valid as references. How 480p is switched on in the game: not looked up (not needed).
- **What Windows shows today** (sotc runtime `GSCpuBackend::PresentFromLocalMemory`): the whole frame buffer every
  frame (no fields), the two circuits merged with PMODE's ALP and the DISPLAY offsets, one progressive picture
  scaled to the window. That already equals 480p output; forcing 480p would at most move the picture and would
  feed the runtime timing values it has never seen. Not worth doing.
- **For 0.2:** the native presenter (GS off) must do the same circuit merge from the game's registers (PMODE,
  DISPLAY1/2, DISPFB1/2; ~half a day, estimate), including the ALP 255 case once its flag is understood.
- **For the real-PS2 reference:** GS memory readback over ethernet checks every game pass (post-processing is GS
  blits into GS memory); the circuit merge is a formula from the registers above. No capture card needed; one
  would only show the console's analogue encoder (not the game's doing, not something to copy).

## Plan of attack (agreed with the user 19 Oct night)
Renderer, in order:
1. **Native static world in the game window** (DONE 22 Oct: `POP_NATIVE_WORLD`, on by default in play.ps1; see
   "Native world in the game window" below): in-process Vulkan drawing (the live window exists only as a
   separate process today; recommend porting the presenter to Vulkan over GL interop), the static draw paths
   no longer build DMA (as `POP_NATIVE_SKIP=static`, keeping the driver switches), the emulated GS draws the
   rest on top. The 60 fps win. Open design point: depth for GS draws over the native world (characters and
   particles test against it) - step 2 makes that moot.
2. **Characters and particles native** (decoders exist; NEXT, see CLAUDE.md status): the GS stops drawing 3D.
3. **Gaps**: water (P6 on VU1: Qwen port judged by the trace replayer), sprites (P9), shadows, halos, light
   shafts / the mirror beam (191059: not type 0x18), full-screen flashes and white-outs, UV animation details.
4. **2D**: HUD, menus, text, loading screens, letterbox bars, the movie picture.
5. **GS off by default** (reference renderer toggle).
Sound: (1) sound in save-state boots (enables offline sound work from saves); (2) map the DARE/UbiSound calls;
(3) native mixer (ADPCM, ADSR, pitch, volume/pan, streams, reverb; OpenGOAL ISC as reference); (4) dual render
against the SPU2 emulation + glitch detectors + every-sound-alone overnight; the known audio bugs checked on both.
Then: IOP (pad via SDL incl. rumble later, memory card as native files; the IOP goes once sound is native),
native frame pacing (EeScheduler/vsync overhead ~15%), 1.0 at 30 fps.
Playtesting: renderer 1-2 low (existing saves); 3 medium-high (saves of later levels' effects); 4 medium
(saves on every menu/screen); sound high until save-state boots have sound; final full playthrough with the QA
checks. **Agreed: a data-gathering playthrough on the current build soon** (quick-saves at every new
room/effect/menu/cutscene, in chunks; each batch is scored/inventoried offline), then the user checks
milestones, then one verification playthrough at the end.

## Native world in the game window (renderer step 1, built 19-20 Oct night; first booted 22 Oct)
Design chosen over porting the presenter: **GS-memory injection**. The emulated GS keeps presenting; the native
static world is drawn by our Vulkan renderer in-process and written into the GS frame and Z buffers at the
point of the frame where its 3D starts, so everything the GS draws afterwards (characters, particles, decals,
2D, post effects) lands on top and depth-tests against the native world.
- Switch: `POP_NATIVE_WORLD=1` (off by default; the current build is unchanged without it). Needs runtime
  patch 38 (`GSHostInject`, pop-sotc only) and the play build linked with `pop_native_render` + volk
  (`rebuild.ps1` does both: `PS2X_RUNNER_EXTRA_LIBS`). Without patch 38 the mode reports itself off.
- Game side (`pop_native_live.cpp`): opaque static draws (CJMeshElement, CJMeshElementRef, CJIdxMesh of the
  first lists, plus lightmap passes over natively drawn elements) are recorded and their Display calls
  skipped (driver switches kept where the chain is not all static). The frame's first Display3DElements puts a
  marker in the DMA list (VIF FLUSHA + DIRECT, GIF A+D to GS register 0x7F with 'PNWD' + frame number); at
  the DMA kick the frame's native draws go to the runtime side as **PNF7** (textures by key with pixels once,
  each batch by a 64-bit key with geometry once: static = conversion serial + batch + vertex variant,
  instanced = content hash).
- Runtime side (`pop_native_world.cpp`): a worker thread draws each frame offscreen (640x448, colour + GS Z as
  Z24/Z32 like the VU1 programs write it; the renderer keeps meshes on the GPU, 512 MB budget, dropped with an
  epoch bump that also clears the sent set); the hook replaces the marker with two `uploadImageNative`
  transfers (FRAME PSMCT32 and ZBUF PSMZ24/32, fbw 10). It waits up to 250 ms for the frame's picture; a
  frame without one draws without its static world (logged).
- **Offline results.** Mac `tools/native/world_saves.sh` + `world_check.py` over the 49 saves of 19 Oct: the
  world-only Z is never in front of the full native frame's ("extra" 0.00% on all 49;
  `game/analysis/play1019/world/world.txt`). PC (`oct19.ps1 -World`, fight save 185602; RTX 5070 Ti):
  1,760 draws, 46k triangles; GS Z from `pop_world_test` byte-identical to `pop_native_view`'s; worker
  2.66 ms per frame (read 0.88, upload 0.94, draw + readback 0.84), **~3 ms from submit to the uploads**
  after the first frame (20 ms: all geometry and textures sent). Full-frame scores after tonight's renderer
  changes (`score_saves.sh` -> `game/analysis/play1019/native_pnf7/`): mean 18.64 -> 17.98 over the 49 saves;
  44 better, 5 worse by 0.01-0.07 (185052, 185102, 185301, 191317, 192114). The change comes from d23ebe9
  (GS-exact depth, half-pixel sampling offset), not PNF7 (byte-identical on the fight save). PNF7 cut the frame from 8.2 MB to 79 KB and
  the wait from ~9 ms. The play build with the mode compiles and links.
- **Known risks, to check in the first boot:** (1) the hook runs on the async VIF/VU thread
  (`asyncvif::submitGif` -> `GS::processGIFPacket`), so it blocks the VU thread for those ~3 ms per frame
  (with the static world skipped the VU thread was 53% busy in the fight, so it should fit 60 fps; measure);
  first frames after loading new areas pay the full upload (a hitch of ~20 ms+). (2) The upload overwrites
  the whole frame and Z: anything the GS drew before the marker in that frame is lost. Checked on the fight
  save's last chain (offline, 20 Oct): before the first VU program call there are only the frame setup, the
  clear (PRIM sprite, RGBAQ 0, Z test ALWAYS, 20 strips) and texture uploads, so the upload's colour 0 / Z 0
  equals the game's own clear. Other areas (menus, letterbox) not checked. (3) Pixels with no native draw
  get colour 0 / Z 0 (= the clear). (4) Nothing was ever run with a real game window; Vulkan and the GS's
  OpenGL share the GPU for the first time (separate devices, no interop).
- **First boot (morning, TV on, user present):** on the PC, the fight save with and without the mode,
  compare fps and pictures:
  `tools\windows\boot.ps1 -Play -Out game\recomp-sotc -Seconds 60 -Env 'POP_60FPS=1;POP_NATIVE_WORLD=1;PS2X_STATE_LOAD_AT=120:states/play_20261019_185602.state' -Shots 600,1200,2400`
  then the same without `POP_NATIVE_WORLD=1`. Log lines `[pop:world]` (every 600 frames: frames drawn,
  missing, batches sent/known, wait times).
- **First boots (22 Oct, user on the TV).** (1) From the fight save: nothing drawn natively ("loaded 0
  elements ... never loaded"): a save-state boot never runs ReadElement, so nothing is converted. Test from
  a normal boot (title -> memory card), not from a save. (2) Normal boot (`play.ps1 -Env 'POP_NATIVE_WORLD=1'`):
  857 elements converted (83k triangles), ~400 native draws per frame, 59.2-59.9 fps, worker 3.4 ms, GS thread
  waits 3.3 ms; responsiveness like the GS path (user). The world was missing after loading in and in the
  fixed vault camera: 1,789 frames refused as "unexpected buffers" (ZBUF ZMSK 1). Saves 171143/171355: the
  last state before the marker is ZBUF ZMSK 1 + ALPHA 0x44 with no VU1 run before it; the game sets ZMSK 0
  right after the marker. Fixed (bd89bd4: the write mask is not checked); retest: 9,600 frames injected,
  0 missed, 0 refused, world there at once and through vaults (user). (3) Shadows (lever, save 172041)
  flicker more than on the GS: the native Z is nearer than VU1's on 26.7% of world pixels (49 saves of
  19 Oct, 2.9M pixels: p50 -19, p90 +15, p99 +106, p99.9 +442 Z24 units), so GEQUAL second passes fail
  there. `POP_NATIVE_WORLD_ZBIAS` (default 128) pushes the injected Z back; a cheap fix for the hybrid
  period only (native shadows come in step 3). In game (22 Oct evening, A/B 128 vs 0): flickers less with
  128, no clipping seen (user); kept as the default.


## Native characters in the game window (renderer step 2, first part; built 22 Oct, offline only)
Switch: `POP_NATIVE_CHARACTERS=1` with `POP_NATIVE_WORLD=1` (`play.ps1 -NativeCharacters`); off by default until
judged in game. Off reproduces step 1 exactly (all 49 saves of 19 Oct: the same `world_check.py` numbers).
- **Game side** (`pop_native_live.cpp` `display`): the CPU-posed skinned draws (`poseSkinned`, already made
  every frame in world mode and thrown away) are world records (kind 1); a CJMeshElement::Display call whose
  chain is all recorded (opaque world lists 200/+0x2C/+0x3C, as step 1) is skipped. Skipping a skinned call
  must keep its **VU1 driver switch** (the 19 Oct ~7 fps lesson): decompiled, both switch functions are leaf
  functions, `CVuManager::DoSetShadowDriver` (0x4B5BC0, used when word 0x700000AC = 2) and FUN_004B7480
  (otherwise), each appending one 2-qword packet (CNT 1; FLUSH; UNPACK V4-32 to VU 0x1D resp. 0x1E: 0x8000,
  0x303EC000, 0x412, driver index) and storing the index at 0x70000080. Display switches when byte
  0x700000A8 (0 one-bone driver, 1 more) differs from the palette's word (< 2: one bone), moving the index
  by -/+ the word 0x700000AC. `worldReplayDriver` does exactly that per skipped chain element, in order.
  Also from the decompile: Display draws nothing and does not follow the chain (+0x4C) when the element has
  no packet for the pass (+0x20 + 4 * pass, pass 4 -> 1); the hook's chain walk now stops there too (it used
  to record such elements).
- **PNF7**: batch key 0 = this frame only (posed geometry changes every frame), always with its data; the
  loader adds it as an ordinary per-frame mesh (dropped by `reset(true)`), not a kept one.
- **Offline** (`POP_NATIVE_CHARACTERS=1 tools/native/world_saves.sh`, 49 saves of 19 Oct): world coverage mean
  69.6% -> 71.4%, "other native" 4.3% -> 2.4% (the rest is step-1 leftovers: blended statics, Z-writing draws
  of later lists), "extra" 0.00% on all. Saves whose camera matches (neither 0%): 25-125 skinned draws native
  each, "ahead" <= 0.02%, so posed characters never stand in front of the game's own surfaces. Calls left
  on the GS are all for render state (list +0x88 translucent, blend kind 1 with local alpha: fading
  enemies); none for decoding or textures (`POP_NATIVE_WORLD_WHY=1` lists them; the 300-frame
  `[pop:world]` line counts them).
- **Known differences, to judge in game:** lighting is the 16 Oct approximation (ambient + directional
  lights, no point lights: "Character lighting live" above), so characters come out a little paler/whiter
  than the GS's (save 185803 with post effects: close, trousers whiter); skinned second layers (pass 2) and
  translucent characters stay on the GS and draw over the native ones with the world's Z bias; posing costs
  game-thread time (it already ran in world mode); posed geometry is uploaded every frame.
- **First boot (TV on, user present):** normal boot (title -> memory card; save-state boots convert no
  world but characters decode from memory), `play.ps1 -NativeCharacters`, A/B against `play.ps1`: fps in
  fights, character look, the driver-switch count in the log (`driver switches replayed`), no ~7 fps drop.
- **First in-game sessions (22 Oct evening, user on the TV, `play.ps1 -NativeCharacters`):** 59.9 fps over
  21,600 frames, 0 missed; ~70 skinned draws native per frame, 172k driver switches replayed, no slowdown;
  worker 3.44 ms (3.40 without characters), 293 KB per frame (66 KB). User: frame rate good; (1) the
  Prince's white trousers flicker a little (minor; not investigated yet: candidates are the blended layers
  left on the GS over native characters, Z bias, or lit colours at the clamp); (2) **the Prince's whole body
  lit up briefly, when walking towards the camera.** The flash detector (`[pop:flash]` lines; per posed
  draw, a >35% jump of the mean lit colour from the frame before, logged with both light sets;
  `POP_NATIVE_FLASH_SAVE=1` quick-saves) found it: light set 1 (+1 = 0) had a slot whose +0x10 vector was
  (1.6e29, 0, 0), read as a directional light, so any normal with +x lit to white.
- **Light sets, decompiled (`CVuManager::DoSetSkinLightEnv` 0x4B64C0):** sets with +1 != 0 or no lights:
  `count` slots as directional lights, +0x10 vector, +0xD0 colour, no type check (as the code did). Sets
  with +1 == 0 (the characters' sets in play): per slot type at +0xE0: type 1 = directional with its unit
  direction at +0x10 (uploaded to VU 0x3FC; the main slot gets a zero vector with w = type << 4); type 0 =
  point light, position at +0x50 (w 1), falloff pair at +0x90 (e.g. 1, -0.00163; 1.21, -0.0343), colour
  +0xD0; its +0x10 is left over. `readLightEnv` now lights typed sets with the type-1 slots only; point
  lights are left out until the VU1 falloff is decoded (the program that reads VU 1020-1023 in
  vu1_527ef67b uses them as a matrix with a point at 1019: a projection, not this light loop). Save 192521:
  the Prince's mean brightness 68.2 -> 65.3 (GS 73.7 with glow).
- **Noticed:** the Prince's sword (on his back) is in the GS picture but not in the native frame (save
  192521): another draw path, still drawn by the GS in game.
- **Third session (after the fix), user: flash fixed, trouser flicker not seen (watch).** 58.5 fps mean, one
  38 fps one-second sample (unattributed). New: **a full-frame flash right after the loading glyph** when
  entering a scene (consistent); to check with `play.ps1 -NoNativeWorld` whether the game does it too. Native
  characters made the default in `play.ps1`; the flash detector is opt-in now (`POP_NATIVE_FLASHLOG=1`).
- **Load flash is ours:** the same scene entry with `play.ps1 -NoNativeWorld` (GS only) does not flash (user,
  22 Oct). So the first frame(s) after a load with the native world injected show a full-frame flash.
  Candidates: the frame(s) between the loading screen and the first 3D frame (marker present but the picture
  from a frame drawn before the new area's geometry arrived, or the clear colour 0 / Z 0 over a fade the game
  draws before its 3D), the kept-mesh reset when a new area loads. To do: capture the frames after a load
  (`boot.ps1 -Shots` around a scripted load, or a frame dump of the injected pictures) and compare.
- **Point lights decoded (22 Oct evening)** by replaying the character VU1 program (image 31829861, 2,820 runs
  in the trace of save 192521, `tools/windows/vu1trace.ps1`) with changed inputs (`tools/vu1port/lightprobe.cpp`;
  build like vu1replay, x86_64): all other lights and the ambient off, the point light's colour 200, then the
  falloff pair varied. Output vertex colours: linear in a, linear in b, additive (a 0.5 + b 1e-3 = 69 vs
  44 + 24), clamped to [0, 1] (a 2 = a 1, a -0.5 = 0), the recorded pair reproduced to 1-2 levels (48.9 vs
  48), the b factor = d^2 to the light (Prince at (-12.3, -16.4, 3.5), light at (-5.0, -3.8, 10.6): 262 vs
  ~273 measured), no-falloff contribution <= the colour (196 of 200: unit direction). So a point light adds
  colour * clamp(a + b d^2, 0, 1) * max(0, n . unit(pos - vertex)); (a, b) = near/far falloff, a =
  far^2/(far^2 - near^2), b = -1/(far^2 - near^2) (seen: far 24.8; near 2.47, far 5.94). Now in
  `poseSkinned`. Save 192521: the Prince's mean 65.3 (no point lights) -> 67.6 (GS 73.7 with glow).
  Qwen candidate (user asked): the whole character program (2,048 lines, ~60 stages at the water job's
  25-35 pairs) to C++, judged on this trace: exact lighting incl. any other light types.

## The Prince's missing sword (22 Oct night): a lit static mesh

Found with skip boots of save `play_20261022_192521` (PC, TV on; `tools/windows/oct22-sword.ps1`, shots in
`game/analysis/native/sword/<group>/`; plain GS, `POP_NATIVE_SKIP` per group, measurement only):
- gfx, lightmap, CJIdxMesh, sprite, water, list18 (FUN_00469C00): the sword stays.
- skinned with `POP_NATIVE_SKIP_REPLAY=1` (every palette chain skipped, its VU1 driver switches replayed as
  `worldReplayDriver` does): the Prince and Farah vanish, **the sword stays** (plus two small wrist parts).
  Without replay the skip is leaky (chains that would switch the driver are kept), so the first run said nothing.
- static: the sword vanishes. `POP_NATIVE_SKIP_RA=<hex>` (one caller only): from 0x4B98A0 (CElementList::Render)
  it vanishes; from the shadow-list renderer FUN_004B80C0 (its two Display calls return to 0x4B8220 / 0x4B84A0,
  `POP_NATIVE_SKIP_CALLERS=1` lists callers) it stays.

The element: **0x00B395B0** (list +0x2C, matrix 126 at (-12.08, -16.33, 3.61), next to the Prince's skinned parts
at matrices 960-966; chained with 0x00B3A2C0 / 0x00B3A4B0), passes 1 and 2, no bone palette. Its pass-1 packet is
static-style (ref tags, RET) but **lit**: per batch a V3-16 UV upload to VU 0x43, then one shared block with
V4-8 **normals** at 0x83, **V4-32 positions** at 0x3 and a V4-32 x3 header at 0 (pass 2 reuses the block with
its own UVs). So pass 1 here is the lit driver (as the characters' pass 1), on a static mesh.
- In a normal boot ReadElement's `convertElement` accepts it (format 0xC at addr 3 = positions) and the world
  path draws it natively as an unlit static mesh, reading the normals as colours (first vertex 7D 11 03 01).
  **In game it was not missing but reddish-brown** (user, 22 Oct 20:37, old build; save
  `play_20261022_203744` shows the orange-red stripe in the game's own frame); silver again with the fix
  (20:40). The "missing" sword was the offline frame only:
- Offline (`pop_state_frame`) `livePacketBatches` closes batches on V3-32 positions only, so it decodes nothing.

Related, fixed offline: rigid parts with a skinned packet but no palette (0x018421B0 / 0x018426F0, the wrist
pieces; 0x00B3AAC0 + glow pass 0x00B3BBD0 at matrix 1, likely the Dagger) were decoded from memory only with a
palette; the from-memory skinned decode now also takes palette-less elements whose packet starts with
cnt/ret/end tags (posed by the object matrix, as poseSkinned already did). `POP_NATIVE_ELEMLOG=<hex>,...` prints
what the Display hook records for chosen elements (path, list, matrix, posed box, material).

Step 1 (same night): static pass-1/4 draws stay on the GS (below); native lit statics are renderer step 2.

## Static world audit (22 Oct night)
Is renderer step 1 complete? Two offline measures over all 196 saves on the Mac (`game/analysis/play10*/states`):
- **Layout census** (`tools/staticcensus.py <dirs> [--elements]`): every palette-less CJMeshElement and
  CJMeshElementRef entry by pass and upload layout (format@VU address per batch), and what the V4-8 at 0x83
  holds. **Pass 0 always has colours there; pass 1 always normals** (V4-8 with a spare 0/1 byte, or V3-8). Plain
  static (V3-32 positions) is 69.5k of 77k draws; objects with V4-32 positions to VU 0x3 (their own matrix:
  the mirror-room puzzle pieces, gold arcs, frames) come with colours in pass 0 and a second layer in pass 2
  (V4-8 at 0x43, not decoded). Lit pass 1: the sword (one ref-style mesh), 6 + 34 instanced refs, and 106
  inline rigid character parts (skinned-style packet, no palette).
- **World accounting** (pop_state_frame --world with `POP_NATIVE_CHARACTERS=1`; `POP_NATIVE_WORLD_WHY=1` now
  also logs each static/instanced call left undecoded).

Fixes from it (`pop_native_live.cpp`):
1. **Lit statics stay on the GS** (`isLitStaticPass`: CJMeshElement passes 1 and 4) unless the character path
   posed them; the converted path used to draw them unlit with normals as colours (the sword). Counter: "lit
   static draws on the GS" in the [pop:world] report.
2. **`livePacketBatches(..., v4Positions)`**: V4-32 positions at VU 0x3, as the load converter takes them, for
   pass-0 packets (static from memory, instanced pass 0/4) and lightmap pass 3. Instanced refs use this decoder in
   every boot, so these objects were always on the GS; in save-state boots the meshes were too.
3. **Instanced keys carry pass 2**: pass 0 and pass 2 of a ref shared one dedupe key (pass 0 first and native:
   the pass-2 call looked drawn and was skipped, losing the layer; pass 2 first: pass 0 never decoded).
4. **A ref without a packet for the pass ends the chain** as in the game (FUN_004C7B40: the recursion is inside
   the packet test); it used to count as undecoded and send the whole call to the GS.
5. Palette-less rigid parts with a skinned packet are posed from memory too (see the sword section).

Result over the 196 saves: undecoded calls 3,629 -> 1,044, all accounted for: 500 lit pass-1 calls (step 2) and
544 pass-2 second layers of V4-32 objects (format to decode); native calls 76,933 -> 79,530 (+107k triangles in
list +0x2C). The old/new comparison changed no static or instanced line before fix 2 (25 saves). Still on the GS
by design: list +0x4C opaque (671 draws over the saves; opened with the translucent lists in 0.12), +0x98 blend 0
(the alpha-only pass), 23 calls kept for textures. Before/after/game pictures of the mirror room: save
`play_20261019_191304` (gold arcs and mirror frames now native, matching the game; mirror faces get their
reflection from the pass-2 layer on the GS). **To check in game from a normal boot (title -> memory card):** the
sword back, the mirror room unchanged to the eye, `lit static draws on the GS` in the log.
**Checked in game 22 Oct 20:39 (memory-card slot 04, the fountain after the mirror room):** sword silver, mirror
room right (gold pieces, faceted mirrors), 59.8-60 fps (lowest 59.4), 0 missed injections, worker 3.4 ms,
~12-15 lit static draws per frame on the GS. Saves `play_20261022_204040` / `204108` (mirror room, new build).

## Next: lit statics native (plan, 22 Oct night; renderer step 2, roadmap 0.12)
The first part of step 2's rest, before translucents + particles: the static draws of the lit driver, which the world
mode now leaves to the GS (`isLitStaticPass`; counter "lit static draws on the GS", ~12-15 per frame in the mirror
room). 500 calls over the 196 saves (`tools/staticcensus.py game/analysis/play10*/states --elements` lists them):
- **CJMeshElement pass 1, ref-style packet:** the sword (0x00B395B0, one element, in 156 saves). Per batch V3-16 UVs
  at VU 0x43, then a shared block: V4-8 normals at 0x83 (xyz signed, fourth byte spare 0/1), V4-32 positions at
  0x3, V4-32 x3 header at 0. In game it is converted at load (`g_elements`), so the fix sits in display()'s
  `isLitStaticPass` branch: draw the converted geometry lit instead of leaving the call.
- **CJMeshElementRef pass 1:** 6 elements with the V4-8 normal layout, 34 with inline V3-8 normals at 0x83 + V4-32
  positions (`refDisplay`; `livePacketBatches` decodes neither, so they go to the GS). FUN_004C7B40 (decompiled in
  `game/analysis/naming/decomp/004c7b40.c`): a ref whose element has no palette uploads the matrix-pool entry
  (object matrix) before CALLing the packet.
- (Palette-less rigid character parts with skinned-style packets are already posed and lit by the character path.)

Approach: decode normals with the positions (a lit variant of `livePacketBatches`, or the converter keeping
normals for pass-1 elements), then light like the characters: `poseSkinned`'s no-palette path (bones[0] = the
pool entry) with `readLightEnv` (light set from the list entry, scratchpad 0x88). **Open before coding:** which VU1
program draws these (VU1 inventory / the trace of save 192521, `game/analysis/vu1trace/`) and whether its lighting is
the character formula (directional + point lights, `tools/vu1port/lightprobe.cpp`) or a variant; measure, don't
assume. Judge offline: `pop_state_frame` (`POP_NATIVE_CHARACTERS=1`) against the game's frame on saves
`play_20261022_192521` (sword, from behind), `204040` / `204108` (mirror room), and the guards' props
(`--elements` list); then in game: memory-card slot 04 (play time 0:51, the fountain after the mirror room), sword
silver and lit as on the GS.

Left after that (also 0.12): the pass-2 second layers of V4-32-position objects (544 calls: V4-8 at 0x43, meaning
not decoded; the mirrors' reflection), then the translucent lists + particles (status block in CLAUDE.md).

## Lit statics measured (22 Oct, late night): which program, which formula

Measured on the VU1 trace of save 192521 (`game/analysis/vu1trace/`) with `tools/vu1port/inprobe.cpp` (new; build
like vu1replay: replays one recorded run, then once per VU data quadword with that quadword changed, and lists which
output words change; `--match <hex word>` picks the runs whose uploads hold a mesh's first position, `--dump`,
`--out`). The sword's first position word is 3cccfe09; it runs three times per frame:
| Run | Program | Data | What it is |
|---|---|---|---|
| record 1809 | P4 (image 31829861, the character program) | pass 1: V3-16 UVs +0x43, V4-8 normals +0x83, V4-32 positions +0x3 | lit: reads the light block (qw 0-3 directions/point position, 5/7, 16-18 colours, 22 falloff, 29 ambient) and the normals |
| record 2034 | P4 | pass 2: V4-8 **normals at +0x43**, V4-8 colours at +0x83 (40 47 4A 80 live) | **environment map**: no light input; colour = the V4-8 at +0x83 as is; UV from the normals |
| record 2548 | P7 (image b720d191) | pass 1's packet | the shadow-list draw (FUN_004B80C0): every vertex colour = qw 21 (128,128,128,~53), normals unused |

- **Pass 2's env map** (checked by hand on the first vertex, output STQ / q): u = 0.5 + n . (qw4.x, qw5.x, qw6.x),
  v = 0.5 + n . (qw4.y, qw5.y, qw6.y), n = the V4-8 normal / 128. qw 4-7 come from **FUN_004B6D80** (called by
  CElementList::Render before the Display of an env-mapped pass, sub-type 2): V2-32 x4 to VU 4 = -0.5 x the xy
  columns of R = M3 x C3 (row vectors, FUN_00494500), then (0.5, 0.5); M = the entry's matrix-pool rotation
  (translation cleared), C = the camera matrix at *(gp 0x6749F0 + 0x110) + 0x1B0. Checked against every env
  upload in the chains of saves 204040 / 204108: 230 of 233 match a pool entry within 1e-4 (the rest, and the
  sword in 192521 at 0.025, are moving objects: the chain and the pool are from different moments).
- So the **544 pass-2 "second layers" of V4-32 objects (the mirrors) are the same env map**, not a separate job.
- The chain-side attribution (MSCAL/MSCNT under the program in the save's VU1 code memory, scratch script) said
  pass 1 = P7: wrong, the save's code memory is not what ran then. Trust the trace's image hash.
- Implementation: pass 1 = the character lighting (`poseSkinned` no-palette path + `readLightEnv`); pass 2 =
  decode normals at +0x43 and colours at +0x83, UVs as above per vertex (depends on the camera: per frame, not at
  load). Judges as in the plan.
- **Built (22 Oct, late night), offline results.** `driverKind()` / `litDriverPass()` (the game's driver table), `litPacketBatches()`
  (batches split at the VU1 kicks, strip flag = the normal's fourth byte), `envUvs()` (FUN_004B6D80's matrix per draw), and a
  display() branch before the unknown-element path: pass 1 posed + lit by `poseSkinned` (no palette: the pool matrix),
  pass 2 posed with env UVs and the packet's colours (the material's constant colour/alpha applied by writeVertices as for
  every draw; the sword's pass 2 gets 0x4A4740 = the trace's 40 47 4A). `POP_NATIVE_NOLIT=1` = the old behaviour (A/B);
  world stats print "native lit/env". `POP_NATIVE_ELEMLOG=all` logs every element. Scores (`score_saves.sh`, characters on):
  204040 13.04 -> 11.28, 204108 5.02 -> 4.25, 192521 4.70 -> 4.70. Seen: the ceiling's gold frames and horns, the mirror
  rings and the faceted hanging mirrors now drawn (were missing); the sword drawn silver-blue with env highlights.
  Then (same night) the **instanced refs** too (refDisplay: the ref's element picks the driver; posed by poseSkinned into
  g_frameRefs with an identity model) and **V3-8 normals** (most refs' pass 1; strip flag from the UV's third value);
  pass-2 colours no longer clamped at 0x80 (GS modulate). Offline counts (`[state] lit static passes native`): 192521
  lit 1 / env 1 (the sword), 204040 13 / 76 (incl. the 12 lit refs at (-2.7, 54.6, -34)), 204108 0 / 83.
  Open: (1) the faceted mirrors looked darker than the game's when zoomed, but region means match (69/77/75 vs
  68/73/69): the facet-to-facet pattern differs a little; (2) the gold frame of the angled mirror (204108, top right)
  is missing before and after: not a lit static (no pass-4 draw in any 22 Oct save), another path (translucents?);
  (3) the mirror-room light beam is a light shaft (not native yet: 0.13); its glow differs before/after; (4) save 192521's
  game frame shows the orange-red sword of the 19:25 build (native injection with normals as colours), so it is no
  reference for the sword: judge the sword in game (memory-card slot 04).
- **Mirror flicker fixed offline (22 Oct, ~22:00):** in game the mirrors flickered in first person while turning (user; save
  `play_20261022_214633`); offline the additive gold horns (env pass 2, list +0x3C, flags 00244207) were missing in that
  view. Cause: the env layer was posed on the CPU into world space, the base pass is drawn in object space with the model
  matrix on the GPU: different float paths, so the second layer's depth sat a hair behind the base and failed the depth test
  (always in one view, on and off while turning). Now lit/env passes keep object-space positions (lit in world space for
  the colours only: `poseSkinned(..., objectSpace)` via `litPose`) and go to g_frameRefs with `poolMatrix(index)`.
  Scores: 214633 8.14 -> 4.85, 204040 11.28 -> 11.21, 204108 4.23 -> 4.22. Left in that save: the centre faceted mirror
  greyer than the game's, one facet of the right hexagon darker.

## Next: translucent draws + particles (plan, 22 Oct ~22:00; rest of renderer step 2, roadmap 0.12)
Where we are: static world, characters and lit statics (pass 1 lit, pass 2 env map, elements and refs) are native and
checked in game. What the GS still draws per frame (`pop_state_frame --world`, 108 saves, 22 Oct): ~136 mesh draws /
3.8k triangles, nearly all translucent (blend 1/4/6 in lists +0x3C, +0x4C, +0x88, +0x98, +0xA8; ~5 opaque in +0x4C),
~14 GFX particle draws (FUN_00458FC0) and ~6 sprite draws; shadows, halos and shafts are not replayed (0.13).
Plan: open the world mode (`worldStateOk`) to these lists in the game's draw order, together with the particles
(particles alone would be drawn over by later GS draws); offline over the saves first (score_saves.sh, A/B with the
old behaviour), then in game. Check on the way: the angled mirror's missing gold frame (save 204108, top right; not
a lit static), the load "flash" (one frame of a random texture, user 22 Oct: capture the frames after a load), the
two leftovers of save 214633 (centre faceted mirror greyer, one hexagon facet darker).

## Translucents before the shadows: POP_NATIVE_TRANSLUCENT=1 (23 Oct evening, offline)
**29 Oct: two rules added after the baths flicker (N13/N14/N16, `docs/native_bugs.md`):** a blended or Z-less draw
of these lists goes native only while the frame is pure (no earlier draw went to the GS; `worldStateOk`), and every
draw without depth write tests `POP_NATIVE_DECAL_ZBIAS` GS Z units nearer (default 32; mesh.vert, `fogColour.a`).
- **Prefix rule.** The injected picture is an image upload that replaces pixels, so only a prefix of the game's draw
  order can be native. Display3DElements: 200, +0x2C, +0x3C, +0x68, +0x78, +0x4C, lightmaps +0x5C (native), shadows
  (GS), +0x88, +0x98, +0xA8. So the lists after the shadows, and with them nearly all GFX particles (203 saves: 2,777
  of 2,854 GFX entries are in +0x88, the rest in +0x2C), wait for native shadows. Frame alpha (glow mask) needs nothing
  in the prefix: only +0x98 writes it.
- **Measured (203 saves, per frame):** can go native now (+0x3C, +0x4C; +0x68/+0x78 empty): ~53 GS draws / ~2,250
  triangles; after the shadows: ~89 draws / ~2,500 triangles + ~13 GFX particle draws.
- **Switch:** `worldStateOk` takes every draw of the prefix lists, any blend (the writer keeps the game's order by seq).
  Offline A/B (world frame vs the game's picture, `livecmp score`): mean 19.48 -> 18.50; 117 better, 19 worse, 67 same.
  Worst "regression" 18_202622 (+4.4) is a cutscene: the game's letterbox bars (2D, later) over our now-drawn floor;
  the floor there is a translucent layer over a reflected room (without the switch the reflection's pillars show).
  Real difference: 21_202624 (palace at night): the translucent cloud layers come out brighter/whiter than the game's.
- **The cloud layer (save 21_202624):** list 200 draw 3, flags `40010307` (blend 1, no Z write, AFAIL, UV scroll from
  sub +0x14), texture 66001738, vertex colour ~0.93 (0x80 = 1.0), vertex alpha ~0.31. In two cloud regions the game's
  picture equals our frame *without* the layer (48/68/79 vs 50/70/81) and ours with it is +7..+9 brighter: the game's
  clouds are elsewhere or fainter at that moment, so suspect the scroll offset (m[4] = sub +0x14 under 0x40000000) or
  the vertex alpha, not the blend. To settle: the GS-side cloud primitives of the same save (PS2X_GS_TRACE after a state
  load, or the VU1 trace) against ours. Until then `POP_NATIVE_TRANSLUCENT` stays off by default.
- **Settled offline (23 Oct night): the offline judge, not the native draws.** `pop_native_view --frame` used the game's
  draw order only for PNF4/PNF5; `pop_state_frame` writes PNF7, so offline frames drew all opaque draws first and all
  blended ones after. The cloud layer (draw 3) then landed on top of draw 4, the opaque horizon band drawn right after
  it (same UV scroll; ALPHA 0x29 with FIX 0 = Cs, ZMSK 0, constant colour by STROW 0x16/0x20/0x2A = its material's +8
  0x2A2016). The game window always used game order (`pop_native_world.cpp` sets `gameOrder`). Fixed in the viewer (PNF4
  and later = one pass in order). 21_202624: 6.16 -> 5.03 (translucents off 5.05).
  Checked against the save's DMA chain (scratch walkers over `statechain.chain_words`): the cloud's UV matrix (VU 0x15-0x17,
  scroll 0.792 vs ours 0.793 one frame later), texture (PSMT8 128x128 + CT32 palette uploaded in the chain: byte-identical
  to `jadetex` decode), vertex colour/alpha (alpha 0 or 60 per vertex, mean 0.31), GS state (ALPHA 0x44, ZTST GEQUAL, no
  alpha test, ZMSK 1, CLAMP REPEAT, FOGCOL 0xD1CB98), all as ours.
- **Static driver (image 9d41512c = e10ba587 = eea60abc for the first 1,424 dwords), read along the way.** MSCALF 0x196
  (byte 0xCB0) is the setup; each batch: header qw 0/1 = box centre/extent, culled whole if all 8 corners are outside one
  plane (0x530); qw 2 = the GIF tag sent; VU 17.w (the GIF tag template's last word) = mode: 1 = plain (0xEC8, or clip
  path 0x1090), 2 = UV matrix (0x18A8: u' = m0 u + m2 v + m4, v' = m1 u + m3 v + m5, uv = raw / 512), 3/4 other paths, 0 =
  a layout with positions at +0x43. VU 8-11 = screen matrix, 12-15 = clip matrix (both times the model matrix at VU 0-3),
  16 = fog (255, 0, a, b), F = a + b w. Per triangle (clip path): any vertex beyond +z (the far plane, w ~3,470 in this
  save) -> dropped; all three outside one of -z/+-x/+-y -> dropped; any vertex behind -z or outside the 0..4095 guard band
  -> Sutherland-Hodgman clipper (0xA88/0x840, colours and UVs carried); else drawn. So a native far-plane cull would only
  matter for geometry beyond ~3,500 units.
- **A/B with the fixed judge (23 Oct night, 201 saves, characters on):** mean 19.24 -> 18.23; 108 better, 14 worse, 79
  same. The worst three are cutscenes: letterbox bars (2D, 0.14) over our now-drawn floor (18_202622 +4.4, 20_213411
  +1.7) and 19_192022 (+1.8, a cutscene camera unrelated to the save). **On by default in `play.ps1`** (`-NoNativeTranslucent`
  turns it off); waiting for the user's check in game.

## Load flash: what the GS traces show (23 Oct evening; runtime patch 40, POP_DISPLOG_TRACE)
Traces of the 8 fields after the first save's load, GS only (`loadtrace/gs/load2.txt`) and native
(`loadtrace/native/load2.txt`; the second save was not traced: trace slowdown made the 300 ms load detector fire
again). Same scene, the same stale camera on the first 2 frames.
- **Every normal frame** starts with a clear (20 sprites, black, z=0, ZMSK 0), then the 3D (ZTST GEQUAL), then the post
  effects. **Frame 1 after a load has no clear**: its 3D (z ~79k) is tested against whatever Z the loading screen
  left. Likely why it comes out black on the GS (user: "black for exactly the flash's length"); unproven without pixels.
- **Native frame 1: our picture goes into the wrong buffer.** The marker's FRAME is fbp 0x8C (stale), so the picture and
  Z are uploaded to 0x8C (`T dst=1180` / `dst=2300` at the top of tick 1942), but the game draws all of frame 1 (rest of
  the 3D, post effects) into fbp 0, and 0x8C is the buffer on screen (V tick=1942 dispfb1=148c). So the stale-camera
  picture is written straight into the displayed buffer: the flash. (The "both buffers" test also wrote it into
  buffer 0, so the flash stayed.)
- Frame 2 has a clear and draws the full stale-camera world on both paths (GS ~10k primitives).
- **Fixed with (a) (85587ef), checked in game 23 Oct ~19:15.** Fix candidates were: (a) never inject into the buffer on screen / take the frame's real target from the game (its own
  buffer index at Display3DElements) instead of the GS state at the marker; (b) honour the frame's missing clear: no
  injection (or a Z-tested merge) when the Z buffer was not cleared before the marker.

## Shadows: first decompile (24 Oct)
Decompiled C in `game/analysis/shadow/decomp/` (PoPDecompileTo). Projected-texture shadows, not mesh shadows:
- Display3DElements calls `CJadeShadowManager::DisplayAllShadows` (0x4C02C0) after the lightmaps (+0x5C): it resets
  some GS state words (0x6751B0..C4), `FindAndSortProjectorByTextureId`, then FUN_004C2400 per projector, then
  FUN_004B1D40 (render state flush).
- **FUN_004C2400 (one CJadeShadowProjector):** builds the projection at +0x170/+0x1B0/+0x1F0 (perspective when
  +0x3D0 == 0 from +0x3EC..+0x410, else orthographic from +0x3D8..+0x400), the camera from +0x360/+0x370/+0x380
  (sceVu0CameraMatrix -> +0x130), +0x230 = proj * cam, +0x270 = clip * cam, `UpdateTexProjectionMatrix`; strength
  +0x45C scaled by a distance fade (+0x104 set: 1 below 5, 0 above 20, linear between; distance at
  (+0x460)->+0x14->+0x60) and FUN_004C2C40; nothing when <= 0. VU program 6 (`DoSetVUProgram`), a GS word from
  +0x40C..+0x42C. Then: +0x111 set -> FUN_004C3180 (texture +0x100/+0x102 known) or FUN_004C3540 (caster lists +4/+0x14),
  both call FUN_004B80C0 twice (likely: render the caster into the shadow texture); +0x114 set -> receivers: either
  (+0x115 == 0) load texture +0x102 (TEX0 built inline), FUN_004C4540(projector, TEX0, alpha = +0x11C * 128 | 0x62),
  FUN_004B59C0, **FUN_004B80C0(+0x20 list, 0)**, FUN_004B8080; or (+0x115 set) FUN_004C2E00. Ends with a 3-qword GS
  packet (A+D: PRMODECONT = 1, i.e. back to PRIM attributes).
- **FUN_004B80C0(list, mode):** walks 16-byte entries (count at +4), kind = (byte +6 & 0x3F) >> 2 as in Render:
  4 CJIdxMesh (driver 0xB, mode 1: 5), 9 CJMeshElement::Display, 10 CJMeshElementRef (FUN_004C7B40), pass = byte +0xD
  & 0xF, matrix = entry bits 28-37. Before each, `DoSetShadowDriver` to driver (one bone 5 / more 3) + (mode 0: 6), |
  0x100 when the element's flag bit (+0x47 / ref +0x1B, bit 2) is set; sets 0x700000A8 = more than one bone,
  0x700000AC = 2. So the receivers are redrawn through their normal Display with a shadow VU1 driver.
- **Render-to-texture shadows.** FUN_004C3540 / FUN_004C3180: FUN_004C48C0 (target setup), `SetDrawShadowCastersMode`,
  `CVuManager::SetShadowCastingEnv`, **FUN_004B80C0(projector, 1)** = the casters (projector list +0) drawn into the
  shadow buffer; FUN_004C3180 first fills it (FUN_004C3A40, colour 0x80808080) and may apply a mask texture +0x102
  (`ApplyMaskToProjectorShadow`, alpha +0x120 * 128) and `BlendShadowTextureWithBrightnessTexture`. Then
  FUN_004C4540(projector, TEX0, ALPHA, fbmsk): an A+D packet that points the GS back at the frame (FRAME_1 from the
  display size 0x66D03C/40 and +0xBCC & 1, ZBUF_1, XYOFFSET_1, SCISSOR_1, TEST_1 0x50000, CLAMP_1 0, TEX0_1 = the
  shadow texture, TEX1_1 0x60, TEXFLUSH, ALPHA_1, PRMODECONT 0, PRMODE 0x58); FUN_004B59C0 (a VU1 constant: 0x63FD20..4C
  or 0x6E1580); **FUN_004B80C0(projector + 0x10, 0)** = the receivers, drawn with the shadow texture projected by the P7
  driver. FUN_004B8080 empties a list (count = 0) after use. The +0x20 receivers path (FUN_004C2400, +0x115 == 0) skips
  the caster render: a fixed (blob) texture +0x102.
- FUN_004C2C40 = a fog-like fade by distance from the camera (fog zone table 0x6CA504, the projector's object's zone).
- Still to read: FUN_004C2E00, FUN_004C48C0, FUN_004C3A40, SetDrawShadowCastersMode, SetShadowCastingEnv,
  UpdateTexProjectionMatrix, the two Blend/Mask functions, FUN_004B59C0's constants; and the P7 driver(s) 3/5/6+.
  Native plan follows from it: (a) render casters into our own shadow texture, (b) redraw receivers with projected UVs.
- **Manager and projectors in a save** (`game/analysis/shadow/tools/projectors.py <state>`): CJadeShadowManager at
  scratchpad 0x500: projector pointers +0 (count +0x100, active bitmask +0x110). Projector: lists +0 casters / +0x10
  receivers / +0x20 blob receivers (16-byte Render entries, ptr + count), shadow buffer +0x40C x +0x410 at fbp +0x42C,
  strength +0x45C, fade +0x120, mirror flag +0x50, mask/blob textures +0x100/+0x102, flags +0x104/+0x111..+0x115.
  Save 192521: 9 projectors, 8 active, all 256x256 perspective, strength 0.414-0.422, **lists filled** (e.g. #0: 14
  casters, 1 receiver), so it can be replayed offline. Lever save 172041: lists already emptied (FUN_004B8080 runs
  after the draw), so not usable for shadows; look for more saves with filled lists.
- **The shadow program in the trace of save 192521** (image b720d191 = VU program 6; `vu1replay --only`, decoded by
  `game/analysis/shadow/tools/p7runs.py`): 2,964 runs, per projector a setup run then casters then receivers. Entries:
  0x03F0 setup (no output; loads the projector matrices into **registers**: the caster runs read no matrix from VU
  memory), **0x0750 / 0x0B78 / 0x1008 casters** (three mesh variants; into the 256x256 buffer centred at 2048 by
  XYOFFSET, the mesh's own UVs, Q = 1, colour (128,128,128, strength x 128 = qw 21 from SetShadowCastingEnv), PRIM
  0x7C), **0x2058 receivers** (screen space, projective STQ into the shadow texture, colour (128,128,128, A) with A
  per vertex: depends on qw 21 (128,128,128,128 from FUN_004B59C0), qw 22 (0.03125, 255, 127, ..), qw 23, qw 28 and
  the projector position at qw 1019, not on normals: a spotlight-like falloff; `CreateSpotLightParam` 0x4C4F80 is the
  lead). Receiver inputs: positions qw 144+, a 4x4 at qw 1015-1018, rotation qw 1020-1022. (The earlier table's
  "record 2548 = the shadow-list draw" is a caster run.)
- **Receiver constants (FUN_004B59C0, qw = VU address):** 4-7 / 8-11 / 12-15 = gp+0x200 / +0x240 / +0x280 (camera
  matrices), 16-19 = projector +0x2B0 (tex projection x camera, UpdateTexProjectionMatrix: 0.5-scale/offset of the
  projector camera +0x130) or +0x2F0 (the same scaled by +0x108/+0x10C), 20 = projector +0x360 (its position),
  21 = (128,128,128,128), 22 = scratchpad 0x460, 23 = projector +0x3A0 or the caller's (FUN_004C3180: 0x6E1580 =
  (100, 110, 5.76, -0.000476)), 24-27 = projector +0x270, 28 = caller's direction (0x63FD20/30/40) or projector
  +0x390, 29 = GIF tag template, 30 = (0, 0x300008D, 0x2000190, 0). **qw 23 = (r0, r1, a, b)** with a = r1^2/(r1^2-r0^2),
  b = -1/(r1^2-r0^2) (trace: 2.119, 3.469, 1.595, -0.1326; CreateSpotLightParam computes the same pair and a cone
  term -cos(fov/2), 1/(cos(0.95 fov/2) - cos(fov/2))): a fade a + b d^2, 1 at r0, 0 at r1.
- **Careful: the VU code at 0x23B0-0x25D0 (reads qw 1019 = projector position, qw 22/23/28, writes alpha = ramp x
  fade, RGB = 128 x clamp(n . qw28)) is never executed in this trace** (`vu1replay --coverage`). Covered ranges:
  0x0000-0x0508 (setup), 0x0740-0x0870, 0x0A00-0x0CA8, 0x0D68-0x1130, 0x1410-0x17F0, 0x1BA8-0x2320 (0x1CE8-0x2050 =
  clipping). The executed receiver loop is still to be found: coverage per entry (receivers = 0x2058, 594 runs) needs a
  filter in vu1replay (or inprobe per run); then check the formula on recorded runs (inprobe --dump for the inputs,
  --out for the outputs; data after a run is not the input: buffers are reused). Blend (from the decompile): ALPHA
  0x46 = Cd (1 - As) on the rendered-caster path (FUN_004C3540), 0x62 = Cd - Cs FIX (FIX = +0x11C x 128) on the
  blob path; TEX0 = the shadow texture, TCC on, modulate.
- Then the offline frame: casters into a 256x256 texture, receivers redrawn, judged on save 192521.

## Native shadows (24 Oct afternoon): the receiver loop found, POP_NATIVE_SHADOWS=1 offline
- **The receiver loop** (`vu1replay --start <pc> --coverage`, new: only the runs that start at a pc): entry 0x2058 is
  `B 0x750`, the head shared with the casters; 0x14E8 culls the batch by its bounding box (8 corners, x 1.005); the
  dispatcher at 0x1410 sends batch types >= 6 to 0x1CE8 (the earlier note "0x1CE8-0x2050 = clipping" was wrong) and
  type 11 to **0x1D98-0x2038, the receiver loop** (software-pipelined), with 0x2068-0x2320 the per-triangle clip
  handling (ADC). Per vertex p (object space, qw vi1+1+k; count = qw vi1 x & 0x7FFF; output from vi1+196: tag, then
  STQ / RGBAQ / XYZ2):
  screen = (vf1..vf4) p / w; STQ = (T p / T.w) / w, Q = 1/w, with T = qw 1020-1023; RGBA = (128,128,128, A),
  A = int(max(min(qw22.z + qw22.w w, qw22.y) fade, qw22.x)), fade = clamp(qw23.z + qw23.w sum(qw28_i d_i^2), 0, 1),
  d = qw 1019 - p. Triangles with all three vertices outside one plane of qw 1015-1018 get ADC (dropped); outside the
  camera: guard band check (0x1BA8) or real clipping (0x2240) when qw29.w & 256. Model (`game/analysis/shadow/tools/
  rcvmodel.py` on `vu1replay --stage 1d98:2038` cases): **alpha within 1 on all 32,940 vertices of the 594 receiver
  runs of save 192521, STQ within 0.5%**; XY only differs on vertices behind the camera (ADC set). In that save qw22 =
  (0.03125, 255, 127, 0) (A = 127 x fade) and qw28 = (0, 0, 1): the fade is by height below the projector only.
- **The setup (0x0000, receivers):** W = qw 0-3 (the object's matrix): qw 1015-1018 = projector clip (qw 24-27 =
  +0x270) x W, qw 1020-1023 = texture projection (qw 16-19 = +0x2B0) x W, qw 1019 = the projector position (qw 20 =
  +0x360) in object space ((P - W.t) . W's rows), vf1-4 / vf5-8 = the camera matrices (qw 4-7, 8-11) x W.
  SetShadowCastingEnv (casters) uploads qw 4-7 = +0x230 (projector proj x cam), 8-11 = +0x270, 12-15 = +0x1F0,
  16-19 = +0x2B0, 21 = (128,128,128, strength x 128), 22 = scratchpad 0x360.
- **The shadow buffer is the alpha of the displayed frame** (fight GS trace `gstrace26f`: FRAME = DISPFB's buffer, fbw 4,
  FBMSK 0x00FFFFFF): per projector ~20 strips clear the alpha of a 256x256 corner to 0, the casters write alpha =
  strength x 128 (0x35) without blending or Z (SetDrawShadowCastersMode: XYOFFSET (2048 - size/2) x 16, scissor
  1..size-2, so the border stays 0; mirror projectors +0x50 use a Z test), then 8-9 receiver triangles into the back
  buffer: TEX0 = that buffer (CT32, TBW 4, TCC, modulate), CLAMP 5, bilinear, ALPHA 0x46, TEST 0x50000 (Z GEQUAL), Z not
  written, FGE 0. The picture is unchanged (only alpha is written), so the screen never shows it.
- Projector #0 of save 192521: the "perspective" path is orthographic (+0x170 has w = 1), the projector camera looks
  straight down from z 4.12; +0x230 maps to GS pixels around 2048; +0x2B0 = 0.5 clip + 0.5 (UV = t.xy).
  FUN_004C2400 multiplies +0x45C by +0x120 (distance fade) and FUN_004C2C40 (a fog-zone fade) for the call and
  restores it afterwards; all 8 active projectors take FUN_004C3540 (render to texture; +0x104 clear, both lists
  filled, +0x111 set).
- **Implementation** (`POP_NATIVE_SHADOWS=1`, offline frames only so far): a hook on FUN_004C3540 (`shadowRtt` in
  `pop_native_live.cpp`). The geometry is the frame's own records: the Display hooks note which records each call
  (element or ref, matrix index, pass; first call only) added, and the list entries look them up (kind 4 CJIdxMesh not
  yet). Casters: world space through +0x230, rasterised on the CPU into a 256x256 image (alpha only, GS top-left rule,
  1..size-2). Receivers: a copy of the base record's batches in object space with the base's model matrix (the same
  transform as the base draw, so GEQUAL sees its exact depth): UV = T p_world / T.w, colour (1,1,1, A/128), frustum
  culling as the VU; material flags 0xF0200 (Cd (1 - As), no depth write), fog off, Z-test word bit 1 = clamp
  (new `Material::clampUv`, a CLAMP_TO_EDGE sampler). Shadow images use synthetic texture keys 0xFE5D0000 + n.
  `pop_state_frame` replays DisplayAllShadows after the lightmaps (strength x +0x120; FUN_004C2C40 taken as 1).
- **Save 192521:** 6 projectors, 40 casters (7,033 triangles), 18 receivers (145 triangles drawn, 4,079 culled). The
  Prince's shadow lands where the game's does, with the same shape (`game/analysis/shadow/native/`). First version
  darkened twice (records of every pass and repeat call taken; floor x 0.49), now keyed by pass, first call only:
  floor x 0.67 in shadow, game ~0.74 (noisy; the GS formula gives 0.59 at fade 1). The game's edge is a little softer.
- **Live path built (same day):** `Renderer::updateImage` (a key re-sent with pixels updates its texture in place;
  shadow images are always sent, synthetic keys), world mode: a projector goes native only if every receiver's base
  record is native, then FUN_004C3540 is skipped and both list counts zeroed (as FUN_004B8080). PC (save 192521,
  `game\jobs\shadowtest.ps1`): 6 of 6 projectors native, `pop_world_test` 60 frames fine, worker 3.09 ms per frame
  vs 2.60 without shadows (six 256x256 RGBA images per frame, 1.5 MB: R8 or a bounding box would cut it). (Its GS Z
  is not byte-identical to the viewer's with or without shadows: older than this, not looked into.) Play build with
  it compiles.
- **In game (24 Oct, normal boot, TV):** ~59-60 fps, every projector native (1,132 of 1,132 in the first minutes, no
  list entry without records); **user: shadows look right on the floors, the lever flicker is gone.** Now on by default
  in `play.ps1` (`-NoNativeShadows` = off). The stats line printed every frame in that build (fixed: every 300).
- **Entries without records (25 Oct, N12): entries that draw nothing.** Saves 115837 / 115853 (many enemies, a save
  point): ~1.8 shadow-list entries per frame had no records, so ~40% of projectors and every later list went to the GS.
  All of them were element 01522420 (matrix 135/136), pass 1, **without a pass-1 packet** (+0x24 = 0): Display draws
  nothing for it, in the shadow pass too, so the entry now counts as empty geometry (`entries that draw nothing` in the
  stats line) instead of a miss. stnative boots: frames pure at +0x88 175 -> 2,095 (115837) and 112 -> 2,098 (115853),
  late calls native 4,300 -> 140,061 / 0 -> 69,210; misses 0; fps unchanged (~49 / ~53; worker ~5 ms per frame,
  GS thread waits ~4.8 ms). CJIdxMesh entries (kind 4) now find their records too (the CJIdxMesh hook notes them
  under `idxCallKey`; none appeared in these saves). The stats line splits misses: idx / mesh/ref / other kinds /
  call without records (the first 8 logged as `[pop:shadow] entry without records`) and receiver bases on the GS.
  Blob receivers (+0x20, FUN_004C2E00) need a blob texture (+0x102 != -1): none in these saves, still on the GS.
- Was "not yet": live / in-game (shadow images change every frame, so the PNF6/7 texture-by-key cache needs an in-place
  image update; then skip FUN_004C3540 when every receiver's base is native, clearing both lists as FUN_004B8080
  does), blob receivers (+0x20 list, FUN_004C2E00 path), mirror projectors, CJIdxMesh entries, FUN_004C2C40.

## Lists after the shadows: POP_NATIVE_LATE=1 (24 Oct afternoon)
- **Rule (purity).** The injected picture goes in at the frame's marker, so every draw left to the GS lands on top of
  every native one. A native draw after the shadows keeps the game's order only if *no* draw before it in the frame
  went to the GS (an early GS draw, e.g. water or an undecoded wall, would otherwise paint over native smoke). So
  `worldOnGs` ends the frame's purity at its first GS draw (mesh, ref, CJIdxMesh, sprite, water, list 0x18, GFX, the
  shadow list walker FUN_004B80C0 = any projector not native, FUN_004BDC80), and lists +0x88 / +0xA8 go native only
  while the frame is pure. The earlier lists keep the old policy (native wherever decodable).
- **+0x98 stays on the GS:** EraseAlphaBuffer runs before it and every +0x98 draw is alpha-only (render state 0x44 =
  0xFF000000: FBMSK 0x00FFFFFF), so it changes no colour; it ends purity only if it writes Z. A +0xA8 draw goes native
  only if it writes neither Z (+0x98 tests GEQUAL against it) nor alpha (EraseAlphaBuffer would clear it).
- **GFX particles** (FUN_004584C0) native in world mode (with the switch): the call is skipped and the particle driver
  index 0x70000080 set as the function does (0 billboards, 1 oriented). Counted as a GS draw only if its generators
  hold particles.
- **Census (203 saves, entries):** +0x88 17,750 (CJIdxMesh 8,898, mesh 3,676, GFX 3,049, sprites 1,128, ref 923, water
  76), +0x98 2,526 (mesh/ref), +0xA8 1,189 (all CJIdxMesh). Only 37% of +0x88 entries come before its first sprite or
  water draw, so **sprites are the main limit**: 58 of 142 pure frames end at a +0x88 sprite.
- **Sprites (CJadeSpriteElement::Display 0x4C6200, list kinds 7/8) read:** element = *(0x6E1598) + u16 * 0x20 (not
  low word << 4: `pop_state_frame` fixed); up to 4 slots, packet word at +4i (low 31 bits), count at +0x10+4i; per slot
  `SetParticleEnv` (0x4B6140; kind 8: FUN_004B5F80, VU program 5 instead of 4) with the camera right/up turned into
  object space, and a 9th argument (t2, dropped by Ghidra's C; `tools/ghidra/PoPDisasm.java`) = the packet, which turns
  the env tag into a DMA CALL. The packet (save 19_082854): MSCALF 0x68 with 8 control points (V3-32 at VU 0x10) and 8
  directions (0x3A), then count 45 at 0x64 and 3 qw per sprite at 0x65: the sprites are placed along a curve **on
  VU1**, so a native version needs that program read (0.13, with P9).
- **Offline A/B** (world frame vs the game's picture, 203 saves, characters + translucent + shadows on): mean 18.07 ->
  17.58; 54 better (best: 21_195939 21.80 -> 11.99, the moon and sky layer of the ramparts), 7 worse by <= 0.30
  (cutscene saves whose picture does not match the save's camera), 142 same. Pure at +0x88: 142 frames; 677
  particle draws native. What ended purity first: sprite +0x88 58, gfx +0x2C 30, mesh +0x2C 22, water +0x88 18, ref
  +0x88 18, mesh +0x98 (Z writes) 15, gfx +0x88 15.
- **GFX textures not found** (the GFX ends purity then): 2E005850 (+0x2C, flags 0x17; in no pack of the index: made at
  run time?) and 250004EC (+0x88; in many packs, does not decode from the one chosen). `POP_NATIVE_WORLD_WHY=1` logs them.
- Stats: `[pop:world] late lists: ...` every 300 frames (frames pure at +0x88, ended within, calls native/GS,
  particles native, the five commonest causes). On by default in `play.ps1` (`-NoNativeLate`); in-game check pending.
- Next: sprites (VU1 curve program), the two textures, then +0x98 natively (skip EraseAlphaBuffer when pure).

## Native sprites on the VU1 interpreter (24 Oct evening)

CJadeSpriteElement (list kind 8, `CJadeSpriteElement::Display` 0x4C6200) goes native under `POP_NATIVE_LATE` while
the frame is pure (`POP_NATIVE_SPRITES=0` sends them back to the GS). Until VU1 program P9 (ELF MPG 0x4E338C,
0x13A0 bytes) is ported to C++ (a Qwen job candidate, judged against the interpreter), the override runs it on
the runtime's own VU1 interpreter (`spriteRecord` in `pop_native_live.cpp`):

- FUN_004B5F80's env (19 qw at VU 0): model matrix, graphics object +0x200 / +0x240, camera right/up in object
  space, fog zone, GIF tag (sprite, TME/FGE/ABE, ST RGBAQ XYZF2 x 2), screen box. Rebuilt from guest memory.
- The element's packets (up to 4) are replayed VIF code by VIF code into one persistent 16 KB data image:
  the first element of a group uploads the curve, the next ones only a colour and their sprites, so data and
  the program's position carry over (MSCALF 0x68 then MSCNT).
- XGKICK output is caught by the interpreter's packet sink (`vu1_impl::packetSink`, forward = false); each GS
  sprite (strips of 32 pixels) is unprojected through +0x200 into a world-space quad and drawn as a kind-2 record
  with the vertex RGBA (material constant colour/alpha ignored: P9 writes RGBAQ itself). Sprite fog (F) not yet
  applied. A call whose sprites are all culled draws nothing and keeps the frame pure.
- Offline: `pop_state_frame_vu1` (x86_64 build with the sotc interpreter; on the Mac it runs under Rosetta).
  A/B over 203 saves (late lists on, game picture as judge): 17.58 -> 17.35, 41 better, 2 worse (<= 0.19: a
  lattice +0x88 mesh that now goes native because the frame stays pure longer); 1,128 of 1,128 draws replayed.
- Not sprites: the big light halos and streaks around glowing pickups are the after-effects pass (FUN_004ABFC0),
  still on the GS.

### P4 and P5 in the game: what each is for (29 Oct)

- **P4 (characters) has no place in the live path.** P9 replaced an interpreter the sprite path already ran; native
  characters run no VU1 program at all (own mesh decode, skinning and lighting on the GPU), so the "P4 into the
  game as P9" item in `docs/native_bugs.md` was a planning slip. P4's value is as an **offline judge**: the port is
  bit-exact to the interpreter (673 whole runs), so running it on a save's character packets and comparing vertex
  positions / colours / UVs with the native path names any shading or skinning mismatch exactly (as the STQ check
  on 32,940 vertices of 192521 did by hand). Running P4 live would move per-vertex work to the CPU: no.
- **P5 (GFX particles) replaces a reimplementation with gaps** (the `particles` hook rebuilds quads on the CPU from
  the particle data: oriented particles, random size ranges and the sheet V direction unverified; offline frames
  log "GFX draws ... not drawn natively", e.g. 115853's sand cloud). Particles are a few hundred quads per frame, so
  the exact program on the CPU is cheap. As P9: offline first, live behind `POP_NATIVE_P5`, off by default.
  **Done 29 Oct (offline; live written, off by default, not yet run in the game):**
  1. **Packets** (decompiled, retail): `CVuManager::SetParticleEnv` (0x4B6140) appends at *(gfx +0xEC) a DMA CNT of
     0x17 qw (FLUSHA, UNPACK V4-32 at VU 0, no TOPS): VU qw 0-3 pool matrix *(0x70000260) + u16 0x70000090 * 0x40,
     4-7 gfx +0x200, 8-11 +0x240, 12.xy sheet scale (1/cols, 1/rows; .zw stale), 13 / 14 the two axes (billboards;
     stale for oriented), 15 fog zone (0x70000270 + byte 0x70000084 * 16), 16 GIF tag (count | 0x8000, 0xC02A4000,
     0x12412412, 0x4124: PRE, triangle strip TME ABE, 4 x ST RGBAQ XYZF2), 17.x the driver index (0x70000080: 0
     billboard, 1 oriented; P5 reads it at 0x358), 18 a second GIF tag, 19-22 gfx +0x280. `CGraphicParticle::Initialize`
     appends one CNT: STCYCL 4/4, FLUSHA, BASE 0x18 / OFFSET 0 / MSCALF 0x68 from the program table, then billboards
     (0x4B5700, at most 200) V3-32 positions at TOPS, V4-8 colours at +0xC8, V4-32 (sx, sy, column, row) at +0x190;
     oriented (0x4B54C0, at most 150) V4-32 (position, flag) at TOPS, colours at +0x96, two axes (+ column / row) at
     +0x12C / +0x1C2; MSCNT. More particles than the cap: another env + batch pair in the same call.
     `tools/p5packets.py STATE [--dump]` lists them from a save.
  2. **Runs:** 0x340 calls the matrix set-up at 0 and ends at the E bit at 0x370; the MSCNT goes on at 0x380; the
     billboard / oriented loops end at 0x760 / 0xD88.
  3. **Code** (`pop_native_live.cpp`, `POP_NATIVE_P5=1`, `=interp` the interpreter only): the sprite path's VU code
     is shared (`VuProg` per program: loader, `vuRun`, `vuVif` with an end bound, `gifToBatch` now also turning
     triangles / strips / fans into world-space triangles). Live, `particlesP5` calls the game's FUN_004584C0, replays
     the DMA records it wrote (*(gfx+0xEC) before / after; nothing after an unwind) and, drawn natively, moves the
     pointer back so the GS never sees them; otherwise they go to the GS (`worldOnGs "gfx"`). Native only under the
     sprite rules (`POP_NATIVE_LATE=1` and a pure frame). Offline, `pop_state_frame_vu1` hands each GFX draw its
     records from the save's last chain (a new call at each env not after a capped one).
  4. **Checks:** frames with P5 off byte-identical to the build before the refactor (11 GS-only saves `gsref1027`);
     port vs interpreter byte-identical on all 11, no hand-over. Scores against the game picture, CPU quads -> P5:
     better in 10 of 11 (162246 2.27 -> 1.73, 184926 7.98 -> 7.85, 125052 2.86 -> 2.64); 145139 10.66 -> 10.71
     because the save-point light column is now drawn and the too-strong CPU glow blows it up (`--noglow` 7.29 ->
     5.84: the column itself matches). 115853: the sparks around the sword now drawn; its two oriented draws are
     behind the camera (w ~ -4) and P5 culls them, as the game does. The sand haze is not these particles.
  **Live, 29 Oct evening (patch 43/44 build):** stnative's three saves with P5 on: no interpreter hand-over, nothing
  not replayed; native share 184926 33% (my guess, unchecked: the native run ends at the water draw), 115837 / 115853
  ~99.7%; fps as the 25 Oct runs without P5. User check (`play.ps1 -Mode native -Save ... -Env POP_NATIVE_P5=1`):
  115853 sparks fine; 145139 column good (a glow line two thirds up is in GS-only too); 184926 fight fine (N17 is
  GS-only too). **On by default since then** (`POP_NATIVE_P5=0` off; native only with the late lists);
  sprite kind 7 (SetParticleEnv + program 4, left to the GS in the sprite path) is the same packet: P5 covers it.

### The P9 port in place of the interpreter (25 Oct)

P9 is ported (Qwen + hand stages, `docs/llm_jobs.md` "Night of 24-25 Oct results"). `port.py --chain p9` writes
`tools/vu1port/ported/p9.cpp`: every stage port inlined in its own namespace inside `vu1_p9`, `vu1_p9::run()` the
dispatcher on vu.pc (judged on 598 whole runs). `pop_native_live.cpp` includes it; `spriteRun` keeps the registers
in a `VuState` whose `mem` is the one data image the VIF replay and the interpreter both use. MSCNT goes on at 0x390
after a run from 0x340 (the setup ends at the E bit at 0x380; the port reports other ends only as 0xffffffff, and the
traces have no MSCNT after them: if one comes, the sprites go back to the GS for the session, logged). At a pc no
stage starts at, the registers go to the interpreter (`VU1Interpreter::state()`, stage borders have nothing in
flight), it finishes the run, and they come back. `POP_NATIVE_P9=0` = interpreter only; the offline P9 trace
(`POP_P9_TRACE`) always uses the interpreter. Offline A/B over the 236 saves of the P9 traces: all native frames
byte-identical with and without the port, 630 sprite elements / 75,009 quads, no handover.

## Native halos (30 Oct, 0.13; offline only so far, reviewed 30 Oct)

**What the game does** (retail FUN_004AA240 = AE_ProcessOneHalo, called per halo-array entry by FUN_004ABFC0 at the end of
Display3DElements; ProcessLensFlare = FUN_004AB500; disassembly via `tools/ghidra/PoPDisasm.java` because Ghidra's C drops
the stack arguments). Entry (0x20 bytes at 0x6DF2C0): position, +0x10 core size, +0x14 halo size (world units), +0x18
colour (top byte = alpha), +0x1C material; the material's +0x10 points at the first texture node (u16 texture index, +4
flags, +0x20 next node = lens-flare ghosts). The light goes through +0x240 and the viewport +0x280; core square = +0x10 / w
of half the display width (at least 8 px), clipped to the display; then on the GS: alpha 0 over the core, the core again at
the light's own Z with ZGEQUAL writing alpha = colour alpha * the core's on-screen fraction (register f20), bilinear halving
to one pixel, spread over the halo square with RGBAQ 0x808080FF (A = 0x80: As = At, not doubled); then the halo texture over
core +- (+0x14 / w), ALPHA 0x58 = Cs * Ad + Cd, no Z test, colour 0x808080 or the material's +8 (node flags & 0xE000 =
0x4000). The frame's alpha (glow mask) is saved before the halo loop and restored after.

**Natively** (`haloRecord` in `pop_native_live.cpp`, hook on 0x4AA240, `POP_NATIVE_HALOS=0` off; live only with `-NativeLate`
while the frame is pure after +0xA8): five GS sprites through `gifToBatch`: clear, Z test, spread (image -3: the frame copied
before it, mesh.frag bit 256 averages the copy's alpha over the core square, 8x8 samples), the halo (blend kind 6 + dest
alpha), restore (image -4: the same copy read per pixel, no new copy). The copy covers only the spread's square
(`Draw::copyRect`, from its vertices in `frame_loader`; review 30 Oct: ~30 full copies a frame in torch rooms otherwise); a
halo whose texture index is not in the texture list stays on the GS. Live it asks only for a pure frame (22697b4,
review 30 Oct): `worldStateOk` judged the leftover +0xA8 sub-material, usually +0x98's glow mask, and refused nearly
every halo. Halo geometry is sent with key 0 (`frameOnly`, see the water review). Nightly gate: the `vu1` lines of
`tools/nightly/render_gate.txt` (162621, 145139 through `pop_state_frame_vu1 --noglow`). Shortcuts: halos with ghosts stay on the GS (none
seen); the core square keeps the test's alpha instead of the glow mask. `pop_state_frame`: the save's count is always 0 and
the array holds stale entries from longer frames (162621: the sun at 0, 22 and 27), so it replays entries up to the first
without a material and skips repeats within 0.5 units. The 25 Oct live halo log read the texture node at +0x20 (wrong; fixed).

**Offline check** (11 GS-only saves `gsref1027`, `--noglow` because the offline glow over-brightens on its own, halos on vs
off against the game picture): 162621 (sun behind a tower) 10.59 -> **2.62**, the glow now matches the game; 145139 (moon)
5.26 -> **8.20** (worse); the other 9 unchanged (their halos are off screen or occluded: the 25 torch halos of 162621 sit
behind walls natively and on the GS alike). **Open: the moon.** The game adds nothing over the tower inside the moon's halo
square, so on the GS the moon's core fails the Z test (light Z 5,610), while native Z there is the sky's ~1,465 and it
passes. The saves cannot show the GS Z: the post effects overwrite Z memory for x < ~448, y < ~256 (the half-resolution
buffer at FBP 280), which is where every moon save has its moon. Next: a GS trace (`PS2X_GS_TRACE`) of a moon frame, GS
only, to read the Z under the core at the halo's test (needs the PC). This is also the save-point pop's halo (N10 notes).
