# Native renderer audit (27 Oct 2026, review only)

Baseline audit of the native renderer as it stood at edd6cb4. Read: `tools/native/src/native_renderer.{h,cpp}`,
the shaders, `frame_loader.cpp`, `tools/overrides/pop_native_world.cpp`, ~20% of `pop_native_live.cpp` (native/GS
boundary, materials, palettes, vertex colours), texture decode in `assets.cpp`. Not read: `viewer.cpp`,
`state_frame.cpp`, post-effect shaders (own judge), VU1 ports (own judges), mesh decoder, `pop_live_client.cpp`,
runtime patch 38. Nothing was built or run. The five next steps are at the top of `docs/native_bugs.md`.

Sound as checked: offscreen layouts and barriers, reversed D32 depth with GEQUAL, per-vertex screen-space fog,
half-pixel viewport shift, cached readback invalidation, kept-mesh generation drop.

## Confirmed defects

Fixed 27 Oct: 1, 3, 4, 5, 7 (the AFAIL failure pass now inverts the GS test with its own shader bit, since the strict
GS comparisons are no longer complements of each other). Item 2 was first rejected (walls of 160002 / 134531 / 145139 "warmer than the GS picture"), but that picture was
the native renderer's own (see item 9); **fixed 27 Oct night** against GS-only frames, with item 9's vertex alpha.

1. **Alpha test ignores vertex alpha** (medium, now). `shaders/mesh.frag:28-30` tests `t.a`; the GS tests As after
   MODULATE (texel x vertex alpha, the value written at line 40). Alpha-tested draws with vertex or constant alpha
   below 0x80 (`pop_native_live.cpp:3330`) keep pixels the PS2 discards. Fix: `a = t.a * min(vColour.a, 1.0)` for test and output.
2. **Instanced draws clamp vertex colour at 1.0** (medium, now). `pop_native_live.cpp:976` and `assets.cpp:468`
   use `min(c / 128, 1)`; the static and lit paths (`pop_native_live.cpp:1118`, `:837`) keep up to ~2x (GS MODULATE).
   Fix: drop the RGB clamp in both instanced paths (keep the alpha clamp until item 9).
3. **Alpha-test threshold off by one level** (low, with 1). Texel alpha is doubled, cutoff = top byte / 255
   (`native_renderer.h:68`): GREATER passes at `2a == top` (GS: `a > top/2`, differs for even top), LESS mismatches
   for odd top; the float equality is fuzzy. Fix: compare on the PS2 scale, `round(alpha * 128)` vs `top >> 1`, strict.
4. **AFAIL RGB_ONLY failure pass writes alpha** (low, latent: only +0x98 writes alpha, on the GS).
   `native_renderer.cpp:546-551`: add `failMat.writeAlpha = false`.
5. **Material dedupe key collision** (low). `frame_loader.cpp:113`: bit value 2 is both `clampUv` (`zTest & 2`) and the
   RGB-mask flag. Use `4u` for the mask flag.
6. **`present()` hang + shared semaphore** (low, viewer/live window only). `native_renderer.cpp:1762-1774`: fence reset,
   then `ensurePostBuffers` can fail and return, so the next wait blocks forever. Move it before acquire/reset. One
   `rendered` semaphore for all swapchain images (validation warning): one per image.
7. **Data race on worker timings** (low). `pop_native_world.cpp:144-146` write `g_stats` without the lock the hook
   reads them under (`:449`). Move the `+=` into the lock at `:155`.

## Risks to investigate

8. **No Vulkan validation anywhere** (medium, now; done 27 Oct, see `docs/native_bugs.md` step 2). Env switch in `Renderer::init` for `VK_LAYER_KHRONOS_validation`;
   run over the saves offline (`pop_native_view`, `pop_world_test`). Also check index < vertex count in
   `FrameLoader::load` (no robustBufferAccess).
9. **Alpha above 0x80 lost** (medium, measure first). Texture alpha: `assets.cpp:124`, `pop_native_live.cpp:3187`
   map 0x80-0xFF to 255; vertex alpha: `mesh.frag:40` clamps; the UNORM target clamps too. On the GS As > 0x80 scales blends
   up to ~2x. Census over the saves of palette / vertex alpha > 0x80 reaching native draws; fix only if any do.
   **Census 27 Oct (step 3a):** `POP_ALPHA_CENSUS=1 pop_state_frame --world` (play defaults: characters, translucents,
   shadows native) over the 51 saves; the switch keeps vertex alpha up to 2.0 in the decoders (`pop_mesh::g_keepAlpha`)
   and `Image::alphaOver` counts source texels > 0x80. **Texture alpha: none** (4,924 texture lookups, 10 not found,
   0 with a texel > 0x80; shadow textures are synthetic, lightmaps not counted). **Vertex alpha: yes.** Native today:
   83 of 1,795 alpha-tested opaque instanced draws in list +0x2C (19 saves): with the clamp the native test sees a
   smaller As than the GS (At x Av / 0x80), so texels the GS keeps can fail natively (small holes; not seen yet).
   On the GS today (late lists, native from 0.13): list +0x88 AddAlpha (blend 6) 548 of 1,090, Blend (1) 216 of 717,
   Add (4) 394 of 461 (alpha unused: harmless), +0xA8 AddAlpha 2. The GS traces (step 3b) agree: blended draws with
   vertex alpha > 0x80 in every trace, ALPHA 0x48 (Cs x As + Cd) 160-2,480 per 4 fields, 0x44 (Blend) 0-216, also
   in the menu (0x48: 896).
   **Caveat found 27 Oct evening: these vertex counts are the packets' bytes, not what the GS gets.** User decision:
   fix the alpha test now (accuracy first). Tried: decoders keep vertex alpha up to 2.0 and `mesh.frag` tests
   As = At x Av >> 7 clamped to 0xFF. `score_saves` changed on 9 of 51 saves, all through one path,
   `livePacketBatches` (instanced CJMeshElementRef draws, `pop_native_live.cpp` ~977; found by lifting each decoder's
   clamp alone), and 2 of the 3 checked saves moved away from the game: 201540's torch smoke came out darker and
   larger than the game's (6,574 pixels changed, 5,876 further), 134636 likewise (3,806 of 4,553 further); 202314 moved
   closer (24,438 of 30,735). The cause is not the test: with the old test the picture was the same; the shader clamps
   the interpolated alpha (`min(vColour.a, 1.0)`) instead of each vertex's, so an unclamped vertex makes the smoke's
   inside more opaque. That evening's conclusion ("the GS draws it light, so P1 must change the alpha") was wrong,
   see the P1 check below. **Reverted:** the decoders cap vertex alpha at 0x80 again (default output identical to
   before on all 51 saves); kept: the GS formula in the test (same result while Av <= 0x80) and the census switch.
   **P1 check (27 Oct night, static, `game/analysis/vu1/prog_4cde80.txt` + `tools/p1modes.py` over the 51 saves):**
   P1 picks a vertex path by VU 17.w & 0xFF (header written per batch). **Modes 0, 1, 2, 4 copy the packet's colour
   quad to the GIF packet unchanged** (`LQ vf26, 127(vi3)` -> `SQ vf26, -2(vi6)`; PACKED RGBAQ takes the low byte),
   and the guard-band clipper (0x840 / 0xA88) reads colours back with `IAND 255` / ITOF0 and carries the full byte:
   **P1 does not change vertex alpha; bytes up to 0xFF reach the GS.** Runs: mode 1 6,675 (275 with a byte > 0x80),
   mode 2 2,867 (1,406), mode 0 415, mode 4 1; 234 runs with a header the script does not follow. **Mode 3 is the
   lightmap pass** (ITOF15 UVs, 0x13B8 / clip 0x1590): no packet colours; RGB = VU 21 (SetLightmapColor, 0x80) and
   **alpha = clamp(c + d x w, lo, hi)** with VU 16 = (hi, lo, c, d), the fog formula written as alpha. Only in the
   baths saves (162246-162819, 583 runs each): (127, 76.2, 127.68, -1.062), so Av = 127 near and 76 from w ~48 on.
   Native draws lightmaps with Av = 1.0 (`livePacketBatches` without colours), so **native lightmaps there are
   darker than the GS's** (As = At x 76 / 128 on most of the scene; a native-path defect, fix now by the hybrid rule).
   **Why the experiment "moved away from the game": the reference is not the GS.** Every save since 22 Oct was made
   with the native renderer on (glitch notes: "injected"), so `popstate --frame` holds the native picture for every
   native-drawn draw: for those, `score_saves` compares the native renderer with its own older output. The torch
   smoke darker and larger with unclamped alpha is then likely what the GS draws. **Next:** a GS-only reference
   (boot the saves with every native switch off and take the frame, or `PS2X_GS_TRACE`), then re-run the vertex-alpha
   change against it. **Done 27 Oct night:** `game\jobs\gsref1027.ps1` boots 201540, 134636, 202314, 162246, 162621 with
   every native switch off and quick-saves 9 s after the load (`POP_INPUT=...,select@10`: a scripted Select now saves);
   the new saves' frames are pure GS (`game/analysis/gsref1027/`; TV off but `display_active` Enabled, 60 fps). The
   GS smoke is a light-grey plume and the flame yellow; native drew a dark smudge and an orange flame: **the instanced
   colour clamp (item 2) was the main error, alpha second.** `score_saves` against these frames, clamped -> RGB and
   alpha up to 2.0 in `livePacketBatches` / `assets.cpp`: 201540 1.60 -> 1.49, 202314 6.81 -> 6.70, 134636 11.10 ->
   11.05, 162246 (baths) 3.57 -> 2.44 (110,446 pixels changed, 102,019 closer), 162621 same. Alpha alone (RGB clamped)
   moved 201540 further; with RGB both together are best. Now the default. Blending still clamps As at 1.0 in the
   shader (the As > 0x80 blend question below). **Lightmap fade done the same night:** CLMElementList::Render
   (`004b7b80.c`) selects fog zone (entry bits 14-17) + 0x10, and table entries 16-31 are entries 0-15 x 127/255
   (all 361 zones of 56 saves, within 3e-5). The native lightmap draw now carries that zone's constants as its fog
   (`zoneFog`) and `Material::fogAlpha` (flags 0x30284) makes `mesh.frag` use F / 128 as vertex alpha with no fog
   colour. Against the GS-only saves: 202314 6.70 -> 6.60, 162246 2.44 -> 2.27, 134636 11.05 -> 11.04, others same;
   mean brightness of the changed pixels moves towards the GS (202314 110.6 -> 114.2, GS 116.9; 162246 216.7 ->
   220.6, GS 222.5; 162621 174.5 -> 177.6, GS 174.9, its sun glow differs anyway). Was open: mode-3 alpha for native lightmaps (VU 16 per draw, from the fog the driver sets). P5 and P9
   (ported) write their own alpha: their ports decide what the GS gets for particles and sprites.
   **Blending with As > 0x80 (user, 27 Oct: needed natively in 0.13, not left to 0.2; user suggested dual-source
   blending).** On the GS As up to 0xFF scales the blend up to ~2x. Vulkan clamps blend factors (dual-source ones
   too) to 0..1 on a UNORM target, so: AddAlpha (Cs x As + Cd) is exact with Cs x As written from the shader and
   blend ONE, ONE (if Cs x As > 1 the GS result clamps to 1 anyway). Blend ((Cs - Cd) x As + Cd) with As > 1 is
   Cs x As - Cd x (As - 1): dual-source can give the factor As - 1 (alpha output stays the written As), but it needs
   the SUBTRACT op while As <= 1 needs ADD, and the op is per pipeline, not per pixel: two draws of the same geometry
   split by As (with ordering issues for overlapping triangles in one draw), and still inexact where Cs x As > 1.
   Exact options: read Cd in the shader (programmable blending: `VK_EXT_rasterization_order_attachment_access`,
   check MoltenVK support, or input attachments with a feedback-loop barrier per overlapping draw), or a float
   target with the GS's per-draw clamp emulated. To decide at 0.13, after the P1 check shows how many draws need it.

10. **GS features not modelled or checked** (low-medium): DTHE, mipmaps (TEX1 MXL; sampler `maxLod = 0`), PABE, FBA,
   region clamp modes. One `PS2X_GS_TRACE` per area type through `gsvram.py`: which values the game uses.
   **Census 27 Oct (step 3b):** `tools/gscensus.py` over 8 GS-only 4-field traces (`game\jobs\gscensus1027.ps1`: saves
   115837, 125052 chasm, 134636 slowed enemies, 145139 save point, 155110 rewind, 162246 baths, 184926 fight, and the
   menu; plus the 16 Oct fight trace; 0.75 M draws). **DTHE 0, PABE 0, FBA 0 in every draw; COLCLAMP 1** (clamp, what
   a UNORM target does). **No mipmaps:** MXL 0 everywhere (MMIN 4 and K 104 appear, but with MXL 0 they only pick
   MMAG vs MMIN, both linear), so `maxLod = 0` is right. Nearest filtering (MMAG = MMIN = 0) on 80 + 12 draws in two
   traces, not identified. **Region clamp (2/2)** only on the post-processing sprites that read framebuffer copies
   (TBP 0x2300 / 0x1180 / 0): the same 12 rectangles with the same counts in every trace (the glow's downsample
   chain); match them when the post-processing goes native. Otherwise CLAMP (0,0), (1,1), a few (0,1) / (1,0).
   Nothing to model for the world; nothing to fix. Trace field = the save's own tick + 300 (after a state load the
   field counter continues from the save: `popstate.py` prints it; field 600 traced nothing).
11. **Hybrid boundary assumptions** (medium, parked to 0.2): the upload replaces everything drawn before the marker
   (checked on one save only); Z bias 128 lets GS draws up to 128 Z24 units behind a native surface show through.
   Cheap check: count GS primitives between the clear and the marker in the glitch note.
12. **Synchronous worker on the critical path** (medium, measure first): every frame `upload()` makes new buffers and
   a descriptor pool and waits for the queue 2-4 times (`oneShot`), plus `renderOffscreen`; the GS thread waits up
   to 250 ms. Measured ~3.3 ms per frame (22 Oct); N6 drops to 51-54 fps at ~1,450 draws. Unmeasured: that the waits are the cause.
   Measured 27 Oct (save 200200, native on, 60 fps): worker 3.6-4.0 ms, GS thread waits 3.4-4.0 ms (on the critical
   path, no frames lost at that load); N6's slow stretch did not reproduce from the save.
13. **No texture budget** (low-medium): the worker only calls `reset(true)`, so textures and `imageOf_` grow all session.
   Log count/bytes in the 600-frame stats line. Done 27 Oct (`<n> textures, <MB> MB`; 86 / 3 MB flat over 3,000
   frames of one scene); growth over a long session still to read from a `play.ps1` log.

## Migration work (not defects)

14. The final picture is still the GS's, fixed 640x448 (`kWidth`/`kHeight`, fbw check): blocks 1.3 and ray tracing,
   as expected; `present()` + GPU post effects already exist for GS-off. HUD/2D/post must go native first.
15. Lighting baked into vertex colours on the CPU (`pop_native_live.cpp:828-837`), no normals in `Vertex`, point
   lights ambient only. Fine for the baseline; for ray tracing, kept meshes with stable keys + world-space posed
   characters are a good base, and albedo/lighting separation + normals can be added later without a rewrite.

## Deferred

16. `pop_native_live.cpp` (4,338 lines) mixes ~10 jobs and is included by six-level relative path; split a part
   out when it is next touched (shadows, lighting, packet decoding first).
17. No automatic output gate: `score_saves.sh` is not in `tools/nightly/checks.py`; no test of `materialFromGsFlags`
   / blend factors (one quad per blend kind through `renderOffscreen`). The emulated GS is a regression reference only.
   **Done 28 Oct:** `pop_blend_test` + the nightly `render` check (`docs/native_bugs.md` step 4).
18. `recolouredKey` (`pop_native_live.cpp:356`) re-decodes and re-uploads every frame while a palette differs, even
   unchanged; the 4-bit path (`assets.cpp:90-104`) ignores the RAM palette.

## Targeted reads still worth doing (before step 2)

The PNF writer in `pop_native_live.cpp` (sender half of `frame_loader.cpp`), the skip decisions (`displayRecord` and
the other "drawn natively, skip the GS call" paths), and `currentMaterial` + fog/FBMSK capture. The rest is better
covered by validation and output scoring than by reading.

**Done 27 Oct.** (a) PNF writer (`writeFrame`) against `FrameLoader::load`: consistent. The worker queue drops no
frame, so a texture key or batch marked sent always reaches the renderer. Shadow and recoloured-palette keys
(`kShadowKey` .. +0x10000 holds `kPaletteKey`) are resent with pixels every frame. (b) Skip decisions
(`displayRecord`): one open question, not a confirmed bug. Pass 0 and the lit pass 1 share the frame-seen key
(element, index, alpha bit; only passes 2 and 3 add bits). An element displayed with pass 0 and then pass 1 at the same
matrix index in one frame would get its pass-1 call skipped on the GS (`worldSeen` + `g_worldBase`) with nothing
recorded natively for it. Unknown whether the game ever does that: count it in step 3's census before changing keys
(shadows' `callKey` and the posed path use the same scheme). **Census 27 Oct (step 3c): never.** Over the 51 saves'
draw lists 17,676 (element, matrix) pairs got kind 9 / 10 Display calls: passes {0} 16,040, {1} 1,489, {0, 2} 80, {1, 2} 67,
{0, 1} none; the shared key stays (`pop_state_frame` census, pass masks). (c) `currentMaterial` against `FUN_004B2000`
(`game/analysis/native/decomp/004b2000.c`): FBMSK, ZTST, AFAIL, alpha-test enable match. One defect fixed: TEST is
written as `byte << 3 | 4` for the inverted test, so an odd AREF byte sets ATST bit 2 and LESS becomes GREATER;
`mesh.frag` now does the same (no score change on 51 saves: no such draw in them). Not checked: the GS re-sends FBMSK
only when flag bits 0x68 change (or on a forced reset), while the native side reads render state 0x44 per draw; they
differ if 0x44 changes alone between two draws with the same flags (who writes 0x44 not traced).
