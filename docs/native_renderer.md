# Native renderer (Stage B), plan

Decided 14 Oct 2026: Vulkan; PS2 assets only; the emulated GS (SotC fork, OpenGL) stays for
everything not yet native. Format knowledge comes from the Python reference converter
(`tools/jade2gltf.py`, `tools/jadetex.py`), checked against captured frames.

## Steps

1. **Engine-level draw list per frame.** Which elements the game draws, with which matrix and
   material, read where the game decides it (the draw manager's element lists), not from the DMA
   chain. Open question first: the frame capture saw only 11 `CJMeshElement::Display` calls while
   the DMA chain held 1,440 geometry uploads, so the static world reaches VU1 by another route
   (`Display` chains to the next element through +0x4C, possibly as a tail call the hook does not
   see; the lightmap list; or a batch path). Find it by decompiling `CElementList::Render`
   (0x4B9440) and reading the list entries directly. Judge: the per-frame list reproduces the DMA
   chain's draws (`tools/dmacmp.py`).
2. **Load-time conversion in C++.** Port the reference converter's decoding (inline and
   reference packets, instanced meshes, skinned meshes, UV scales, baked colours, lightmap UVs,
   textures incl. PSMT4 lightmaps, material blend bits) into the runtime, hooked at
   `CJMeshElement::ReadElement` / the visual loader, producing vertex/index buffers and textures.
   Judge: identical output to the Python converter on every world pack (a natural Gemma job:
   precise spec = the Python code, strict judge = byte comparison).
3. **Vulkan "native view" window.** A second window in the runtime process that draws the frame's
   static world from (1) + (2) with the game's camera. No interop yet. Judge: `tools/imgscore.py`
   against the GS frame.
4. **Composite into the game window.** Draw the native world into the main presentation (Vulkan
   presenter, or GL/Vulkan interop) with the emulated GS drawing only what is not native yet.
5. Then: characters (GPU skinning with the game's bone matrices), effects, post-processing, 2D,
   GS off (Stages C-D).

Build: Vulkan headers + `volk` (loader) via CMake FetchContent, pinned versions; shaders compiled
to SPIR-V ahead of time and embedded. No Vulkan SDK install needed on the PC.
