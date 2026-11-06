# 0.3 native sound: plan of attack

Written 6 Nov 2026 (morning), when 0.3 started. Goal (roadmap 0.3): sound comes from the engine's DARE/UbiSound
calls through our own mixer, with no IOP or SPU2 emulation. Older notes this plan builds on:
`docs/native_renderer.md` "QA plan for sound" and the "Sound:" step list near "Native world in the game window";
GT4Recomp pacing in `docs/reference.md`; the 14 Oct stream bug history in `docs/history.md`.

## Decisions (6 Nov)

- **Not waiting on the DARE byte-matching decomp.** 0.3 keeps DARE's EE code exactly as it runs recompiled
  today and replaces what sits under it (the IOP emulation and SPU2; route chosen in step 3). A byte match would give cleaner source for the
  EE functions that send commands, but no new knowledge: Ghidra + POP1.ELF's DWARF already show what they send.
  The DARE desktop shortcut stays for nights the user wants it (`docs/decomp_dare.md`); nothing here depends on it.
- **No outside decomp helps** (searched 6 Nov: no public DARE / UbiSound / Ubisoft PS2 decomp; the only Jade
  source is the leak, which we can't use). Our own sources are better:
  1. `game/analysis/iop/ubisound.irx` (= the disc's `DARE_RT.IRX`), **unstripped**: the driver we replace, with
     its function names.
  2. POP1.ELF DWARF: DARE's EE structs, names, file split (`game/decomp/dare/dare.h`, `protos.tsv`).
  3. The prototype disc's `DARE_DG.IRX` (debug driver; not opened yet, may have asserts/logging).
  4. **PCSX2's SPU2 core, built into our game as the reference sound backend** (user, 6 Nov; not the full
     PCSX2 program). Our own SPU2 model in the recomp is glitchy and unreliable: never compare against it.
     With PCSX2's SPU2 core (precedent: fukami, `docs/reference.md` "Sound") fed by the same run, the reference
     and the native mixer hear the same commands at the same moment, so the outputs line up sample for sample.
     **Gap:** the SPU2 core is only the bottom half. The IOP driver above it still runs in our IOP emulation (timers,
     RPC scheduling, AutoDMA; patches 16, 17, 19), which may be part of why the current sound is glitchy. So
     where the reference sounds wrong or the two disagree in a way that points at timing, settle it with the
     full PCSX2 program on the same in-game save (the `.ps2` memory card is shared since patch 49): a tie-breaker
     only, by hand.
     **Licence:** PCSX2 is GPL-3.0. Fine in the personal build; keep it a separate, optional reference module so
     the 0.7 public release can leave it out or ship under the GPL (decide then).

## The boundary (what native sound replaces)

37 retail DARE functions call SIF / IOP RPC / `sceSdRemote` / IOP memory / sound threads directly (scan of the
retail asm, 6 Nov; by callee name, so calls through a pointer would be missed). The groups:

| Path | EE functions (retail address) | What crosses |
|---|---|---|
| IOP commands | `SND_fn_iIOPCommandRPC` 160740, `SND_fn_eInitRPC` 1609c0, `fn_vUbiEELoop` 160940 | UbiSound RPC commands (voices, banks, init); the IOP also calls back into the EE |
| Sample voices | `sndxdwav_ee.c`: `fn_vUpdateVoiceStatusAndTransfer1stStack` 182700, `SND_fn_vSynchroSxdWave` 182600, `SND_fn_ePostInitEngineConfSxdWave` 181b80, `SND_fn_lGetNbVoiceWishedSxdWave` 1825c0 | voice commands batched per frame, voice status back |
| Streamed mix | `RPS2` (`RPS2.cpp`): `UpdateBuffer` 16a840, ctor 169d40, dtor 16a040, `ClearBuffer` 16a7c0, `fn_ulSendToIOP` 16b9c0, `fn_vWaitForLastIOPTransfers` 16ba80, master/input volume 16b8c0/16b940, VSync thread 16a140/16a1c0/16a380 | EE-mixed PCM -> IOP double buffer (2 x 0x1400 at IOP 0xa9dc0) -> core 0 AutoDMA; `sceSdBlockTransStatus` as play position |
| Bank uploads | `CAsyncTransfer` (`CASyncTransfer.cpp`): `StartTransferEE_IOP` 1914c0, `StartTransferIOP_SPU` 1915c0, `TestTransfer*` | sample banks EE -> IOP -> SPU2 RAM |
| Memory | `sndiopmem.c`, `sndspumem.c`, `sndsubblockhw.c` | IOP heap / SPU2 RAM allocation |
| Setup | `SND_fn_eInitSound` 15dc80, `SND_fn_vDesInitSound` 15de00, `fn_vLoadModuleIRX` 1828c0, `SND_fn_eInitSpuReverb` | module load, reverb preset |

## Steps

1. **Map the commands (offline, no boot).** In `ubisound.irx`, find the RPC dispatch and list every command:
   number, arguments, what it does to SPU2 voices / memory / reverb. Match each to its EE senders (the table
   above). Settle the open question from `docs/decomp_dare.md` "Numbers": does the EE mix all streamed audio
   while the IOP only plays it out, and are sound effects SPU2 voices keyed on by the IOP (14 Oct evidence: 48
   key-ons in the first second, menu clicks and footsteps as voices)? Peek at `DARE_DG.IRX` for strings. Output:
   a command table in this doc.
2. **Survey before designing** (rule): Waffle565/God-Of-War-Native-Port's native sound driven by LIBSD calls
   (its `docs/` sound pages; `docs/reference.md` recomp.fyi entry), OpenGOAL's ISC, GT3-revisited's
   `spu_reverb.cpp` / `voice_mixer.cpp` (MIT; the 10 libsd reverb presets, Gaussian interpolation, ADSR),
   GT4Recomp pacing. Note what to take.
   **Added by the 6 Nov survey (`docs/ps2_projects.md` "6 Nov survey"); read these first:** TheBozos/PS2Recomp
   branch `native/gs-backend` (GPL-3.0, same licence as our base): `ps2xIOP/src/native_audio/native_libsd.cpp`
   (555 lines) + `native_mixer.cpp/.h` (786 + 193 lines), libsd taken over at its API (`PS2X_NATIVE_AUDIO=1`):
   48 voices, PS2 ADPCM, ADSR, pitch, 4-tap interpolation, libsd reverb presets 0-9 at 24 kHz in sound memory,
   `sceSdBlockTrans` PCM input with half callbacks, MMIX routing, movie PCM pushed straight in; no noise or pitch
   modulation. Used by Valkyrie Profile 2 (thebozos/vp2s_recomp: no GS/VU1/SPU2 by default, IOP modules recompiled
   to C++; README has no licence: read only). This is a **third cut point** for step 3: keep `ubisound.irx` (DARE's
   IOP driver) recompiled and swap only libsd under it, so the driver's own logic (voice allocation, RPC) needs no
   rewrite. Chosen as the route (step 3). Copies of
   the three files: `game/analysis/recompfyi/2026-11-06/`. Also: plaught-armor/berserk-recomp (own HLE of a game's
   IOP sound module, Odin; effect mix not applied), bigdaddyjrich/hctp-recomp `tools/audio_check.py` (stereo
   offset / click analysis, idea for step 7's glitch detectors).
3. **Cut point: chosen 6 Nov (user): route A, recompiled IOP driver on native plumbing.** "No emulation" means
   the same for the IOP as for the EE: the game's code runs, recompiled; nothing under it is simulated.
   - **Recompile** the sound modules (`UbiSound_Driver`, `sdr_driver`; the others too if cheap) from R3000 to
     C++ ahead of time, like the EE (precedents: VP2S's ten IOP modules, Waffle565 GoW's 8, black-recomp). No
     R3000 interpreter on the sound path.
   - **libsd** is replaced at its API by the native mixer (start from TheBozos `native_libsd.cpp` /
     `native_mixer.cpp`, GPL-3.0; see step 2). No SPU2 registers, DMA or chip timing.
   - **The plumbing, rewritten native** (user: this is where today's bugs live, patches 16/17/19): the IOP kernel
     services the modules import (threads, semaphores, event flags, alarms/timers, memory, interrupts) as host
     code on host threads, timers on the PC clock instead of counted IOP cycles; SIF RPC as a direct queue
     between the EE caller and the driver's server thread (no simulated SIF DMA); `sceSdBlockTrans` play
     position from the mixer's real output position. First job (offline): list every import of those modules
     and how the driver's update thread is woken (history: "an IOP hardware timer"; if it programs timer
     registers directly, that gets a native stand-in too).
   - **Accuracy:** the driver's behaviour is the game's own code, so the open questions are only the mixer
     (checked against PCSX2's SPU2 core, steps 5-7) and timing (the native plumbing must deliver wake-ups and
     buffer refills at the console's rate, checked by the same WAV comparison).
   - Fallback if the plumbing can't be made right: route B, the EE-side cut (override each UbiSound RPC command
     and the RPS2 / `sceSdRemote` calls; driver logic rewritten from `ubisound.irx`).
   Step 1 still matters: the command table tells us what each refill and wake-up should look like when checking.

   **First job done (6 Nov, offline): what the sound modules import and how the driver is woken.**
   Tool: `tools/irx_imports.py <SLUS_207.43 | .irx>` (splits the 7 IRXes embedded in the ELF, names imports from
   ps2sdk's `exports.tab` files kept in `game/analysis/iop/ps2sdk_tabs/`, counts call sites). Decompiled driver
   (headless Ghidra, unstripped, 114 functions): `game/analysis/iop/ubisound_decomp.c`.
   - **What's already native today:** the runtime's `ps2xIOP` implements every IOP kernel import (threads,
     semaphores, event flags, timers, SIF RPC) as host code (HLE, `iop_emulator.cpp` dispatch). Only the R3000
     CPU is interpreted, and **everything is timed in counted IOP cycles** (`timrman`, `vblank` and SIF DMA
     callbacks take `totalCycles`). That's the root of the timing bugs: IOP time advances only while the IOP
     interpreter runs (patches 16/17 hand it time after RPC calls).
   - **How UbiSound is woken:** `fn_vCreateTimer` (0x19e4) takes hardware timer `AllocHardTimer(1, 0x20, 1)`
     with compare `USec2SysClock(5555 µs)`: an interrupt **every 5.555 ms (180 Hz)**. The handler
     `fn_vTimerFunc` only does `iSignalSema(gs_ThreadSema)`; `fn_iUpdateThread` then runs
     `SPUDMAManager_Update` (bank uploads, polling `sceSdVoiceTransStatus`), `fn_vUpdateBuffersState` and
     `fn_vStartTransfer` (IOP-side ADPCM stream refills into SPU2 memory). Native stand-in: a host timer at
     5.555 ms of real time that signals the semaphore. No timer registers are touched directly.
   - **Other threads:** `fn_iUbiIOPLoop` (RPC server, id 0x12345678, `SND_fn_pvRPC_IOPServerFunc`),
     `fn_iStackThread` (runs the EE's batched command stack; woken by `WakeupThread`, polls with
     `DelayThread(10)`), plus `DelayThread(42 µs)` busy-waits in voice handling and `DelayThread(1000)` in
     `SPUDMAManager`. Delays must be real time, not IOP cycles.
   - **Imports per module** (call sites in brackets where >1):

     | Module | libsd | kernel |
     |---|---|---|
     | `UbiSound_Driver` (33.5 KB) | Init, SetParam (25), SetSwitch (9), GetSwitch (6), SetAddr (3), GetAddr (5), SetCoreAttr (3), VoiceTrans, VoiceTransStatus (2), SetEffectAttr, ClearEffectWorkArea | thbase (Create/Delete/Start/Terminate/Sleep/WakeupThread, DelayThread ×7, USec2SysClock), thsemap (6 calls incl. `iSignalSema`), timrman (the 180 Hz timer), sysmem (Alloc/Free/QueryMaxFree), sifcmd (RPC server), intrman (EnableIntr ×3, CpuSuspend/Resume), FlushDcache |
     | `sdrdrv` (8 KB; the EE's `sceSdRemote` server) | all of 4-27 + ordinals 30-33 (newer libsd entries not in ps2sdk's table; the fork's native libsd stops at 27 too: find what they are) | thbase incl. `iWakeupThread` ×3 from interrupt callbacks, sifcmd server **and client** (`sceSifBindRpc`/`CallRpc`: calls back to the EE, e.g. transfer-done) |
     | `libsd` (28.6 KB) | (the library itself: replaced whole by the native mixer) | intrman (SPU2/DMA interrupts), sifman, thevent |

   - **IOP recompiler exists:** TheBozos/PS2Recomp `tools/iop_recomp.py` (525 lines, GPL-3.0) recompiles an IRX
     to C++ from the image the IOP loaded (`PS2X_IOP_DUMP_MODULES`); functions from the entry, export tables, JAL
     targets and code pointers. Copy in `game/analysis/recompfyi/2026-11-06/`. Our base has no IOP recompiler.
   - **Next:** (a) port `iop_recomp.py` + the module dump to our runtime and recompile `UbiSound_Driver` and
     `sdrdrv`; (b) move the IOP kernel HLE's timers, delays and wake-ups from `totalCycles` to the host clock
     (threads stay cooperative; the 180 Hz timer and `DelayThread` become real-time waits); (c) native libsd
     from the fork's `native_libsd.cpp`/`native_mixer.cpp` + the four unknown ordinals. (a) and (b) can be done
     and checked against today's interpreted IOP before the mixer exists.

   **(a) done offline (6 Nov), not yet built or run on the PC.**
   - Runtime: pop-sotc **patch 51** (`51-iop-native-modules.patch`): the fork's `iop_native.h` (trimmed), registry,
     `NativeEnv`/`runNative` in `iop_emulator.cpp`, `PS2X_IOP_DUMP_MODULES`; the runner builds
     `src/iop_generated/*.cpp`. A module runs natively only if its file hash **and load base** match the
     generated code, and only with `PS2X_IOP_NATIVE=1` (off by default). The generated code keeps the
     interpreter's clock (one tick per instruction), so behaviour and timing match the interpreter; (b) changes
     the clock, not this.
   - Bases (from the 16-17 Oct PC logs, identical in 3 runs): sio2man 0x11a00, padman 0x23a00, mcman 0x45d00,
     mcserv 0x6cf00, libsd 0x77700, **sdrdrv 0x7f100, UbiSound 0x88f00**. If the PC log ever shows
     `[IOP] native: ... recompiled at base X but loaded at Y`, redo the dump with the new bases.
   - Redo (Mac): build ps2xIOP from `~/tools/PS2Recomp-sotc` (`cmake -S ps2xIOP -B <b> -G Ninja`; not the stale
     `game/recomp-sotc/runtime-src` copy, which lacks most patches), build `tools/iop/iop_dump_modules.cpp`
     (command in its header), then
     `iop_dump_modules game/ps2/disc/SLUS_207.43 game/analysis/iop/dump sdrdrv:0x55a700:0x7f100 ubisound:0x55c6c0:0x88f00`
     and `python3 tools/iop/iop_recomp.py game/analysis/iop/dump/<m>.json --out game/analysis/iop/gen`.
     UbiSound: 67 functions, sdrdrv: 13, none left to the interpreter.
   - Check: `tools/iop/iop_native_check.cpp` (build line in its header) loads libsd + sdrdrv + UbiSound at those
     bases, runs the start routines, RPCs 0 (init, version string "UbiSound_Driver"), 3, 2, 5, 6, 7 and ~3 s of
     IOP time, interpreted and native: **IOP RAM, clock and 7 threads identical (215,690 instructions)**;
     breaking one instruction of the native update thread is caught. Not covered: bank uploads (RPC 8/9) and
     real command stacks (need step 1's command table or a recorded run). **Re-run it after any change to
     `iop_emulator.cpp`, the IOP kernel/imports or the generated code** (exit 0 = identical, 3 = different), and
     rebuild the check's object files against the new `libps2_iop.a` first. Save states run the IOP interpreted
     (patch 51 clears native entries on state load).
   - PC (when available): copy `game/analysis/iop/gen/*_iop.cpp` to the PC's `game\analysis\iop\gen`, then
     `rebuild.ps1 -Play -Fast -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc -IopNative game\analysis\iop\gen`
     (after `git am` of patch 51); boot with `PS2X_IOP_NATIVE=1` and look for the two `[IOP] native:` lines.
   **(b) first part done offline (6 Nov): pop-sotc patch 52, `PS2X_IOP_CLOCK=host` (off by default).**
   - Finding: the IOP already schedules by events (threads run, idle time jumps to the next timer/delay/DMA
     wake: `runCycles`); all its timers and delays are in IOP cycles. The trouble was only what drives it:
     `runEeCycles` ran the EE's *estimated* cycles / 8, and the EE's host pacing (`EeScheduler::hostCatchUp`)
     gives up past 8 fields of lag, so in a long slowdown IOP time (180 Hz timer, `DelayThread`, IOP vblank,
     SPU2 output) fell behind real time: the "sound slows with the frame rate" bug.
   - Change: in host mode the EE's cycles only decide when the IOP gets a turn (every 8192 EE cycles, as
     before; inline IOP time is no longer an EE-cycle debt); the target is `anchor + wall seconds x 36.864 MHz`.
     Ahead of the PC (inline RPC servers, patch 17's background cycles) it waits up to a frame, then re-anchors
     (review 6 Nov: 5000 fast polls put it 0.28 s ahead and it stalled that long); after a PC stall at most
     0.1 s is replayed. One change point, so every
     IOP-cycle timer and delay becomes real time, interpreted or native. Works with the emulated SPU2 too, so
     it can be judged in game before the native mixer exists.
   - Check: `iop_native_check <elf> --clock` (1 s wall with EE cycles fed at a third of the rate -> 1.000 s IOP;
     a 0.5 s stall replays 0.108 s; after 5000 inline RPCs the IOP keeps real time, 0.647 s in 0.654 s). The
     default-mode differential check is unchanged (identical).
   - **In-game check needed (user, TV on):** a fight or a known slow spot with `PS2X_IOP_CLOCK=host`, A/B against
     the default: music/effects pitch and speed steady when the frame rate drops; no new stutter or desync
     (the Prince-Farah dialogue sync bug in step 8 is a candidate to change either way).
   - Known limits: the IOP still only runs when the EE thread accounts cycles, so while the EE thread sleeps
     (a vsync wait) IOP work comes in bursts afterwards (several 180 Hz updates at once). Fine for the
     emulated SPU2 and the native mixer (c), which both render by IOP time and hand 512-frame blocks to the
     host's audio queue; if bursts are ever heard, the IOP needs its own host thread. EE code that polls IOP results in tight RPC loops still gets
     patch 17's 2048 background cycles per call, so during load polling the IOP runs faster than real time
     (the emulated SPU2 renders that time early); it no longer stalls afterwards.
   - Left for (b): nothing else found in the kernel HLE that counts time outside `totalCycles` (timrman, vblank,
     DelayThread/`delayCurrentUntil`, SIF DMA and CDVD completions all use it). Revisit only if the in-game check
     shows a timing problem.

   **(c) done offline (6 Nov): pop-sotc patch 53, `PS2X_NATIVE_AUDIO=1` (off by default): native libsd + mixer.**
   - The fork's `native_libsd.cpp` / `native_mixer.*` (GPL-3.0), adapted: libsd is an HLE import library
     (`imports/iop_libsd.*`) dispatched before the import resolves to the loaded libsd module; the mixer
     (`devices/iop_native_mixer.*`) renders by IOP time where `spu2.advanceTo` ran, so with
     `PS2X_IOP_CLOCK=host` it is real time; interpolation is the SPU2's Gaussian table (fork: Catmull-Rom).
     Transfer-done / block-half interrupts are guest calls 2000 cycles later through their own queue (not in
     states; `ScheduledGuestCallback` is saved raw, so growing it would break every saved state).
   - What PoP uses: UbiSound 11 libsd calls (Init, Set/GetParam, Set/GetSwitch, Set/GetAddr, SetCoreAttr,
     VoiceTrans(+Status), SetEffectAttr, ClearEffectWorkArea). sdrdrv (the EE's `sceSdRemote`: DARE's stream on
     core 0 is BlockTrans through it) all of 4-27 and 30-33 = sdrdrv commands 0x8180/0x8190/0x81a0/0x81b0. The
     EE's code holds only 0x8100 (BlockTransStatus) of those; 0x81a0/0x81b0 sit in the client's table at EE file
     0x12cd4, so 30-33 are probably never sent. Ordinals not taken (13/14 note<->pitch math, 30-33) run libsd's
     own code and are logged once; PMON/NON (not played by the mixer) are logged once when set.
   - Check: `iop_native_check <elf> --sound <dir>` drives sdrdrv RPC as the EE does (sceSdInit, VoiceTrans of
     a looping 40-block ADPCM sample using all 5 filters, voice params/ADSR/SSA, KON, KOFF, then a looping PCM
     BlockTrans on core 0) on both paths and writes `spu2.wav` / `native.wav`: voice and stream correlation
     1.00000, level 0.9999 / 1.0001, native voice 1 sample later (21 us; the real PS2 decides which is right).
     A 12 % quieter voice in the native run fails it. WAVs: `game/analysis/sound/libsd_ab/`.
   - Not covered offline: reverb (effect modes, the SPU2 path's reverb vs the mixer's), exponential ADSR,
     EndX readback timing, the real banks and command stacks. Next: add a reverb section to `--sound`, then the
     in-game check.
   - **PC steps (boot: ask first, TV on):** `git am` patches 51-53 on `C:\tools\PS2Recomp-sotc` (and check out
     HEAD in `game\recomp-sotc\runtime-src`), copy `game/analysis/iop/gen/*_iop.cpp`, `rebuild.ps1 -Play -Fast
     -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc -IopNative game\analysis\iop\gen`, then play with
     `PS2X_IOP_NATIVE=1 PS2X_IOP_CLOCK=host PS2X_NATIVE_AUDIO=1` (A/B: drop `PS2X_NATIVE_AUDIO`): menus, music,
     voice-over, a fight, water, a cutscene; look for `[audio] native libsd:` lines and the 10 s mixer stats.

   - Correct decompiles (Ghidra with `-processor MIPS:LE:32:default`; the default picked MIPS64 R6 and broke
     jump tables): `game/analysis/iop/ubisound_decomp.c`, `sdrdrv_decomp.c`. UbiSound RPC functions: 0 init
     (version check, SFX init, stack thread), 1 shutdown, 2 voices wished, 3 post-init config {reverb on,
     biggest reverb, IOP stream voices}, 4 post-shutdown, 5 free IOP memory, 6 voice status (+ optional stack),
     7 command stack, 8 start bank upload {_, src, dst, size; byte 0 = mode}, 9 test upload.
4. **Sound in save-state boots** (save-state boots have no sound today): needed to work from saves offline.
   Check what a load skips (module load, bank upload, RPC bind) and replay it.
5. **Swap PCSX2's SPU2 core into the runtime, then record (boots: ask first, TV on).** Replace our SPU2 model
   behind the same register/DMA interface (SPU2 registers, core DMA, AutoDMA, the IRQ back to the IOP), with a
   switch back to the old model for A/B. Then one run per save logs every command and stream refill with
   timestamps beside the PCSX2-core WAV: menus, music, voice-over, fights, water, sand powers, a cutscene. The
   logs are the offline input for the mixer, the WAVs its reference. First check: do the known bugs (step 8)
   go away with the new core? If not, they are on the IOP/EE side.
6. **Native mixer** (portable, SDL3 audio out): sample banks in our own memory (no SPU2 RAM model), PS2 ADPCM
   decode, ADSR, pitch, volume/pan, the stream input straight from RPS2's buffer, reverb from the libsd presets;
   GT4Recomp-style rate control (never block the game thread). Offline first: drive it from step 5's logs and
   compare with the WAVs.
7. **In game, both at once** (`docs/native_renderer.md` "QA plan for sound"): the same run drives the PCSX2
   core and the native mixer; both write WAVs, compared in ~100 ms blocks (same run, so only a fixed latency to
   align); glitch detectors on our output; every sound alone against vgmstream and the PCSX2 core overnight.
   Disagreements that look like timing: check with the full PCSX2 program on the same in-game save.
8. **Known audio bugs, checked against the PCSX2 core (and the full program if still there):** quiet/missing fight track, Prince-Farah in-engine dialogue out of
   sync, sound slows with the frame rate (`docs/history.md`), and crash 2 (only watched, emulated-sound related?)
   should go away with the IOP.
8b. **Optional: real PS2 as the hardware reference** (user, 6 Nov; reuses the real PS2 + ethernet pipeline of
   `docs/gs_off.md` step 5). Use it to check the PCSX2 core once on a few scenes, to settle disagreements (instead
   of the full PCSX2 program), and for what emulators get roughly right: reverb, interpolation, IOP timing.
   - **Capture** the PS2's optical S/PDIF out with a USB S/PDIF receiver on the PC (48 kHz PCM; that it carries the
     SPU2 mix unaltered is unverified; check the user's model has the port). Not over ethernet: the SPU2 can write
     some taps to its own RAM, but not clearly the final mix with reverb, and reading it back through the IOP
     disturbs the timing. Ethernet (ps2link) is for loading programs and sending logs.
   - **(a) The real game:** the same in-game save from the shared `.ps2` card, recorded. All real hardware, but
     not aligned with our run: compare per scene with a tolerance. Needs only the cable and receiver.
   - **(b) Log replay:** a ps2sdk homebrew loads the game's own `DARE_RT.IRX` on the real IOP and replays step 5's
     logged bank uploads, commands and stream refills timed by vsync, standing in for the EE side the driver
     expects. Gives hardware output for exactly the input the native mixer and the PCSX2 core get. Real work:
     only if the PCSX2 core turns out wrong somewhere that matters.
9. **Close:** native sound on by default in `play.ps1` (the PCSX2-core path kept as the reference switch), user
   listening check on the TV (never over Moonlight).

## Open questions

- Is the streamed layer the only thing the EE mixes, and what else does the IOP do on its own timers (fades,
  voice status)?
- How does sound timing follow the frame rate today (the "slows with the frame rate" bug), and should the native
  mixer run on its own clock?
- How self-contained is PCSX2's SPU2 core (what it needs from PCSX2's IOP, DMA and timing code), and which
  version to take? Read fukami's integration first.
- Which PCSX2 SPU2 settings are closest to a real PS2 (interpolation mode, reverb)?
- How much of today's glitchiness is our IOP emulation rather than our SPU2 model (step 5's first check)?
