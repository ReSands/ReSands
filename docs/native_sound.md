# 0.3 native sound: plan of attack

Written 6 Nov 2026 (morning), when 0.3 started. Goal (roadmap 0.3): sound comes from the engine's DARE/UbiSound
calls through our own mixer, with no IOP or SPU2 emulation. Older notes this plan builds on:
`docs/native_renderer.md` "QA plan for sound" and the "Sound:" step list near "Native world in the game window";
GT4Recomp pacing in `docs/reference.md`; the 14 Oct stream bug history in `docs/history.md`.

## Decisions (6 Nov)

- **Not waiting on the DARE byte-matching decomp.** 0.3 keeps DARE's EE code exactly as it runs recompiled
  today and replaces what sits under it (the IOP driver and SPU2). A byte match would give cleaner source for the
  EE functions that send commands, but no new knowledge: Ghidra + POP1.ELF's DWARF already show what they send.
  The DARE desktop shortcut stays for nights the user wants it (`docs/decomp_dare.md`); nothing here depends on it.
- **No outside decomp helps** (searched 6 Nov: no public DARE / UbiSound / Ubisoft PS2 decomp; the only Jade
  source is the leak, which we can't use). Our own sources are better:
  1. `game/analysis/iop/ubisound.irx` (= the disc's `DARE_RT.IRX`), **unstripped**: the driver we replace, with
     its function names.
  2. POP1.ELF DWARF: DARE's EE structs, names, file split (`game/decomp/dare/dare.h`, `protos.tsv`).
  3. The prototype disc's `DARE_DG.IRX` (debug driver; not opened yet, may have asserts/logging).
  4. The running recomp, whose emulated sound is right in most places: the reference to record against (with the
     known exceptions below; our SPU2 emulation is not ground truth, `independent-reference` rule).

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
3. **Choose the cut point** (after 1-2): most likely the EE side, by overriding the RPC call per command and the
   RPS2 SIF DMA / `sceSdRemote` calls, so the IOP and SPU2 can be switched off; or at the IOP RPC server, which
   keeps the IOP alive (not the 0.3 goal). Write the choice here.
4. **Sound in save-state boots** (save-state boots have no sound today): needed to work from saves offline.
   Check what a load skips (module load, bank upload, RPC bind) and replay it.
5. **Reference recordings (needs a boot: ask first, TV on).** One run logs every command (and stream refill) with
   timestamps beside a WAV of the emulated output (`PS2X_AUDIO_DUMP`), on saves covering menus, music, voice-over,
   fights, water, sand powers, a cutscene. Becomes the offline test input for the mixer.
6. **Native mixer** (portable, SDL3 audio out): sample banks in our own memory (no SPU2 RAM model), PS2 ADPCM
   decode, ADSR, pitch, volume/pan, the stream input straight from RPS2's buffer, reverb from the libsd presets;
   GT4Recomp-style rate control (never block the game thread). Offline first: drive it from step 5's logs and
   compare with the WAVs.
7. **In game, both at once**, then compare (`docs/native_renderer.md` "QA plan for sound"): dual render vs the
   emulation in ~100 ms blocks, glitch detectors, every sound alone against vgmstream overnight.
8. **Known audio bugs, checked on both paths:** quiet/missing fight track, Prince-Farah in-engine dialogue out of
   sync, sound slows with the frame rate (`docs/history.md`), and crash 2 (only watched, emulated-sound related?)
   should go away with the IOP.
9. **Close:** native sound on by default in `play.ps1` (switch back to the emulation kept as the reference), user
   listening check on the TV (never over Moonlight).

## Open questions

- Is the streamed layer the only thing the EE mixes, and what else does the IOP do on its own timers (fades,
  voice status)?
- How does sound timing follow the frame rate today (the "slows with the frame rate" bug), and should the native
  mixer run on its own clock?
- Reverb: does our current SPU2 emulation do it at all (unchecked), so is the reference silent on reverb?
