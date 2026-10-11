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
- Be straightforward in status reports: say plainly when something does not work.

## Inputs

- PS2 disc image (ISO) of the US release, dumped from the owner's own copy. ELF should be `SLUS_207.43`.
- GOG copy of the PC version (includes GOG's preservation-program wrapper: widescreen/ultrawide, camera scaling, FPS limit, multi-monitor mouse lock).
- PS2Recomp: https://github.com/ran-j/PS2Recomp (GPL-3.0).

## Local layout

All game material lives under `game/`, which is git-ignored. Never move game files outside it.

- `game/ps2/`: the downloaded `.7z`; `iso/` (the ISO); `disc/` (extracted disc contents, incl. `SLUS_207.43` and `DATA/PRINCE.BF`).
- `game/gog/installers/`: GOG offline installer (`setup_…_181_gog_v2_(92028).exe` + `-1.bin`) and the 28547→92028 patch. Install output, if extracted on the Mac, goes in `game/gog/install/`.

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

## Renderer and DLSS notes

- Render path: translate GS draw calls into modern GPU calls (D3D12 or Vulkan). This is where texture replacement hooks in and where DLSS would attach.
- DLSS needs depth and motion vectors. The PS2 has no motion vectors; they would have to be derived from the game's own transforms. Check exactly what DLSS 5 requires when we get there (not researched).
- Path tracing on the recomp would be a further step after DLSS; RTX Remix is the shortcut for it on the PC build.

## Suggested order of work

1. Install the GOG copy; try RTX Remix on it (an evening's test).
2. Buy/dump the US disc; extract ISO and ELF; note the exact serial.
3. Ghidra + Emotion Engine Reloaded; check for debug symbols; export TOML/CSV.
4. Recompile; get the build working on the Windows PC.
5. Boot loop: fix blockers (I/O, syscalls, input, audio).
6. Build the renderer; add texture replacement (PCSX2-compatible hashing).
7. Later: DLSS.

## Open questions

- Does the US ELF contain debug symbols?
- Does PS2Recomp build and run on Apple Silicon (at least the recompiler)?
- Is the PC version compatible with RTX Remix, with and without GOG's wrapper?
- Which engine name is correct (Jade vs Scimitar), and what file formats does the game use?
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
