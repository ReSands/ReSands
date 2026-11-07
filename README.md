# ReSands

ReSands is a static recompilation and port of Ubisoft Montreal's *Prince of Persia: The Sands of Time* for the PlayStation 2, originally released in 2003. The goal for the 1.0 release is a pixel-perfect reproduction of the original PS2 game, running natively on PC at 60fps, with zero emulation. Quality of life features like support for widescreen, uncapped framerates, and native rendering resolutions up to 4K are planned for version 2.0.

Yes, The Sands of Time already exists natively for PC thanks to a port that was released the same year as its PS2 version. But this version has issues on modern hardware that require fixes from [GOG](https://www.gog.com/blog/preserving-sands-of-time/) or projects like [OpenJade](https://github.com/dekamaru/openjade). Even with these fixes, problems such as hardcoded keyboard controls displayed in in-game tutorials remain. Personally I don't like how clean the PC game looks compared to the hazy, dreamlike PS2 original.

So the aim here isn't to produce the best-looking version of Sands of Time, but the most PS2 accurate one possible on modern hardware. So far, while building my native Vulkan renderer, I've been using emulated PS2 graphics as a source of truth, but the eventual goal is to set up real PS2 hardware and compare the output of its graphics and sound chips directly against what my reimplementation produces.

Only once version 1.0 is complete will I think about adding quality of life improvements to the game, including support for higher resolutions, framerates, and widescreen, the ability to import higher quality assets and audio from the PC version of the game, and modern graphical enhancements like support for dynamic shadows, fog, and reflective water.

ReSands is still in heavy development, and is far from complete. The game still drops to 30fps during heavy combat encounters, and the native Vulkan-based renderer occasionally produces graphical glitches. If you'd still like to give the project a go then I'd absolutely love you to, just know that at the moment you'd be serving as a beta tester.

## Trying it today

There's no ready-to-play download yet: ReSands only exists as source code, and you'd have to build it yourself. That's possible, but rough for now. You'll need a Windows PC, your own copy of the game, and an afternoon: the build pulls in Visual Studio's C++ tools, CMake, Python, Java and Ghidra (`tools/windows/setup.ps1` installs most of them), applies the patches in `tools/patches/pop-sotc/` to [PS2Recomp](https://github.com/ran-j/PS2Recomp), then recompiles the whole game, which takes up to an hour the first time (`tools/windows/rebuild.ps1`). The scripts are written for my own machine, so expect to adjust some paths. A proper step-by-step guide and downloadable builds will come with the first public playtest.

Please note that this is a bring-your-own-disc project, and no original game data is included in this repository or its downloads. You'll have to provide your own ISO of the NTSC version of the game (US, SLUS-20743; support for PAL and JP versions is planned for the future). The project is also not affiliated with, nor endorsed by Ubisoft or Ubisoft Montreal, and AI was used extensively in its development.

More of an Xbox person? Check out [reSOT](https://github.com/FulGerNet/rePOP), which looks awesome.

## Contributing

If you're interested in contributing to ReSands, here's how you can help:

1. **Playtesting.** Once builds are up (or if you build it yourself, see above), give the recomp a play. If you encounter any crashes, glitches, or anything that simply doesn't look, sound, or feel right, please report it. ReSands currently has a save state feature built into it and accessible via the Select button (unused in the original game). Hit the button when you encounter a bug and it'll generate a log file which you can include in your report to help me hunt down whatever bug you're encountering.
2. **Byte-matching.** This isn't strictly speaking required, but in the interests of learning and preservation, I eventually want to offer a byte-matched decompilation of the entire PS2 game. We're starting with the game's sound engine (DARE). Even with current AI tools this is an arduous and token-expensive process, so if you'd like to contribute some computing power, you should point your LLM of choice at the [AGENTS.md](AGENTS.md) file.

## 1.0 Roadmap

We're currently working on 0.3, with some of 0.4 and 0.5 done early.

- 0.1 - Playable start to finish with emulated graphics and sound **(done)**
- 0.2 - Graphics ported to a fully native Vulkan-based renderer **(mostly done: the native renderer is on by
  default; switching off the emulated graphics for good waits on comparisons with a real PS2)**
- 0.3 - Sound ported to a native engine **(in progress)**
- 0.4 - Native pre-rendered video playback **(done)**
- 0.5 - Native memory card and gamepad support **(in progress: memory card done; gamepad and rumble being tested)**
- 0.6 - Support for PAL and JP discs
- 1.0 - Cleaned up code, performance optimised

## 2.0 Roadmap (subject to change)

- 1.1 - Adding a graphics settings launcher
- 1.2 - Support for higher quality audio and pre-rendered movies from the PC version of the game
- 1.3 - Widescreen
- 1.4 - Uncapped framerate
- 1.5 - Support for native higher resolutions
- 1.6 - Modern graphical enhancements including ambient occlusion, dynamic shadows, fog and atmospheric scattering,
  reflective water, global illumination, volumetric smoke, and more
- 1.7 - Support for more PC assets such as high quality textures
- 1.8 - Support for Linux and macOS
- 2.0 - A fully byte-matched decomp

## What's in this repository

Our own tooling, runtime patches and notes, plus the hand-written decomp sources. No game data, executables, disc images, disassembly or recompiled output.

- `tools/`: analysis and build tooling, native renderer/sound code (`tools/overrides`, `tools/native`)
- `tools/patches/`: patches on top of [PS2Recomp](https://github.com/ran-j/PS2Recomp)'s runtime
- `decomp/dare/`: the byte-matching decomp of DARE (shared header and matched functions)
- `docs/`: design notes, progress and findings

## Licence

Our tooling, runtime patches and native code: GPL-3.0 (see `LICENSE`), since the runtime patches build on
PS2Recomp. The reconstructed game code under `decomp/` is a study of Ubisoft's work: we claim no rights to it, and it is not under the GPL.
