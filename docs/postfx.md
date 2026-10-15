# Post-processing (glow / haze), research 15 Oct 2026

Source: the prototype's `CAfterEffect` (decompiles in `game/analysis/post/decomp/`; retail object at
0x6df170 / 0x699060 in the prototype), the fight frame capture `game/analysis/drawlist/pc_1015f/`.

## What we know
- The after effects are **not in the VIF1 chain** we capture (only one 6-vertex sprite group there):
  they are drawn by PATH2/PATH3 (GIF DMA channel) outside it. The Mac probe on 13 Oct saw ~120 large
  textured + ~60 untextured sprites per frame, the same every frame, some into the half-resolution
  buffer at FBP 280 (shares memory with the Z buffer).
- `ApplyEffects` (proto 0x469600) runs, each step behind a status flag and parameters in the effect
  object (`puVar4[...]`): sun **halos** (`AE_ProcessOneHalo`, `AE_g_Halos`, count `_AE_NbHalos`) ->
  `ClearBorderExtend` -> `ComputeWaterReflection` -> **`BrightnessContrast_ColorBalance`** ->
  **`DepthBlur(far, near)`** + `DepthBlurNear` -> `Blur` -> `ZoomSmooth` (x2) -> `Remanance`
  (afterimage) -> `RadialBlurWithMask` -> `PinchEffect` -> **`ColorDiffusion`** (glow) ->
  `MotionSmooth` -> `Spiral` -> **`AE_ProcessSmartBlur`** -> `AE_ProcessVisionEffect` ->
  `ClearBorderExtend`. All are built from "HellBlitter" textured-quad blits (SetHellBlitterB =
  blend/source setup).
- The fight frame's haze is most likely DepthBlur + ColorDiffusion (+ brightness/contrast); the sun's
  evening glow arc is the halo pass.

## Plan
1. (PC) Extend POP_FRAMECAP to dump the CAfterEffect object (status flags + parameters) at the
   captured frame, and capture PATH2/3 GIF packets (runtime GIF arbiter tap, as POP_GIF_TAP on the
   Mac) so each pass's blits (source/target rectangles, blend, colours) are known exactly.
2. Decompile the active effects (from step 1) and write each as a shader pass on the native frame,
   using our depth buffer (DepthBlur needs depth; the PS2 reads Z as a texture via the shared buffer).
3. Judge on the fight frame and the 0102 frame: imgscore against the game screenshot.
Candidate local-model work: once a pass's GS blit sequence is known, porting it to a shader is
spec-like (judge = image difference against a reference render of the blit sequence).
