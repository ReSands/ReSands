# Project brief: Prince of Persia: The Sands of Time, PS2 recompilation and PC modernisation

Written 11 Oct 2026 from a planning chat. Claims marked (estimate) are judgement calls, not sourced. Claims marked (unverified) came from a single forum post or similar. Re-check anything marked with either before relying on it.

## Goal

Personal-use project to get a better PC version of Prince of Persia: The Sands of Time (Ubisoft Montreal, 2003) than the original PC port, using:

1. A static recompilation of the PS2 version (PS2Recomp), with a modern renderer.
2. Community HD texture packs, integrated into that renderer.
3. Later: DLSS (and possibly path tracing).

In parallel, test a much cheaper route: RTX Remix on the existing PC (GOG) version.

## Ground rules

- Personal use only. The game and its source belong to Ubisoft. Original source code is not available to us.
- Work from a legitimately owned disc (US NTSC release, serial SLUS-20743) and a legitimately owned GOG copy.
- Never commit or redistribute the ISO, ELF, game data, texture packs or recompiled game output. Keep them out of any git repo (use `.gitignore`). Only our own tooling and glue code may be shared.
- The PS2 disc is the base: the recomp must run from it alone, so movies come from emulating the IPU (adapt PCSX2's GPL-3.0-or-later implementation) and the game's own player runs. Assets from other versions the user owns (GameCube, Xbox, PC) are optional enhancements on top: the "ultimate version" goal (12 Oct 2026, see "Cross-version assets"). This revises the earlier "PS2 disc only" rule.
- Unreleased prototypes may be used locally for research and reference (user decision, 12 Oct 2026). Like all game material, they stay under `game/` and never reach git.
- Provenance: our code (overrides, HLE replacements, tools) comes only from the PoP binary, our own analysis, public hardware documents and open-source projects under compatible licences. Never use leaked Sony SDK source, headers or samples (e.g. ezmpeg), and do not write such code from memory. Precedent: OpenRAC/rac1-decomp reverted its whole movie player for this (its `docs/MOVIE.md`).
- Be straightforward in status reports: say plainly when something does not work.

## Inputs

- PS2 disc image (ISO) of the US release, dumped from the owner's own copy. ELF should be `SLUS_207.43`.
- GOG copy of the PC version (includes GOG's preservation-program wrapper: widescreen/ultrawide, camera scaling, FPS limit, multi-monitor mouse lock).
- PS2Recomp: https://github.com/ran-j/PS2Recomp (GPL-3.0).

## Local layout

All game material lives under `game/`, which is git-ignored. Never move game files outside it.

- `game/ps2/`: the downloaded `.7z`; `iso/` (the ISO); `disc/` (extracted disc contents, incl. `SLUS_207.43` and `DATA/PRINCE.BF`).
- `game/gog/installers/`: GOG offline installer (`setup_…_181_gog_v2_(92028).exe` + `-1.bin`) and the 28547→92028 patch. Install output, if extracted on the Mac, goes in `game/gog/install/`.
- `game/ghidra/`: Ghidra project `PoP_SLUS20743` for the PS2 ELF, plus headless logs.
- `game/gc/` (GameCube), `game/xbox/` (Xbox), each with `iso/`, `disc/`, `analysis/`; `game/proto/` (PS2 prototype).

## Mac tool setup (done 11 Oct 2026)

- Tools live outside the repo in `~/tools/`: `ghidra_12.1.4_PUBLIC/` (EE Reloaded v2.1.38 installed in `Ghidra/Extensions/`), `PS2Recomp/` (clone).
- Java: Homebrew `openjdk@21`. Ghidra's launcher does not find it on its own in non-interactive shells; export it first:
  `export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home PATH=/opt/homebrew/opt/openjdk@21/bin:$PATH`
- Ghidra ships no macOS natives. They were built with `cd ~/tools/ghidra_12.1.4_PUBLIC/support/gradle && ./gradlew buildNatives` and copied from each module's `build/os/mac_arm_64/` into `os/mac_arm_64/`. Redo this after any Ghidra upgrade.
- EE language ID for headless imports: `r5900:LE:32:default`.
- `innoextract` (Homebrew) unpacks the GOG installer.
- PS2Recomp recompiler builds on Apple Silicon (uses sse2neon on ARM). Homebrew `cmake` + `ninja`; recompiler only:
  `cmake -S ~/tools/PS2Recomp -B ~/tools/PS2Recomp/build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF && cmake --build ~/tools/PS2Recomp/build-mac`
- Export from Ghidra headlessly (script's two `askFile` prompts are answered by positional args):
  `analyzeHeadless game/ghidra PoP_SLUS20743 -process SLUS_207.43 -noanalysis -readOnly -scriptPath ~/tools/PS2Recomp/ps2xRecomp/tools/ghidra -postScript ExportPS2Functions.java <abs>/game/recomp/config.toml <abs>/game/recomp/functions.csv`
- Recompile: `~/tools/PS2Recomp/build-mac/ps2xRecomp/ps2_recomp game/recomp/config.toml` (about 15 s, 0.8 GB RAM; 59,674 `.cpp` files, 458 MB in `game/recomp/output/`).

## First recompile results (11 Oct 2026, PS2Recomp `2c5fbb9`)

- Completed. 12,371 unhandled-instruction errors, all inside three Ghidra "functions" that are mostly data, not code:
  - `entry_004eb020` (0x4EB020–0x4ED920): data tables.
  - `entry_004ed920` (0x4ED920–0x52DDEC): data (libipu tables etc.) running into embedded IOP modules.
  - `entry_00652e40` (0x652E40–0x66D080): starts as real MIPS code, then runs into data. Do not skip it wholesale; split it in Ghidra.
- The ELF embeds seven IOP modules (R3000 ELFs) as data from 0x52DBC0: sio2man, padman, (unnamed at 0x53A500), mcserv, Sound_Device_Library (libsd), sdr_driver, UbiSound_Driver. They are loaded at runtime from memory, not from `MODULES/`. ps2xIOP/HLE must cover them.
- 4,458 control-flow warnings: unresolved `jr`/`jalr` (mostly switch tables) handled by fallback entries. 237 "Unknown MMI function" decoder messages, also data decoded as code.
- Fixed the same day with `tools/ghidra/PoPMarkDataRanges.java` (run headless with `-process SLUS_207.43 -noanalysis -scriptPath tools/ghidra -postScript PoPMarkDataRanges.java`, before the export). It clears code in 0x4EB020–0x651380 and 0x652E64–0x66D080, removes the 404 bogus functions there (mostly IOP code), trims `FUN_00652e40` to 0x652E40–0x652E64, and labels the IOP modules `irx_*`. It found no calls or jumps from real code into those ranges.
- After cleanup: 7,418 functions + 49,140 labels; 0 errors, 0 unknown-MMI messages; 4,291 warnings, all unresolved `jr`/`jalr` with fallback entries; 56,561 `.cpp` files, 404 MB.
- Real EE code: 0x100000–0x4E82D0 (main) and 0x651380–0x652E64 (a small block of short functions after the data; not yet identified).

## Runtime build on the Mac (11 Oct 2026)

- Builds. A PS2Recomp worktree at `game/recomp/runtime-src` (ignored) holds the generated code: `output/*.cpp` copied into `ps2xRuntime/src/runner/` (replacing the placeholder `register_functions.cpp`), `output/*.h` into `ps2xRuntime/include/`. Needs Homebrew `ffmpeg` (found through pkg-config).
- Configure with `-DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF`; build about 7 min on 8 cores (1,879 steps). Output: `build-mac/ps2xRuntime/ps2EntryRunner`, arm64, 179 MB. 0 errors, 22 warnings.
- Known upstream bug (not yet fixed): 14 generated `vcallmsr $vi27` sites read `ctx->vi[27]`, but `vi` has only 16 entries. VI27 is the VU0 control register CMSAR0, which the runtime does not model there, so these reads are out of bounds (undefined behaviour) when the game starts VU0 microprograms.
- `tools/rebuild.sh` does the whole loop (PS2Recomp tools from the `pop` branch, Ghidra cleanup, SDK naming, export, recompile, sync, runtime build); `tools/boot.sh [secs] [logname]` boots with a time limit (exit 142 = time limit).

## Boot log (11 Oct 2026)

- Boot 1: hung on an indirect call to 0x651380, a C++ static constructor. Cause: the ELF is one executable block in Ghidra, so the export's data function-pointer scan never ran. Fixed by splitting blocks in `PoPMarkDataRanges.java` (incl. a `.ctors` block, 0x652E70–0x652F30, 48 entries) and creating functions at every `.ctors` target (3 are prologue-less leaf functions the export heuristic misses).
- Boot 2: hung in `sceSifInitRpc` (0x10B500) waiting on `sceSifGetSreg` for an IOP reply. Cause: stripped ELF, so no SDK names, so PS2Recomp's HLE stubs were not used. Fixed by running `ps2_analyzer` (embedded SCE signature DB, 508 matches) and applying the names in Ghidra (`PoPApplyNames.java`).
- Boot 3: first GS commands (display setup, buffer swaps, clear sprites); SIF RPC init via HLE; the 7 embedded IOP modules are DMA'd to IOP RAM. Then freezes about 5 s in, inside `sceCdLayerSearchFile` (0x101FA8, loop at 0x102060). No HLE handler for it (runtime has `sceCdSearchFile`); the analyzer also flags it as self-modifying. Planned fix: Game Override mapping it to `sceCdSearchFile`, but check upstream issue #264 first (overrides registered from the static library are never applied).
- Boot 4 (on `pop`): same as boot 3.
- Boot 5: game override `tools/overrides/pop_slus20743.cpp` (keyed by entry 0x100008 + CRC-32 0x7E7889A9) binds 0x101FA8 to `sceCdSearchFile`. Works: `\SYSTEM.CNF;1` found, CDVD streaming thread starts, GS image upload. Then hangs in 0x102A48 = `sceCdDiskReady` (unnamed by the analyzer; binds cdvdfsv RPC 0x8000059A).
- Boot 6: override also binds 0x102A48 to `sceCdDiskReady`. Passes. Then a vtable call (vtable 0x653230) to 0x134C80, which Ghidra never disassembled.
- Boot 7/8: `PoPMarkDataRanges.java` now creates functions at undisassembled code targets of data pointers that start like functions (741 + 167 recovered; 8,384 functions total). Boot 8 then reaches the game's own C++ exception: `exception.cpp` line 35, "An exception is about to be thrown". Next: find what throws.
- Other unnamed functions in the CD library range, identified: 0x1022C0 = `sceCdSearchFile` wrapper (calls LayerSearchFile with layer 0); 0x1022E0 / 0x1025F8 = internal N-/S-command RPC helpers (servers 0x80000595 / 0x80000593; only caller is inside the HLE'd `sceCdRead`); 0x101380/0x101880 = allocator internals; 0x103240 = game's command-line option parser.
- Boots 9-20 (heap): the C++ exception was ubi::FileNotFoundException "Virtual path not found: /exedir\DATA\PRINCE.BF" because the CD device object got a garbage address. Root cause: PS2Recomp HLEs (sceGsExecLoadImage's 48 KB GS packet, thread stacks, SIF, MPEG, fonts, libc) allocate with runtime->guestMalloc, whose region the runtime takes from the game's SetupHeap (0x6E6700), and this game runs its own dlmalloc there (arena from 0x6E7000). Override fix: wrap _InitSys (0x111C68) to move the runtime heap to 0x80000-0x100000; EndOfHeap returns 0x1F00000.
- Boots 21-25 (CD): the game's CD worker thread waits for its sceCdCallback completion callback (0x14EFC0, iSignalSema(device+0x4040)); PS2Recomp's sceCdCallback is a no-op. Override fix: after a successful HLE sceCdRead, signal that semaphore (device pointer global 0x66D254).
- Boot 26 (IOP): sceSifLoadModuleBuffer takes an IOP address (the game DMAs each IRX there first); PS2Recomp reads EE memory at that address. Override fix: track SIF DMAs and pass the EE source. All 7 modules (incl. UbiSound_Driver, RPC 0x12345678) now run on the emulated IOP.
- Boots 27-33 (BIG loader): CD data and LZO1X decompression verified correct (tools/lzo1x.py). Bug: the EE ignores the low 4 address bits for LQ/SQ (and LQC2/SQC2); PS2Recomp did not, so unaligned LQ+QFSRV copies were shifted. Recompiler fix on the local `pop` branch (commit 58c553c; tools/patches/ps2recomp-lq-sq-align.patch). Apply it on the PC too.
- Boots 34-36: no loader errors; main frame loop runs (double-buffered flips, memory-card polling), world "MenuBoot" loaded. Frames black.
- Boots 37-49 (render path, `[pop:gfx]`/`[pop:gif]` diagnostics, tools/gsvram.py): VIF1/VU1/GIF/GS rasterizer all work. The text was drawn with vertex alpha 0 because the game's fade never advanced. Cause: PS2Recomp's `mfc0 Count` read a field nothing updated, so game time stood still. Fix: Count derived from host time at 294.912 MHz (pop 55ada1c, tools/patches/ps2recomp-cop0-count.patch).
- Boots 50-51 (movies): game reaches UBISOFT.INT; no IPU (MPEG) emulation, so the movie manager stays busy. Override `kSkipMovies` reports *.INT as not found; the game skips UBISOFT, DOLBY, INTRO, POP_IN, POP_OUT. A real fix needs an IPU/MPEG-2 decoder.
- Boots 52-56 (functions): an indirect call to 0x41CD00, a frameless leaf only reached through a method table built at runtime. New pass createAfterPaddingFunctions (code after zero padding following jr ra / j). It rejects candidates whose range up to the next function contains reserved SPECIAL encodings (VU/DMA packets, jump tables), and trims thunk bodies that Ghidra extended to a distant shared tail (they had become 6.7 MB C++ files and 25-minute links). The Ghidra project was re-imported once (old one in game/ghidra/damaged_20261012/).
- Boots 56-58 (VU0): FUN_00475380 runs a VU0 microprogram, then copies a VU0-computed count of quadwords into a 512-byte stack buffer; a garbage count smashed saved registers. Cause: PS2Recomp's VU0 macro VLQI/VLQD/VSQI/VSQD/VILWR/VISWR used EE RAM at vi*16 instead of VU0 data memory (0x11004000); VSQI/VSQD swapped VF/VI fields; VILWR/VISWR used word addressing. Fix: pop 6ca8b5d, tools/patches/ps2recomp-vu0-macro-memory.patch.
- Boot 58: title screen ("PRESS START BUTTON"). Boots 59-62: scripted input (override, `POP_INPUT`) reaches the main menu and starts a new game; the 3D scene's vertices were garbage.
- Boots 63-70 (libvu0): the projection matrix was wrong. The override's VU0 self-test (calls the game's own libvu0 on known inputs) showed PS2Recomp's C++ stand-ins for libvu0 are wrong (sceVu0MulMatrix order reversed; InnerProduct/Normalize include w). rebuild.sh / rebuild.ps1 now rename `sceVu0*`/`_sceVu0*` in the export so the game's real libvu0 is recompiled. Title screen 3D background and the opening scene render.
- Boots 71-74 (glow): every frame had an orange/green wash. Pass-by-pass capture (`POP_PASSDUMP_AT`) showed the glow pyramid (built in corners of the half-res buffer that shares memory with the Z buffer) bled Z data. Cause: the CPU rasterizer's DrawSprite truncated positions/UVs to whole pixels/texels. Fix: pop 46a8ac5 (tools/patches/ps2recomp-gs-sprite-subpixel.patch). Frames now match the PS2.
- Boots 75-77: after the opening the player has control; scripted left-stick input moves the Prince. Same result on the Windows PC (tools\windows\boot.ps1 -PadInput ...). Speed ~3-4 fps (CPU rasterizer, cycle-accurate VU1 interpreter, verbose runtime logging).
- Boots 78-81 (Mac, 12 Oct 2026, Bink movies): `tools/overrides/pop_movies.cpp` replaces the wrappers around the game's `FullMovie` (0x19F440-0x19F700: load, unload, do frame, next/goto frame, is-waiting, frame count, width, height, current frame, video output) and plays the matching Bink file from another version with FFmpeg (video on a decoder thread, audio through its own raylib stream). The game's own `VIDEO_MainCall` (0x4378C0: skip button, looping, sound pause, centring) and `PS2_DisplayVideo` (0x4B4340: uploads 16x16 RGBA blocks to the framebuffer) stay in use; the guest frame buffer comes from the game's allocator (0x6803B0, alloc vtable+0xC, free +0x10). Bink frames keep the PS2 pixel shape (640x448 full screen, 640x346 letterboxed). Without a Bink file (e.g. `DOLBY.INT`) the old skip applies. `POP_MOVIE_DIRS` (';'-separated, default Xbox `../../xbox/disc/VIDEO`, then GOG, then GameCube, relative to game/ps2/disc), `POP_MOVIES=0` off; debug: `POP_MOVIE_TEST=1` (coordinate test pattern), `POP_MOVIE_FORCE=<file>` (one file for every movie). Xbox 5.1 = four tracks per language (front stereo, centre mono = dialogue, LFE mono, surround stereo; order measured by loudness), mixed to stereo with centre and surrounds at -3 dB, LFE dropped; per-language Xbox files (`POP_IN0`-`4.INT`) are found by a language digit. Ubisoft logo, intro, POP_IN/POP_OUT and (forced) Xbox CINE_010 play with sound on the Mac (~3 fps there, so frames are skipped to follow the clock). Not yet done: checking the language-to-track order, reaching CINE_010 in play, the PC build.
- Mac runtime bug found by it: VIF1 treated whatever followed a DIRECT ending in a GIF IMAGE tag as image data, so the next DMA tag's VIFcodes became 2 pixels per 16x16 block (dot grid, 2-pixel shift). Fixed like the SotC runtime (continuation only inside the next DIRECT): tools/patches/ps2recomp-vif1-direct-image-continuation.patch, pop 73fe0d6. The PC's pop-sotc runtime already handles this.
- Mac presenter: in interlaced field mode it showed one field line-doubled per vsync (scanlines, one-line bob). PoP draws full 448-line frames, so it now shows the whole frame (weave), as the SotC presenter does; `PS2X_FIELD_BOB=1` restores the old way. tools/patches/ps2recomp-gs-present-weave.patch, pop 73fe0d6.
- PC session 12 Oct 2026, evening (pop-sotc, 30 fps): Bink movies play; past CINE_010 into the first in-engine cutscene and gameplay. Two PS2Recomp translation bugs fixed in the override (worth reporting upstream):
  - `rand` (0x118918) is named by ps2_analyzer, so PS2Recomp substitutes `std::rand() & 0x7FFF`; the game's newlib rand returns 31 bits and `lRand` (0x1B9A40) scales by 1/2^31, so `lRand(a, b)` always gave `a` and the cutscene spun forever. Override `newlibRand` runs the game's own algorithm on its own state (`*(0x4E8768) + 0xA8`, x 0x5851F42D4C957F2D + 1, top 31 bits). Better long-term: stop stubbing `rand` in the export like libvu0, so the original is recompiled.
  - `bc2t` is translated as `ctx->vu0_status & 1` (VU0's Z flag) instead of "VU1 running". The game's only bc2t is the VU1-idle wait `FUN_004A4D00` in the VIF1 kick routine 0x4A2C80; it spun once VU0 maths left Z set. Override returns at once.
- Input: the runtime maps WASD to the stick only in analog mode, which PoP never selects, and PoP does not walk with the D-pad; the override maps the arrow keys to the left stick. Letter keys are physical positions (awkward with Dvorak and through Moonlight). A DualSense on the PC (USB) works directly, sticks included.
- Remote play: Sunshine on the PC (winget `LizardByte.Sunshine`, service; web UI https://localhost:47990, login `jon`, password in the Mac keychain item "Sunshine gaming-pc"), Moonlight on the Mac (brew cask). Pairing over SSH: `GET /api/pin` lists pending requests, `POST /api/pin` with `{pairing_id, pin, name}`. ViGEmBus not installed (only needed to forward a Mac-side controller).
- PC sync: the PC cannot `git pull` from the Mac over SSH (access denied); send commits as a bundle (`git bundle create … <from>..main`, scp to game\jobs\pop.bundle, `git pull --ff-only game\jobs\pop.bundle main`). Run PowerShell over SSH with `-EncodedCommand` (quoted `-Arguments` break otherwise). `tools/windows/watch-job.sh` reads the build log under game\recomp, so it reports false stalls for `-Out game\recomp-sotc`.
- Low-priority wishes: camera X invert (the game has no option; the SotC runtime has `PS2X_INVERT_RIGHT_STICK=x`, not yet wired into boot.ps1); forward the game's vibration to the DualSense (not implemented in the runtime).
- Fixed: the "garbage transition" was the loading screen (two rotating layers of the golden medallion, `FUN_004C9C80`; images loaded on each world change by `FUN_004CA700` from bigfile keys 0xF402E7E7/E8, 256x256 PSMCT32, uploaded with `sceGsSetDefLoadImage(…, 0x3480/0x3880, 4, …)` + `sceGsExecLoadImage`). PS2Recomp's `sceGsExecLoadImage`/`StoreImage` stand-ins multiplied the block address by 8 (treating it as 2 KB units), so the images landed in the Z buffer. Fix: runtime patch `tools/patches/pop-sotc/04-gs-loadimage-block-address.patch` (pop-sotc e978a33 on the Mac, 2b44292 on the PC; pop 9f0d60f). Worth reporting upstream.
- Debugging recipe that found it: F5 quick-save (`game\ps2\disc\states\quick.state`; the log shows `State saved (field N)`), then boot with `-Env 'PS2X_STATE_LOAD_AT=120:states/<file>;PS2X_GS_TRACE=<N>:<fields>:<path>'` (text log of GS draws `D`, transfers `T`, vsyncs `V`, plus a VRAM dump `<path>.vram`; decode with `tools/gsvram.py <vram> <fbp> <fbw> <w> <h> <png>`). Gameplay traces are large (~60 MB/s). Other runtime switches seen: `PS2X_INVERT_RIGHT_STICK=x`, `PS2X_GS_RECORD`, `PS2X_STATE_SAVE_AT`. git on the PC has no identity (`git -c user.name=… -c user.email=… am`).
- Not done: movies (no IPU emulation; `kSkipMovies` skips them), sound (the game's IOP driver runs but PS2Recomp does not emulate the SPU2; register writes are only stored), performance (~2.7 fps gameplay on the Mac with logging off).
- Local debug edits in game/recomp/runtime-src (not committed to pop): dispatch trace collapses repeats; VU0 microprogram call logging (`[vu0:call]`/`[vu0:done]`, first 40 calls).
- Override debug switches in tools/overrides/pop_slus20743.cpp: kRenderStats (GIF/DMA/VRAM sampling, VRAM/VU1/RAM dumps to game/recomp/screens/), kForceZtstAlways (experiment, off), kSkipMovies, kWatchParticleFrame (debug, can go), kFrameDump, kDebugTracing (heap debugger, file-system and memset guards), kKernelCallLog (kernel-call ring buffer dumped on reader stall, bigfile read/decompress/BIG reader traces, LZO dumps to game/recomp/dumps/).
- Run-time environment variables read by the override: `POP_INPUT` (scripted pad input, e.g. `start@55,start@65,cross@85,cross@95,lleft@150:8`; buttons by name, left stick `lup/ldown/lleft/lright`, `@seconds[:hold]`), `POP_ASYNC_GS=1` (GS rasterizer on a worker thread, drained at each flip; ~1.3x), `POP_GIF_SAMPLE_AT=<s>` (GIF sampler start), `POP_PASSDUMP_AT=<s>` (per-pass framebuffer PNGs of post-processing). `[pop:fps]` lines give frames per second (end-of-frame flip hook), `[pop:vu0test]` the libvu0 self-test.
- Play builds (PS2Recomp's verbose logging compiled out, ~1.55x): Mac `game/recomp/runtime-src/build-mac-play` (configured by hand: Release, `-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_RUNTIME_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF`), run with `POP_BUILD=build-mac-play tools/boot.sh`; PC `tools\windows\rebuild.ps1 -Fast -Play` and `tools\windows\boot.ps1 -Play` (sets POP_ASYNC_GS=1). Measured gameplay: Mac 3.5 fps, PC 1.9 fps (12 Oct 2026).
- `tools/rebuild.sh --fast` skips Ghidra and the recompile (override/runtime-only changes). Moving the build loop to the Windows PC: see `tools/windows/SETUP.md`.

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
- The PC version of Sands of Time is a DirectX game, so it might be compatible. No existing Remix project for it found. Compatibility unknown; test it.
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
- Symbol check for each new disc: `.map` files on the GameCube disc, symbol strings in `main.dol`, debug sections or a PDB in the Xbox `default.xbe`, an unstripped ELF in any prototype. Names found can be carried to the PS2 ELF with Ghidra Version Tracking.

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

## Renderer and DLSS notes

- Render path: translate GS draw calls into modern GPU calls (D3D12 or Vulkan). This is where texture replacement hooks in and where DLSS would attach.
- DLSS needs depth and motion vectors. The PS2 has no motion vectors; they would have to be derived from the game's own transforms. Check exactly what DLSS 5 requires when we get there (not researched).
- Path tracing on the recomp would be a further step after DLSS; RTX Remix is the shortcut for it on the PC build.
- Reference design: TheMoonPeople/Chronicle (Dark Cloud decomp + native SDL3/Vulkan port, https://github.com/TheMoonPeople/Chronicle). It emulates no PS2 hardware: every function that touches GS/VU/VIF/DMA/SPU2/IOP is replaced natively. Read before designing these parts:
  - `docs/PC_PORT_PLAN.md`: replacing the VU1 path with native mesh drawing from the game's own model data (gives real 3D scenes, needed for DLSS motion vectors and for RTX Remix on the recomp); a native audio mixer that replaces the IOP sound driver instead of emulating the SPU2.
  - `docs/DECOUPLING_PLAN.md`: fixed logic tick with interpolated presentation (each tick records a display list; frames are rendered between the last two). Blueprint for frame rates above 30 fps and for resolution independence.
  - Caveat: Dark Cloud has full decompiled source, no threads, no movies and no streaming audio. For PoP this would be a hybrid: the recompiled game plus native replacements at the hardware boundary, each function found in Ghidra first. Open question: does PoP's engine hand VU1 clean object-space meshes and separate matrices?

## Suggested order of work

1. Install the GOG copy; try RTX Remix on it (an evening's test).
2. Buy/dump the US disc; extract ISO and ELF; note the exact serial.
3. Ghidra + Emotion Engine Reloaded; check for debug symbols; export TOML/CSV.
4. Recompile; get the build working on the Windows PC.
5. Boot loop: fix blockers (I/O, syscalls, input, audio).
6. Build the renderer; add texture replacement (PCSX2-compatible hashing).
7. Later: DLSS.

Update 12 Oct 2026, 11:25: **30 fps gameplay on the PC** with the `pop-sotc` setup (PS2Recomp fork TaylorNAlbarnaz `sotc-port` + our fixes, branch `pop-sotc`, output `game/recomp-sotc`; GPU GS renderer; PoP's VU1 microcode compiled with `vu1_recomp`). Build: `rebuild.ps1 -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc [-Vu1 <capture dir>]`; boot: `boot.ps1 -Play -Out game\recomp-sotc` (`-Env`, `-Shots`). Capture new VU1 programs with `-Env 'PS2X_VU1_CAPTURE=<dir>'`. The sotc base does not build on the Mac (x86 SSE headers). Details and fork survey: `game/FORKS.md`.

Earlier status (12 Oct morning): steps 2-5 largely done. The game reaches gameplay on Mac and PC, but at 2-3.5 fps, with no movies and no sound. Next, roughly in order of value:

- **Speed.** GPU GS renderer (D3D12/Vulkan; this is also step 6's hook for texture replacement and DLSS). A faster VU1 path: the interpreter is cycle-accurate and costs ~1/3 of the game thread; options are a non-cycle-accurate mode or recompiling the game's VU1 microprograms. On the PC, try clang-cl (MSVC Dev build is ~1.8x slower than clang on the Mac); a Release/LTCG link did not finish within 90 minutes on the 5800X (12 Oct).
- **Movies.** PoP-specific HLE of the game's movie player (libipu-based; .INT files in MOVIEPS2.BIG), decoding with the runtime's FFmpeg MPEG-2 path instead of emulating the IPU.
- **Sound references:** OpenGOAL (`open-goal/jak-project`, ISC licence) `game/sound/common/` has a native SPU2-style voice synth (ADPCM decode, ADSR envelopes, pitch, mixing; no reverb seen), and `game/sound/sdshim.*` a minimal libsd stand-in. Use them to check the SotC fork's SPU2, or as the base of a native mixer if we replace the sound path. Its `989snd` is Sony's 989 library (Jak only), irrelevant to PoP's DARE/UbiSound. OpenGOAL has no IPU/movie code (Jak has no FMV).
- **Sound.** SPU2 emulation for the IOP (the game's own UbiSound driver already runs), or HLE of the sound RPC.
- **Correctness sweep.** Other PS2Recomp C++ stand-ins may be wrong like libvu0 was. libm `sin/cos/atan/floor/ceil/fabs` were checked by the self-test and are correct (float ABI); the internal `__kernel_*`/`__ieee754_rem_pio2f` stand-ins are simplified but only matter if called directly. Extend `[pop:vu0test]`-style checks before trusting other stubs.

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
