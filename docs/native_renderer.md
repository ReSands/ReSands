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
