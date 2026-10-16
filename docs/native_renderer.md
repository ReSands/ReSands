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
