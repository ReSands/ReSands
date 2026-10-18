# Project reference (moved from CLAUDE.md on 17 Oct 2026)

Background that is not a session log: PS2Recomp, prototypes and symbols, cross-version assets, formats, other projects, RTX Remix, texture packs, renderer/DLSS notes, sources. Verbatim from CLAUDE.md; claims marked (estimate) / (unverified) keep that meaning. Dated session logs are in `docs/history.md`.

## PS2Recomp PRs (reviewed 11 Oct 2026)

Decision: build on upstream (MSVC-first, Windows is the end target) plus selected small open PRs on a local `pop` branch, made by `tools/ps2recomp-pop-branch.sh`. Not the DQ8 fork (#254): its runtime does not build with MSVC (`ucontext.h`, weak symbols). Its VU1 compiler (has SSE2 paths) and LLE IOP/SPU2 (runs LIBSD/SDRDRV unmodified) are worth lifting later.

- Merged on `pop`: #268 (vcallmsr, replaces our patch), #277, #215, #235, #266, #267, #270, #279. All read line by line; no network, process or out-of-tree file access.
- Conflicted, not merged: #195 (sceSifSendCmd args in $t0/$t1; only its test file conflicts, worth redoing), #239 (path containment + Windows drive paths; conflicts with upstream #244 IOP emulator), #241 (SIF init handshake; conflicts with #244, probably not needed since HLE `sceSifInitRpc` works).
- Deferred: #272 (PS2 float clamping everywhere; slows VU ops, overlaps #277). Dropped: #240 (already upstream as #274), #135 (stale, 3.4k lines).
- Claude Code's auto-mode permission check blocks merging/building/running unreviewed external code. The user runs `tools/ps2recomp-pop-branch.sh` themselves; building and booting need a permission rule for `tools/rebuild.sh` and `tools/boot.sh`.

## Pipeline (PS2Recomp)

1. Extract the ELF and data files from the ISO (standard ISO9660; mount or use 7-Zip).
2. Open the ELF in Ghidra; run `ps2xRecomp/tools/ghidra/ExportPS2Functions.java` to export the TOML config and CSV function map. Fallback for ELFs with debug symbols: `./ps2_analyzer your_game.elf config.toml`.
3. Recompile: `./ps2_recomp config.toml` generates C++ (literal one-to-one translation of MIPS R5900 instructions).
4. Build the generated C++ and link with `ps2xRuntime`.
5. Boot, observe failures, fix, repeat from a cold boot each time.

Iteration advice from the README: start with a minimal config and no aggressive skipping; fix hard blockers first (function not found, syscall TODO, critical I/O stubs); use temporary return stubs (`ret0`, `ret1`, `reta0`) only to classify call importance; promote them to real implementations; put per-game hacks into Game Override modules keyed by ELF metadata (`PS2_REGISTER_GAME_OVERRIDE`), not into global runtime stubs. Stub addresses (`handler@0xADDRESS`) are specific to this exact ELF build.

Modules: `ps2xAnalyzer` (scans ELF, writes TOML), `ps2xRecomp` (TOML + ELF to C++), `ps2xRuntime` (memory, dispatch, syscalls, hardware stubs), `ps2xIOP` (R3000A IRX execution, virtual IOP kernel, HLE fallbacks).

Build requirements: CMake 3.20+, C++20, mainly tested with MSVC, SSE4/AVX host support for some vector paths.

## Honest status of PS2Recomp

- Experimental. Announced late Jan 2026. Early coverage said no playable games.
- A community recomp catalogue (checked 11 Oct 2026) says: CPU works, but the Graphics Synthesizer (GS), VU1 microcode and DMA are still stubbed; no playable game yet.
- README limitations: "Performance is very bad for VU and GS"; hardware emulation partial, many paths stubbed.
- No published count of boot/fix cycles to reach playability. (estimate) Expect weeks to months, not a handful of cycles. The cheap early fixes (file I/O, syscalls, input) are quick; the expensive parts are the GS renderer and VU microcode, each effectively a project of its own. Real risk of stalling on hardware quirks nobody has solved.
- No competing PS2 recompiler found. PSRetroX (an earlier attempt) went inactive.
- Fallback if this stalls: PCSX2 with HD texture packs gives most of the visual benefit today.

- PS2Recomp wiki (https://github.com/ran-j/PS2Recomp/wiki, read 18 Oct 2026; last edit Aug 2026): a getting-started guide (EE overview marked outdated, ELF files, analyzer/recompiler/runtime pages, override hooks, a "stripped game walkthrough for LLMs"). Nothing we had not already learned: no save states, VU0 performance, GS backends or rumble. One rule matches ours: a handler bound to an address with `bindAddressHandler` must advance `ctx->pc` (else it redispatches forever); for anything non-trivial use `registerFunction` with `if (ctx->pc == entryPc) ctx->pc = $ra`.

## Tooling to add

- Ghidra plus **ghidra-emotionengine-reloaded** (https://github.com/chaoticgd/ghidra-emotionengine-reloaded): disassembles/decompiles EE instructions (MMI, VU0 macro mode), recovers types/functions/globals from ELFs with `.mdebug` sections (STABS analyzer), imports PCSX2 save states. Match the release to the Ghidra version. First check whether the US ELF carries debug symbols; that would give function names for free. PCSX2 save states let us inspect live game memory.
- Skip matching decompilation (splat + decomp.me, as used for Sly Cooper / Ratchet & Clank): readable source but years of manual work; wrong fit here.
- Jade-engine file tooling, e.g. JadeStudio (https://github.com/4g3v/JadeStudio): extracts/rebuilds Beyond Good & Evil archives and textures. Same engine family may help with data formats; the project is stalled. Note: sources disagree on the engine name for Sands of Time (Wikipedia says Jade; GOG's announcement says "Scimitar"). Confirm from strings/data in the ELF and game files.

## Using the PC (GOG) port as a reference

- GOG's wrapper (August 2026) adds widescreen/ultrawide, camera scaling that preserves vertical view, FPS limits, multi-monitor mouse locking, and is described as a reusable DirectX wrapper. It is a useful checklist of what the original PC port got wrong.
- Player complaints about the original PC release: unresponsive camera versus PS2, no native controller support. A fan wiki claims the PC version was considered the best at launch (2003), so some "badness" is age. Who built the PC port: one Steam post says Kudosoft (unverified). No developer interviews or postmortems found.
- The PC build may reveal symbols, strings, file formats and quirks that speed up identifying functions in the PS2 ELF.

## RTX Remix (fast path to DLSS / path tracing, test first)

- RTX Remix targets DirectX 8 and early DirectX 9 games with a fixed-function pipeline (DX9.0c shader-heavy games usually do not work; OpenGL and non-PC games do not work without a wrapper). Needs an NVIDIA RTX GPU, Windows.
- The PC version of Sands of Time is a DirectX game, so it might be compatible. Stock Remix does not work (shader-based D3D9). **kaminoer/pop-sot-rtx** (found 15 Oct 2026; v0.2, Apr 2026) is a closed-source `d3d9.dll` proxy that converts the game's shader draws to fixed function so Remix works (video of the Hall of Learning path-traced). Supports the English Ubisoft Connect version only (our GOG copy untested). Repo holds only README + `rtx.conf` (PC texture hashes, not useful for the PS2 path). Its known issues (water placeholder, time-power overlays, fog off, multiplicative particles skipped, VS-driven particles flattened, Remix skeletons without hierarchy) match our Stage D hard parts. Option for step 17: a D3D9 fixed-function back end fed by our native draw list could drive Remix on the recomp.
- GOG's wrapper may interfere with Remix. Test with the original exe/DLLs too.
- Check the ModDB Remix compatibility table before starting.
- Remix needs upgraded PBR materials to look right; plain old textures tend to look uniformly shiny or matte.

## HD texture packs (all PCSX2 replacement packs)

Packs are region-specific (keyed to disc serial) and matched by texture hash, so a recomp renderer must hash textures the way PCSX2 does.

- TexMaster, "Texture Remaster HD Pack", **SLES-51918 (Europe)**, v2.0 (Oct 2025), widescreen-oriented; author notes some environments still need work. (gbatemp.net)
- xXtherockoXx, "Prince of Persia Trilogy HD Textures", **SLUS-20743 (US)**, 1024/2048 BC7 textures, ~1.37 GB. (gbatemp.net)
- "Prince of Persia: The Sands of Time Remastered Project", screenshots from SLUS-20743 (Apr 2025). (gbatemp.net)
- Nexus Mods "Mod Collection V2.1 (PC and PCSX2)": needs the Remastered mod installed first; US version only.
- No community consensus on a "best" pack was found (no ratings or comparisons). Compare them side by side in PCSX2 before choosing. Licensing: packs derive from Ubisoft assets; personal use only.

## Platform split

Mac (Apple Silicon): extract ISO, Ghidra analysis (Java, runs on macOS), editing code, probably the recompile step (ELF to C++) but unverified: the README mentions SSE4/AVX and MSVC, and Apple Silicon has neither natively, so the runtime is more likely to hit problems than the recompiler. If it fails, run that step on Windows.

Windows PC: building the runtime (MSVC), running and testing the game, any D3D12/Vulkan renderer work, DLSS and RTX Remix (need an NVIDIA RTX card and Windows).

Remote workflow: enable Windows OpenSSH Server (Settings > Optional Features); SSH from the Mac; run Claude Code on the Windows PC for the build/test loop; VS Code Remote-SSH for editing. Windowed games do not show over SSH, so use Moonlight + Sunshine or Parsec to actually see/play the game (Windows Remote Desktop is less suited to games). Use Tailscale or another VPN for access from outside the LAN; do not expose the SSH port to the internet.

## Cross-version assets ("ultimate version", started 12 Oct 2026)

Goal: combine the best assets of each 2003 version (PS2, GameCube, Xbox, PC) into the recomp, inventing nothing new. Versions beyond the PS2 disc are optional; the game must still run without them.

- All versions are Jade bigfiles: PS2 `DATA/PRINCE.BF` (227 MB) and PC `prince.bf` (411 MB) both start `BIG\0` version 0x25. Their first FAT table (3,308 entries from 0x44) has identical asset keys in identical order, at different offsets. So assets very likely share keys across platforms, and matching them is a key lookup, not guesswork. Not yet checked: the rest of the FAT, and the GameCube/Xbox files.
- Asset data is converted per platform (PS2 palettised/swizzled textures for the GS, DXT on PC/Xbox, CMPR on GameCube), so each format needs a decoder.
- Higher-resolution textures cannot go through the game's own loading: GS VRAM is 4 MB. Replace them in the renderer at draw time, keyed by the PS2 texture (same hook as the HD texture packs).
- (unverified) The Xbox version is generally considered the best-looking console version. The PC bigfile being 1.8x the PS2 one suggests larger assets. Compare before assuming.
- Movies: the PC's Bink movies (640x346) could become an optional source next to the IPU path, if they look better.
- GameCube (12 Oct 2026): US Rev 1, `GPTE41`, NKit image in `game/gc/iso/`, files extracted to `game/gc/disc/` with `game/gc/analysis/gcm.py <iso> <outdir>` (reads the GCM file table directly). No `.map` file, and `main.dol` (4.0 MB) has no symbols (47 source filenames in strings). Movies are **Bink**, like the PC; the `VIS_*.int` files are byte-identical to the PC ones. It has extra `CINE_901`–`905` movies and its own `pop1.dol` (the bonus original Prince of Persia). `prince.bf` (241 MB) is a Jade BIG v0x25 too, with 1,602-entry FAT tables; 1,598 of its first table's keys are in the PS2 file.
- Xbox (12 Oct 2026): XISO in `game/xbox/iso/`, files in `game/xbox/disc/` via `game/xbox/analysis/xiso.py <iso> <outdir>`. `default.xbe` built 20 Oct 2003 from `e:\builds\v167\xbox\...` (the PC exe is the later v181a), XDK 1.0.5558 libraries (D3D8, DSOUND, WMADEC, XONLINE, Bink). **No symbols**: no PDB, 7 RTTI names, only DARE sound-engine source paths. Bonus `POP1/` and `POP2/` (Prince of Persia 1 and 2, own XBEs).
- Movies compared (`cine_010`, 95 s, all 640x346 Bink at 29.97 fps): **Xbox best**, 6.6 Mbit/s with 20 audio tracks = 5 languages x 5.1 surround (2+1+1+2 channels); GameCube 4.5 and PC 3.9 Mbit/s, 5 stereo tracks; PS2 uses its own format (IPU). Xbox also has `ATTRACT.int` (640x480), per-language logo/intro/loading movies, and the making-of documentary (`CINE_90x0-2`, the same files as the GameCube's `CINE_901-905`, three language variants).
- Bigfiles (`game/xbox/analysis/bigcmp.py`; file count at 0x08, then (offset, key) pairs from 0x44, each entry starts with a u32 size): PS2, Xbox and PC all have the same 3,308 keys. GameCube packs differently (1,602 keys, 1,598 shared). The prototype has 874, all in retail. Totals: PS2 195 MB, GameCube 209, Xbox 332, PC 377. The growth is in ~280 large world packs (keys `ff0…`, PS2 ~2 MB, Xbox ~3.5, PC ~4.3, stored in 128 KB compressed blocks): PC > Xbox > GameCube ≈ PS2. Most other keys are the same size everywhere. Sizes are compressed, so real texture resolution needs decoding.
- **Sound quality across versions (open, 16 Oct):** archive sizes are similar: PS2 `SOUNDPS2.BIG` 300 MB, GameCube `SOUNDGC.big` 300 MB, PC `Sound/` 322 MB, Xbox `SOUND/SOUNDX.big` 243 MB + per-language `soundx0-2.big` ~55 MB each (~407 MB). PS2 streams are 4-bit IMA ADPCM at 24 kHz stereo; the others' codecs and sample rates are not checked. For 1.3: decode the same stream from each with vgmstream and compare (rate, bit depth, channels).
- Symbol check for each new disc: `.map` files on the GameCube disc, symbol strings in `main.dol`, debug sections or a PDB in the Xbox `default.xbe`, an unstripped ELF in any prototype. Names found can be carried to the PS2 ELF with Ghidra Version Tracking.

## Survey of other PS2 projects (13 Oct 2026)

unbaked.games lists 178 PS2 games / 328 projects (116 with matching decomps, 66 with static recomps). No Prince of Persia or other Jade-engine project. 55 PS2-version static recomps have READMEs; raw data and the flag table in `game/analysis/unbaked/` (`ps2_recomps.txt`, `readmes/`). Most use PS2Recomp; hardware layers vary. Most useful references:
- **Renderer options seen:** PCSX2's GS (NBA Street V3 as a separate "live" process fed by a ring buffer; Ridge Racer V "fukami"; Urban Reign; Killzone; up to 8x, FSR/DLSS, PCSX2 texture packs), paraLLEl-GS (Vulkan, supersampling: Def Jam, Road Trip, RE Outbreak, BT3), own GPU renderers (Test Drive EoD: D3D11 up to 6x; BT3-Recomp: Vulkan renderer with PCSX2-compatible texture-pack hashing `<TEX0Hash>-<CLUTHash>-<bits>`), native meshes (Ace Combat 5, own recompiler, Apache-2.0).
- **Same base as ours (SotC fork):** Tobiichi-Port (God of War) adds Hor+ widescreen (from the PCSX2 patch), FSR 1 + FXAA at presentation, presenting at 120+ Hz independent of the 60 Hz logic, VU1 recompiled bit-exact, and audio time-stretch with the IOP clock tied to EE time.
- **IPU/movies:** Test Drive EoD (`hw/` IPU: MPEG-2 macroblock decoding + colour conversion, movies play), Killzone (PCSX2 IPU), SRWZ-native (IPU/MPEG via FFmpeg), Ace Combat 5 (runtime IPU).
- **Sound:** fukami (PCSX2 SPU2 core), RE Outbreak (own SPU2 mixer -> WASAPI), Tobiichi (IOP clock tied to EE), Road Trip (sound driver native).
- **VU1:** Killzone packages PCSX2's microVU ("KZVU", reused by SRWZ-native via `PS2X_VU1_KZVU=1`).
- **Correctness tools:** NBA Street's `PS2X_VERIFY` (recompiled functions vs interpreter) and its fix list (SQRT.S ft, 64-bit branches, VRSQRT/VSQRT/VDIV, CVT.W.S, VCLIP, vf0; our SotC base already has SQRT.S and 64-bit branches); SRWZ/REO compare against PCSX2 via PINE.
- Many projects state they were built with Claude Code or other AI agents.
- **recomp.fyi** (checked 13 Oct 2026; ~1,944 projects, all platforms): 19 PS2 recomps not in the unbaked survey, READMEs in `game/analysis/recompfyi/readmes/` (3 repos gone). Most useful: **Waffle565/God-Of-War-Native-Port** (PS2Recomp fork, runtime on its `runtime` branch, GPL-3.0): replaces every hardware layer natively: VU1 microprograms AOT to C++, the game's 8 IRX modules AOT to C++ (boot 95 s -> 20 s), native sound engine driven by the game's LIBSD calls, FMV via libmpeg2 + that sound engine, native OpenGL render-target GS renderer (2x, 16:9, texture/CLUT store instead of a VRAM model), DMA packets handed straight to VU1/GS, 60 fps; read its `docs/native-port.md` and sound docs before our sound/IPU work. Also: nathanialf/ico-recomp (MIT, own translator, ships paraLLEl-GS, has an unverified clean-room Vulkan/D3D12 GS renderer; names from a dev objdump listing on the disc), vs-sr-dev/pc-extermination (MIT `ps2kit`: Python GS/VU/IRX/PSS/ELF tools; meshes, skinned characters and rooms exported to glTF, a model for our offline mesh converter), Gui-Tora/dmc-recomp (PSS movies via FFmpeg HLE; found PS2Recomp's `sceGsSetDefDBuffDc` stub seeds disp[0]/draw FRAME with the Z-buffer address; check ours). King Kong (Xbox 360, Jade engine) port TekRantGaming/king-kong-recompiled: ReXGlue, emulated GPU, hooks at the GDI camera for FOV; not reusable code.
- **reSOT (FulGerNet/rePOP, checked 16 Oct 2026; MIT + LGPL-2.1 parts from xemu):** static recompilation of the **Xbox** SoT (E3 2003 demo ~60 fps, played to level 3; NTSC retail in bring-up) with xboxrecomp (x86 -> C). Rendering is emulated at the GPU boundary: the game's statically linked XDK D3D8 still writes NV2A pushbuffers, which the runtime executes on D3D11 (NV2A vertex programs -> HLSL, one pixel shader interpreting register combiners, 2x internal resolution). Sound on xemu's APU. Same engine, so its `docs/RECOMP.md` and per-target `target.h` are a source of Xbox addresses and Jade behaviour (useful for 1.3 cross-version assets), and its Hor+ projection/culling hooks a reference for 1.4.
- **CPU side / VU0 microcode (re-surveyed 18 Oct 2026, after the slowdown pointed at the VU0 interpreter):** earlier surveys were read mainly for renderers, but several projects did CPU-side work we now need. **SirBraga/black-recomp** (Black): EE, VU0, VU1 and IOP all recompiled, nothing interpreted in the normal path (VU0 ~70%, VU1 faster than hardware via NEON blocks); **sp00nznet/reo** (RE Outbreak): own VU0/VU1 microcode translator (SSE/AVX); **danielgomesvieira2000/burnout3-pc**: records the game's VU microprograms once, then builds them ahead of time; Road Trip (catsandsoup) and Waffle565's God of War verify VU1 AOT bit-identical against a reference (425k calls). Our runtime already compiles VU1 microcode ahead of time (capture + `vu1_recomp`), so extending that to VU0 microprograms (VCALLMS/VCALLMSR, 36 call sites in PoP) is the likely fix if the PC profile confirms VU0. Check each licence before reusing code; reading is fine.
- **To look into later (16 Oct 2026):** theclub654/ProjectCane, a native PC port (Windows/Linux) of Sly Cooper 1 (NTSC), rebuilt by reverse engineering; FFmpeg, shaders embedded at build time, a file extractor that reads the user's ISO. The README does not say how it renders models or handles sound, so read the code before step 5 (live native window) or the native sound mixer. **No licence named: read only, copy nothing.** Not Jade, so no format overlap. Also checked: lombyte-project/Lombyte (Ratchet & Clank matching decomp, GPL-3.0): not useful (matching-decomp route).

## Texture formats and the PC upgrade (12 Oct 2026)

`tools/jadetex.py` (unpack / list / png / compare) reads textures from the PS2 and PC bigfiles; its docstring has the format details. Summary:
- World packs (bigfile keys `ff0…`) are 0x20000-byte LZO1X chunks; unpacked, a pack is a list of records `[u32 size][0xEEFFC099][u32 key][data]`.
- Texture records (`+0x04` = 0xFFFFFFFF, magic 0xCAD01234 at `+0x18`, width/height at `+0x0C`): PS2 format 3 = 8-bit indices stored GS-swizzled (PSMT8 uploaded as PSMCT32) behind a GS upload packet, palette in a separate record (key at `+0x3C`, CSM1 order, alpha 0x80 = opaque). PC format 0xB = DXT1 (`+0x28` 5/11), DXT5 (7) or BGRA32 (0), with mip levels; data at `+0x38` (`+0x3C` when `+0x24` is 3).
- Same keys on both. In the Aviary pack (`ff0b5b96`) 97 of 131 shared textures are 4x the pixels on PC (128x128 -> 256x256 typically). Some PC textures also differ in tone (grass greener than PS2): check in game before trusting colours, the PS2 modulates textures by vertex colour (0x80 = 1.0).
- Game side (12 Oct 2026): the PS2 texture cache is `CJadeTextureManager` (array of 0x50-byte slots per texture index, at `*0x70000050`). `RegisterTexture` 0x4B3440 (slot: data pointer, width +4, height +6, bpp +0x1C, mip sizes), `LoadTexture` 0x4B3840 (if not resident, allocates GS memory from a wrapping running pointer, stores the GS address at slot +0x18 and uploads via 0x4B3DC0; many callers, it is the funnel). `PS2_Texture_Load(GDI_tdst_DisplayData*, TEX_tdst_Data*, TEX_tdst_File_Desc*, index)` 0x4B4E80 registers each texture: `TEX_tdst_Data` entries are 16 bytes and the **first word is the asset key** (seen with `POP_TEXLOG=1`, which logs these calls). So: index -> key at 0x4B4E80, index -> GS address after 0x4B3840; the renderer can map TEX0.TBP0 to an asset key. Implemented in the override (`g_gsTextures`, GS block address -> key/width/height/bpp; `POP_TEXLOG=1` logs changes). Checked on the Mac: addresses match the GS trace's upload destinations; the pool wraps, so the same texture is re-uploaded at a new address every frame. The manager is at 0x70000050 (scratchpad) and its first word is the slot array (read a0 before calling the original).
- Native loader `tools/overrides/pop_hdtextures.cpp` (`pop_hdtex::loadPack(key)`: PC bigfile index, LZO1X, texture records -> RGBA8; `POP_PC_BIGFILE`, test with `POP_HDTEX_TEST=<pack>:<texture>`): output identical to jadetex.py; the Aviary pack (132 textures) loads in 21 ms on the Mac. Still to decide: which PC pack to load for the textures the game uses (follow the world switch `FUN_00434FC0(loader, worldKey)`, or index texture key -> pack once at start-up in the background).
- World packs named by world switches are tiny (only the `.wow`); content packs are streamed separately. So PC packs are now loaded when the game looks up any `0xFF……` key (`BIG_ul_SearchKeyToPos` 0x488940 hook -> `pop_hdtex::onBigfileLookup`, up to 24 packs kept, `pop_hdtex::find(textureKey)`). Mac test 12 Oct evening: ~95% of registered textures have a PC version loaded (145 of the first 150), packs load in 0.3-10 ms (`POP_TEXLOG=1` prints "N textures registered, M with a PC version loaded").
- Open bug (12 Oct, PC): the loading-screen medallion now shows but orbits/drifts across the screen instead of rotating in place. Suspect the VU0-macro matrix code it uses: `FUN_00494500` (matrix multiply, VMULA/VMADDA.bc, LQC2/SQC2, translation row added with VADD) and `FUN_00494320` (vector transform, QFSRV/QMTC2/QMFC2). Write a self-test like `[pop:vu0test]` calling them on known matrices.
- Still to build (needs the PC): the SotC renderer side. Its shaders sample textures straight from emulated GS memory (`samplePoint`/`sampleTexture` in gs_gpu_shaders.cpp, no texture cache), so replacement = a GPU buffer of replacement images plus a per-draw index set when TEX0.TBP0/size match `g_gsTextures`; sample with normalised coordinates, alpha 255 -> 0x80. Renders at native 640x448, so the gain shows mainly on magnified (close-up) surfaces until internal upscaling exists.
- Plan (agreed): no conversion step. At runtime, when the game loads a PS2 world pack, also open the PC pack with the same key (if the user supplied the PC `prince.bf`), keep its textures resident on the GPU for the level, and substitute them when the game uses the matching PS2 texture. Identify textures at the game's texture loader (asset key known) rather than by hashing GS uploads. Load on a background thread; never upload high-res data per PS2 re-upload.

## PS2 prototype, 19 Aug 2003 (examined 12 Oct 2026)

Hidden Palace dump, under `game/proto/` (`archive/` the .7z, `extract/` the .bin/.cue, `disc/` the files, `analysis/` our outputs). Plain ISO9660, 2048-byte sectors; macOS `tar` extracts both the .7z and the ISO.

- `disc/SLUS_207.43` is **not stripped**: 30,207 symbols, 10,618 named functions (CodeWarrior-mangled C++), relocations (`.relmain`), built with Metrowerks CodeWarrior (`.mwcats`). Main section 0x3DDA80 bytes vs retail 0x3E82D0, so the code is close to retail.
- `disc/MAPFILE.MAP` (51k lines): linker map with libraries and objects, e.g. the VU1 drivers in `Ps2Graphic_cwf.lib` (`multchanneldriv…`, `animunpackeddri…`, `nolightdriver`, `particledriver`, `water3ddriver`, `shadowdriver`, `spg_sprite`; the map truncates object names to 15 characters).
- Other differences: `E3_INTRO.INT`, debug sound driver `DARE_DG.IRX`, smaller `PRINCE.BF` (189 MB).
- `analysis/proto_symbols.tsv`: the symbol table. `analysis/match.py` matches retail functions to prototype functions by instructions with immediates and jump targets masked: 4,750 unique exact matches, 1,910 ambiguous (identical small functions). Output: `analysis/retail_names_from_proto.tsv` (retail addr, proto addr, size, name, old name). Fuzzy matching (Ghidra Version Tracking) should raise the count.
- Movie player found: class `FullMovie` (retail 0x19AA40–0x19F440: `mLoadMovie`, `mDoFrame`, `mStreamData`, `mQueueSoundData`…), `MovieLoadMovie` 0x19F440, `VID_fn_vSetDMAFromIPU` 0x4A0840, Sony `sceIpu*` at 0x19A050–0x19A368. Sound engine names (`SND_*`, association/fade code) match too.
- Names applied to the retail Ghidra project (12 Oct 2026): `tools/proto_names.py` (proto ELF, retail ELF, `functions.csv`, PS2Recomp dirs) writes `game/proto/analysis/proto_names.tsv`; `tools/ghidra/PoPApplyProtoNames.java` applies it (a "Prototype names" stage in rebuild.sh / rebuild.ps1; copy the TSV to the PC). 4,292 functions renamed, 1,112 matches in C++ class namespaces (e.g. `FullMovie::mLoadMovie`), mangled name in a plate comment. Only default-named functions are touched. Where ps2_analyzer had already named a function, the prototype agreed every time.
- Names PS2Recomp would replace with its own stubs (its `ps2_call_list.h` and the exporter's runtime handler list) get a `pop_` prefix (230, e.g. `pop_memcpy`, `pop_sceIpuInit`), so the recompile does not change. Checked: same 301 stubs; 6 extra `untracked_stubs` (entry-point hints only); generated code identical apart from names (73,657 files). Ghidra backup from before: `game/ghidra/backup_20261012_pre_proto_names/`.
- Names and anything derived from the prototype stay under `game/`. Function names in our public code comments are a judgement call; never copy prototype code or data into git.

## Other Jade prototypes (checked 13 Oct 2026)

- PoP: The Two Thrones, Sep 29, 2005 PS2 preview (`game/proto-t2t/`, `SLES_537.77`): **stripped** (no symtab), no map file. No names to gain.
- Beyond Good & Evil, Aug 14, 2003 PS2 preview (`game/proto-bge/`, `SLUS_207.63`, disc label "Preview 8-20-03"): **unstripped**, CodeWarrior like ours: 7,486 named functions, `.relmain`, `.mwcats`, tiny `.debug`/`.line` (no useful types). But it adds almost nothing for SoT: `tools/proto_names.py` with it as the prototype matches 560 retail functions, 542 already named by the SoT prototype (533 identical names), and the 18 others are tiny duplicates (likely false). Only 1,869 function names are shared between the two prototypes (932 with the same size); the 5,617 BG&E-only names are mostly its own game code (AI 1,378, characters, vehicles) plus its sound (SND/SFX/ps2SND ~520) and graphics (GSP/GST/TEX/MAT ~370) layers, which Montreal's fork of Jade had already changed. Output: `game/proto-bge/analysis/bge_names.tsv`. Not applied.
- BG&E Sep 9, 2003 "Review" build (`game/proto-bge/sep09/`): also unstripped (7,628 functions); 577 retail SoT matches, 20 not named by the SoT prototype (2 not in Aug 14 either). Same conclusion. BG&E Sep 15, 2003 "V1.00 9-17-03" (`game/proto-bge/sep15/`): stripped.
- PoP: Warrior Within, Oct 25, 2004 "Review" build (`game/proto-ww/`, `SLUS_210.22`): **stripped**, no map file.
- So the Aug 2003 SoT prototype stays our only useful symbol source. Ubisoft's 2003 preview/review discs were often unstripped (SoT Aug 19, BG&E Aug 14 and Sep 9); release candidates and the 2004-05 PoP builds were not.

## Native renderer probe (step 7, started 13 Oct 2026, Mac only)

Prototype ELF imported as its own Ghidra project `game/ghidra/PoP_Proto0819` (all names; decompile with PoPDecompile.java by prototype address). Graphics code lives in `Ps2Graphic_cwf.lib` / `GraphicDK_cwfn.lib` (MAPFILE.MAP objects jadedrawmanager, jademesh, jadestaticmesh, jadeskinnedmesh, vumanager, geoobjectps2, ...).

Findings (PS2 side): **favourable for route D.**
- Display list design. `CJadeDrawManager::Display3DElements` (retail 0x4BC000, called from 0x4B4A80) draws everything through `CElementList::Render` (retail 0x4B9440, 9 lists in fixed order: opaque, mirrors, Z-biased, lightmaps `CLMElementList::Render` 0x4B7B80, shadows, transparent, after-effects). Sprites, particles, water, strings and indexed elements are added to the same manager.
- Matrices are separate. `CMatrixPool::AddMatrix` stores object-to-world matrices (64 B each, scale flag applied; view-space Z kept for sorting); `AddBoneMatrixForGAO` stores world bone matrices per game object. Camera matrices (`g_graphic_sys+0x200/0x240/0x280/0x300/0x340`) go to VU1 once per driver switch (`CVuManager::DoSetStaticEnv`, retail via `DoSetVUProgram` 0x4B7580).
- Geometry is static, loaded raw from the bigfile. `CJMeshElement::ReadElement` (retail 0x4C9500): bbox min/max (Z up, metres), bone palette bytes, LOD blobs, then per pass one ready-made DMA packet (`ret` tag + VIF). `CJMeshElement::Display` (retail 0x4C8A80) only sends a DMA call to that packet plus the element's matrix (UNPACK V4-32 x4) and MSCNT.
- Packet contents (decoded with `tools/jademesh_find.py` + `tools/vifdump.py`, Aviary pack ff0b5b96): static batches = V3-32 float positions, V3-16 UVs + strip-restart flag, V4-8 RGBA (baked lighting), V4-32 x3 batch bbox, MSCNT; triangle strips. Skinned batches add V3-8 bone indices (VU matrix offsets, /4 = palette index), V3-32 weights (up to 3), positions in bind space. Both convert directly to GPU vertex buffers at load time.
- So a native path can: convert packets at load (hook ReadElement), record each frame's element list + matrices + material (hook Render/AddElement), draw with our own camera (depth, motion vectors from previous-frame matrices per element).

Live check (13 Oct 2026, Mac debug build; `POP_GIF_TAP=1 POP_DRAWPROBE=1`, `tools/overrides/pop_drawprobe.cpp`; needs `tools/patches/ps2recomp-gif-arbiter-current-path.patch` in the runtime, local on the Mac `pop` worktree, not yet on the branch): the frame's DMA is kicked once inside PS2_Flip (0x4A20C0 -> kick 0x4A2C80), so GS work is split by GIF path. Title screen, opening and balcony gameplay: 8 element lists and 78-94 CJMeshElement draws per frame; VU1 (PATH1) 13-18k small textured triangles = 54-69% of drawn area, all scene-dependent; frames without Display3DElements have ~0 PATH1. PATH2 is the same every frame whatever the view: 120 large textured + 60 large untextured sprites (glow/after-effect passes, also into the half-res buffer fbp 280), ~200 small textured (HUD/overlays), so post-processing and 2D. Loading screen: drawn entirely outside the manager (PATH2 only, no flips); its sprites have huge coordinates (~245 Mpx per 10 s), consistent with the medallion drift bug. **Verdict: go for route D** (3D content all passes through the draw manager; the rest is 2D/post-processing to redo as shaders).
- Mac debug runs 13 Oct: after the balcony the curtain load never finishes (loading screen animates from a vsync handler, `FUN_004a5e80`). Not sound (on the Mac the sound engine never starts: `169b40` -> -1 without pop-sotc patch 05, no RPCs at all). `POP_TRACE=1` now also dumps the kernel-call ring after 30 s without PS2_Flip. Findings: all bigfile reads and LZO chunks succeed; the loader thread (stack 0xac19xx) ends with `WakeupThread(1)` + `SleepThread` (`FUN_00434f00`, cLoader); the main thread then processes the world (sema 6 alloc lock pairs, many `FUN_0048c500`), `FUN_0038c540` returns, it runs the epilogue `entry_00435970` (`jr $ra`), and never runs logged code or a kernel call again (no runtime error). The debug build's function log is `game/ps2/disc/ps2_log.txt` (every recompiled function's enter/exit, GBs; delete after use). Next: check whether the PC (pop-sotc) passes this load; if it does, treat as a Mac `pop` runtime dispatch bug.

Open: only 21 elements found in the Aviary pack (check other grammars: CJMeshElementRef, LOD > 0, non-speed-mode geo from separate keys); materials/blend state per element (MAT_tdst_MultiTexture, CJadeRenderStateManager); which draws bypass the manager (2D, after-effects, loading screen, GFX); PC POP.EXE comparison (its mesh format, whether PC meshes are higher detail); live check in the Mac build (count elements per frame vs GS draws).

## Renderer and DLSS notes

- Render path: translate GS draw calls into modern GPU calls (D3D12 or Vulkan). This is where texture replacement hooks in and where DLSS would attach.
- DLSS needs depth and motion vectors. The PS2 has no motion vectors; they would have to be derived from the game's own transforms. Check exactly what DLSS 5 requires when we get there (not researched).
- Path tracing on the recomp would be a further step after DLSS; RTX Remix is the shortcut for it on the PC build.
- Reference design: TheMoonPeople/Chronicle (Dark Cloud decomp + native SDL3/Vulkan port, https://github.com/TheMoonPeople/Chronicle). It emulates no PS2 hardware: every function that touches GS/VU/VIF/DMA/SPU2/IOP is replaced natively. Read before designing these parts:
  - `docs/PC_PORT_PLAN.md`: replacing the VU1 path with native mesh drawing from the game's own model data (gives real 3D scenes, needed for DLSS motion vectors and for RTX Remix on the recomp); a native audio mixer that replaces the IOP sound driver instead of emulating the SPU2.
  - `docs/DECOUPLING_PLAN.md`: fixed logic tick with interpolated presentation (each tick records a display list; frames are rendered between the last two). Blueprint for frame rates above 30 fps and for resolution independence.
  - Caveat: Dark Cloud has full decompiled source, no threads, no movies and no streaming audio. For PoP this would be a hybrid: the recompiled game plus native replacements at the hardware boundary, each function found in Ghidra first. Open question: does PoP's engine hand VU1 clean object-space meshes and separate matrices?

## Open questions

- ~~Does the US ELF contain debug symbols?~~ No. `SLUS_207.43` (v1.01, SHA-1 `a5bc95777d458e1dc36a9bd3d3cb7d3523d06c7d`) is stripped: no symtab, no `.mdebug`, one `main` section at `0x100000`, entry `0x100008`. About 48 source filenames survive in assert strings (`AIengine.c`, `BIGfat.c`, `OBJload.c`, `RPS2.cpp`, …), plus Sony lib strings (`sceGs…`, `sceCd…`).
- Does PS2Recomp build and run on Apple Silicon (at least the recompiler)?
- Is the PC version compatible with RTX Remix, with and without GOG's wrapper? Partial answer: `POP.EXE` is Direct3D 9 (not 8) and links D3DX shader assembly plus `CreateVertexShader`/`CreatePixelShader`, which lowers the odds; could be library code only. Needs a real test on Windows.
- Which engine name is correct (Jade vs Scimitar), and what file formats does the game use? Very likely Jade: PS2 `DATA/PRINCE.BF` and PC `prince.bf` both start with `BIG\0` version `0x25` (Jade bigfile, as in Beyond Good & Evil), and source-file prefixes (AI, BIG, COL, EVE, GRI, OBJ, MDF, LOA, INO) match Jade's module naming. Sound engine is DARE (`DARE_RT.IRX`). Neither "Jade" nor "Scimitar" appears as a string in the ELF.
- PC `POP.EXE` has a PDB path (`e:\builds\v181a (sans safedisk)\Output\ExeDlls\PC\POP4.pdb`) but no PDB, and only 8 RTTI names, so it gives little for naming functions.
- What does DLSS 5 require from the renderer?

## Sources

- PS2Recomp: https://github.com/ran-j/PS2Recomp
- GOG announcement: https://www.gog.com/en/news/prince_of_persia_the_sands_of_time_joins_the_gog_preservation_program (details are in GOG's linked blog post, not read)
- Recompendium catalogue: https://nio03.github.io/unricopie/en/
- Time Extension on PS2Recomp: https://www.timeextension.com/news/2026/01/native-pc-ports-of-ps2-games-could-be-on-the-way-thanks-to-new-recompilation-experiment
- ghidra-emotionengine-reloaded: https://github.com/chaoticgd/ghidra-emotionengine-reloaded
- RTX Remix compatibility: https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/introduction/intro-compatibility.html
- RTX Remix FAQ: https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/remix-faq.html
- Digital Citizen on GOG's wrapper: https://www.digitalcitizen.life/prince-of-persia-the-sands-of-time-gets-major-modern-pc-fixes-for-widescreen-physics-and-frame-timing/
- Ratchet & Clank decomp (example matching project): https://codeberg.org/isaka2025/RC1
- JadeStudio: https://www.github.com/4g3v/JadeStudio
- Texture packs (gbatemp.net threads for TexMaster, xXtherockoXx and the Remastered Project; Nexus Mods collection): https://www.nexusmods.com/princeofpersiathesandsoftime/mods/17
- Also covered in chat, not sourced here: Ridge Racer Type 4 recomp (PS1, omegakatana92/RidgeRacer4Recomp, v1.0.0 released 3 Sep 2026, built on PSXRecomp).
