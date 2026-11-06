# ReSands

A work-in-progress PC version of *Prince of Persia: The Sands of Time* (Ubisoft Montreal, 2003), built by
statically recompiling the PS2 release (via [PS2Recomp](https://github.com/ran-j/PS2Recomp)) and replacing the
PS2 hardware at the engine level: a native Vulkan renderer, native sound, native movie decoding, memory card
and pad. The aim is to keep the PS2 look (haze, post-processing) while running natively on PC.

**Status:** early and unfinished. Playable start to finish; native graphics on by default; native sound under way.
See [`docs/roadmap.md`](docs/roadmap.md) for the release plan.

## What is (and isn't) here

This repository holds our own tooling, runtime patches and notes, plus the hand-written decomp sources under
`decomp/`. It contains **no game data, executables, disc images, disassembly or recompiled output**, and none will
be distributed. *Prince of Persia: The Sands of
Time* belongs to Ubisoft; you need your own copy of the game (US PS2 disc, SLUS-20743).

## Help wanted: byte-matching decomp

We are rebuilding the game's sound engine (DARE) as C++ that compiles to the original machine code, one function
at a time, and AI-assisted contributions are welcome. Point your agent at [`AGENTS.md`](AGENTS.md): it covers the
setup (your own disc, the CodeWarrior compiler from decomp.me's archive), the work loop and the rules.

## Layout

- `tools/`: analysis and build tooling, native renderer/sound code (`tools/overrides`, `tools/native`)
- `tools/patches/`: patches on top of PS2Recomp's runtime
- `decomp/dare/`: the byte-matching decomp of DARE (shared header and matched functions)
- `docs/`: design notes, progress and findings

## Licence

Our tooling, runtime patches and native code: GPL-3.0 (see `LICENSE`), since the runtime patches build on
PS2Recomp. The reconstructed game code under `decomp/` is a study of Ubisoft's work: we claim no rights to it, and
it is not under the GPL.
