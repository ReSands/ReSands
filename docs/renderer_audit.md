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
GS comparisons are no longer complements of each other). Item 2 was wrong: dropping the instanced clamp turned the
walls of 160002 / 134531 / 145139 warmer than the GS picture (`score_saves` 1.67 -> 4.15 on 160002); the clamp stays.

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

8. **No Vulkan validation anywhere** (medium, now). Env switch in `Renderer::init` for `VK_LAYER_KHRONOS_validation`;
   run over the saves offline (`pop_native_view`, `pop_world_test`). Also check index < vertex count in
   `FrameLoader::load` (no robustBufferAccess).
9. **Alpha above 0x80 lost** (medium, measure first). Texture alpha: `assets.cpp:124`, `pop_native_live.cpp:3187`
   map 0x80-0xFF to 255; vertex alpha: `mesh.frag:40` clamps; the UNORM target clamps too. On the GS As > 0x80 scales blends
   up to ~2x. Census over the saves of palette / vertex alpha > 0x80 reaching native draws; fix only if any do.
10. **GS features not modelled or checked** (low-medium): DTHE, mipmaps (TEX1 MXL; sampler `maxLod = 0`), PABE, FBA,
   region clamp modes. One `PS2X_GS_TRACE` per area type through `gsvram.py`: which values the game uses.
11. **Hybrid boundary assumptions** (medium, parked to 0.2): the upload replaces everything drawn before the marker
   (checked on one save only); Z bias 128 lets GS draws up to 128 Z24 units behind a native surface show through.
   Cheap check: count GS primitives between the clear and the marker in the glitch note.
12. **Synchronous worker on the critical path** (medium, measure first): every frame `upload()` makes new buffers and
   a descriptor pool and waits for the queue 2-4 times (`oneShot`), plus `renderOffscreen`; the GS thread waits up
   to 250 ms. Measured ~3.3 ms per frame (22 Oct); N6 drops to 51-54 fps at ~1,450 draws. Unmeasured: that the waits are the cause.
13. **No texture budget** (low-medium): the worker only calls `reset(true)`, so textures and `imageOf_` grow all session.
   Log count/bytes in the 600-frame stats line.

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
18. `recolouredKey` (`pop_native_live.cpp:356`) re-decodes and re-uploads every frame while a palette differs, even
   unchanged; the 4-bit path (`assets.cpp:90-104`) ignores the RAM palette.

## Targeted reads still worth doing (before step 2)

The PNF writer in `pop_native_live.cpp` (sender half of `frame_loader.cpp`), the skip decisions (`displayRecord` and
the other "drawn natively, skip the GS call" paths), and `currentMaterial` + fog/FBMSK capture. The rest is better
covered by validation and output scoring than by reading.
