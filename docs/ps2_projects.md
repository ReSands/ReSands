# PS2 recompilation and port projects (reference directory)

Every PS2 recompilation / native-port project we have looked at, for the "survey before fixing" rule
(CLAUDE.md). Generated 18 Oct 2026 from the saved surveys (`game/analysis/unbaked/`, `game/analysis/recompfyi/`;
READMEs in their `readmes/` folders, file name = `owner__repo.md`), plus projects noted by hand. Findings per
project: `docs/reference.md` ("Survey of other PS2 projects", "CPU side / VU0 microcode"). Repo = github.com/<owner>/<repo>.
Licences: check before reusing any code; reading is always fine.

Flags (unbaked table): R=PS2Recomp O=own recompiler S=SotC fork G=PCSX2 GS L=paraLLEl-GS V=microVU A=VU1 AOT N=native renderer I=IPU M=movies 2=SPU2 U=upscale W=widescreen F=60+fps T=texture packs D=DLSS/FSR P=playable B=boot-stage

## Noted by hand

| Project | What | Notes |
|---|---|---|
| ran-j/PS2Recomp | the recompiler/runtime we build on (GPL-3.0) | wiki read 18 Oct: getting-started only |
| TaylorNAlbarnaz/PS2Recomp `sotc-port` (LightVelox) | our runtime base (pop-sotc) | GPU GS, async VIF/GS, MTVU, VU1 AOT |
| DQ8 fork (PS2Recomp PR #254) | VU1 compiler with SSE2, LLE IOP/SPU2 | runtime not MSVC-buildable |
| ps2-web-recomp | MIT IPU (old movie plan, superseded) | |
| FulGerNet/rePOP (reSOT) | **Xbox** Sands of Time recomp (xboxrecomp) | same engine; Xbox addresses, Hor+ hooks; MIT + LGPL parts |
| TekRantGaming/king-kong-recompiled | Xbox 360 King Kong (Jade engine) | ReXGlue; not reusable |
| theclub654/ProjectCane | Sly Cooper 1 native PC port | **no licence: read only** |
| lombyte-project/Lombyte | Ratchet & Clank matching decomp | not useful (matching-decomp route) |
| SirBraga/black-recomp | Black | EE, VU0, VU1, IOP all recompiled (VU0 lead, 18 Oct) |
| sp00nznet/reo | Resident Evil Outbreak | own VU0/VU1 microcode translator (18 Oct) |
| TaylorNAlbarnaz/PS2Recomp `sotc-port` 17 Oct | our base, newer commits | VU1 zero-padded prefix matching, hot paths: our patches 32-34 (18 Oct) |
| KIexster/god-of-war-recomp | God of War (SotC base) | `docs/RENDIMIENTO.md`: getenv in hot loops 38%, IOP scheduler 41% (18 Oct) |
| Vnnc1/gow-native-pc (GPL-3.0) | God of War; Tobiichi-Port runtime (= Waffle565 + KIexster) + 16 patches, launcher | created 25 Oct; clang/PGO builds, VU1 clean operands, batched SPU2 mixer, MOVZ/MOVN fix (26 Oct; `docs/reference.md` "gow-native-pc") |
| catsandsoup/Road-trip-Adventure-PS2-Recompile-MacOS | Road Trip | VU1 AOT with build-time pipeline scheduling, ~60x (18 Oct) |
| Harold0777/eagle_five_recomp | Ace Combat 5 perf fork | VU1 100%, GS state cache, VBlank condvars (18 Oct) |
| kris-c-anderson/godhand-rearmed (GPL-3.0; fork of LasVola/godhand-recomp) | God Hand, PCSX2 GS, 60 fps, any aspect | same runtime family as ours via thiagordsfx/urban-reign-recomp; callback-stack leak fix, hang dump, switchable instruction patches, HUD anchoring, counter finder (23 Oct; `docs/reference.md` "God Hand") |
| LucasPicoli/god-hand-decomp (MIT) | God Hand matching decomp | names/boundaries for the recomp only; not our route (23 Oct) |
| thiagordsfx/urban-reign-recomp | Urban Reign; root of the God Hand runtime | not read yet: runtime, VU1 compiler, sound, PCSX2 GS bridge, hang dump |
| moesuito/gt4-staticrecomp (GT4Recomp, MIT) | Gran Turismo 4, release 1.2.3, ps2-web-recomp base | pacing + audio rate control, crash-safe memory card, SDL3 pad, ultrawide, FPU oracle (23 Oct; `docs/reference.md` "GT4Recomp") |

## From the surveys

| Game / title | Repo (owner/repo) | Flags | Source |
|---|---|---|---|
| .hack//Infection | dothacktranslate/dothack-recompilation | RMUW | unbaked.games (13 Oct) |
| Ace Combat 5: The Unsung War | sal063/Ace-Combat-5-Static-recompilation | ROANIM2P | unbaked.games (13 Oct) |
| Ace Combat 5: The Unsung War | Harold0777/eagle_five_recomp | RAU | unbaked.games (13 Oct) |
| (ATV-Off-Road-Fury-Quadzilla-Engine-recomp) | lewilou22/ATV-Off-Road-Fury-Quadzilla-Engine-recomp | RGMB | unbaked.games (13 Oct) |
| Avatar: The Last Airbender | Glitxhhh/YipYip-Recomp | R | unbaked.games (13 Oct) |
| Berserk: Millennium Falcon — Recompilation | fenrircl/berserk-ps2-recomp |  | unbaked.games (13 Oct) |
| (Black-2006-Recomp) | portingpete/Black-2006-Recomp |  | unbaked.games (13 Oct) |
| (black-recomp) | SirBraga/black-recomp | ROLAMWF | unbaked.games (13 Oct) |
| black-recomp | elpetese/black-macos-recomp |  | unbaked.games (13 Oct) |
| BT3-Recomp — Dragon Ball Z: Budokai Tenkaichi 3 on PC | z3xox/BT3-Recomp |  | unbaked.games (13 Oct) |
| Burnout 2: Point of Impact — Static Recompilation | timftw21/burnout2-recompilation |  | unbaked.games (13 Oct) |
| Burnout 3: Takedown | danielgomesvieira2000/burnout3-pc | OMUWFT | unbaked.games (13 Oct) |
| Burnout 3: Takedown | matheuz232/Burnout-3-Recompiled | F | unbaked.games (13 Oct) |
| Burnout 3: Takedown — static recompilation | sp00nznet/burnout3 |  | unbaked.games (13 Oct) |
| Burnout Revenge Recompiled | fabioap-cpu/Burnout-Revenge-Recomp |  | unbaked.games (13 Oct) |
| Burnout Revenge ReXGlue Recompilation | xXJSONDeruloXx/burnout-recomp |  | unbaked.games (13 Oct) |
| Burnout Revenge — Static Recompilation / PC Port Research | Luizfe181/Burnout-Revenge-X360-Recomp |  | unbaked.games (13 Oct) |
| (Burnout-Revenge-Remaster) | crazybushman3-source/Burnout-Revenge-Remaster |  | unbaked.games (13 Oct) |
| (Burnout3Recomp) | mxmstr/Burnout3Recomp |  | unbaked.games (13 Oct) |
| (BurnoutDominatorReversed) | literalynn/BurnoutDominatorReversed | RI2B | unbaked.games (13 Oct) |
| Cars — Xbox 360 ReXGlue Recompilation | ant0-blase/Cars-X360-ReXGlue |  | unbaked.games (13 Oct) |
| Champions of Norrath | 0xjjjjjj/halogen | RGL | unbaked.games (13 Oct) |
| Crash Twinsanity | menaman123/Ps2Recomp | RB | unbaked.games (13 Oct) |
| Dark Cloud 2 | Red-tv141/DC2-PS2RECOMP | R | unbaked.games (13 Oct) |
| (DBZ-Budokai-3-HD-Collection) | novapowers0/DBZ-Budokai-3-HD-Collection |  | unbaked.games (13 Oct) |
| Def Jam: Fight for NY — recompilation statique | comoria08/defjam_recomp |  | unbaked.games (13 Oct) |
| Def Jam: Fight for NY — Recompiled | vyanhursky/DefJamFFNY-recomp |  | unbaked.games (13 Oct) |
| Devil May Cry | Gui-Tora/dmc-recomp | RGIMB | unbaked.games (13 Oct) |
| Downhill Domination | lngadesupport/Recomp-Domination | R | unbaked.games (13 Oct) |
| Dragon Quest VIII: Journey of the | Sinan-Karakaya/DQ8-Recompiled | R | unbaked.games (13 Oct) |
| Dynasty Warriors 3 | gzrt3/DW3RE-AI-WORKSPACE | RB | unbaked.games (13 Oct) |
| Enthusia Professional Racing | Lilicris12/EnthusiaRecomp | RIM2B | unbaked.games (13 Oct) |
| (ghpc) | noahbaxter/ghpc |  | recomp.fyi (13 Oct) |
| God of War | YYOzcan/Tobiichi-Port | RSAM2UWFD | unbaked.games (13 Oct) |
| God of War II HD — native macOS port by static recompilation | andrebrumdev/gow2-recomp |  | unbaked.games (13 Oct) |
| God of War — Native Port | Waffle565/God-Of-War-Native-Port |  | recomp.fyi (13 Oct) |
| (god-of-war-recomp) | KIexster/god-of-war-recomp | RSGAM | unbaked.games (13 Oct) |
| Gran Turismo 4 | Kaezzey/gt4-staticrecomp (now moesuito/gt4-staticrecomp) | own (ps2-web-recomp) GAWTFP | unbaked.games (13 Oct); re-read 23 Oct, see the hand-noted row |
| Guitar Hero II | noahbaxter/GHClassic | RM2UF | unbaked.games (13 Oct) |
| Guitar Hero II Recompiled | sp00nznet/gh2 |  | unbaked.games (13 Oct) |
| Guitar Hero II – Recompiled Files | KrispyKerabs/GuitarHero2recomp |  | unbaked.games (13 Oct) |
| Haunting Ground | KunoDemetries/hg2 | RM2B | unbaked.games (13 Oct) |
| (ico-recomp) | nathanialf/ico-recomp | LAN | unbaked.games (13 Oct) |
| Kessen II | creativeprofit22/kessen2 | R | unbaked.games (13 Oct) |
| (KillzoneRecomp) | drakolordx7/KillzoneRecomp | RGVI2UWF | unbaked.games (13 Oct) |
| King's Field: The Ancient City | Voicedrew11/holyland | RM2B | unbaked.games (13 Oct) |
| Madden NFL 08 | codewriter3000/PS2Recomp-madden08 | R | unbaked.games (13 Oct) |
| (MANXBurnout3) | CrownParkComputing/MANXBurnout3 |  | unbaked.games (13 Oct) |
| (mc3r) | cloudalister/mc3r |  | unbaked.games (13 Oct) |
| Medal of Honor: Frontline — GameCube Static Recompilation | ant0-blase/MOHFrontline-GC-RECOMP |  | unbaked.games (13 Oct) |
| Metal Gear Solid 3: Subsistence | outlawproductionstwo-hash/Metal-Gear-Solid-3---Subsistence-Recpmp |  | unbaked.games (13 Oct) |
| Midnight Club 3: DUB Edition | AngelSalazar-dev/MC3-Port | R | unbaked.games (13 Oct) |
| Mortal Kombat: Shaolin Monks | OGmidway/mksm-recomp | RM | unbaked.games (13 Oct) |
| Mortal Kombat: Shaolin Monks (Xbox) - Port Nativo para Ninte | gerisson1996/mksm-switch |  | unbaked.games (13 Oct) |
| (Nightfire-Windows-Port) | limeylicious/Nightfire-Windows-Port |  | unbaked.games (13 Oct) |
| (OpenTricky) | GiZcesi/OpenTricky |  | unbaked.games (13 Oct) |
| (pc-extermination) | vs-sr-dev/pc-extermination |  | recomp.fyi (13 Oct) |
| (Project-Shaolin) | BeiBiyayo/Project-Shaolin |  | unbaked.games (13 Oct) |
| (ps2recomp_eternal_ring_ps2) | kokolika/ps2recomp_eternal_ring_ps2 | R | unbaked.games (13 Oct) |
| Ratchet & Clank | Nikityyy/OpenRatchet | RNM | unbaked.games (13 Oct) |
| rb2 - Dragon Ball Z Budokai HD Recompiled | WistfulHopes/DBZ1 |  | unbaked.games (13 Oct) |
| re-gh2 - Guitar Hero II Recompiled | YoshiCrystal9/re-gh2 |  | unbaked.games (13 Oct) |
| Resident Evil Outbreak | sp00nznet/reo | RGLM2WFB | unbaked.games (13 Oct) |
| (ResidentEvilOutbreakRecompiled) | RaccoonRecomp/ResidentEvilOutbreakRecompiled | M2UWF | unbaked.games (13 Oct) |
| Ridge Racer V | danisandoval/fukami | RG2UWF | unbaked.games (13 Oct) |
| Road Trip Adventure: a native PS2 static recompilation | catsandsoup/Road-trip-Adventure-PS2-Recompile-MacOS | RLVAM2UWF | unbaked.games (13 Oct) |
| (RoadTripAdventure-recomp) | silentsudin/RoadTripAdventure-recomp | RLAUWFTDB | unbaked.games (13 Oct) |
| (RumbleRacingPC) | Ehren1337/RumbleRacingPC | RGM | unbaked.games (13 Oct) |
| (sh2-nx) | cutarev/sh2-nx |  | unbaked.games (13 Oct) |
| (sh3-nx) | cutarev/sh3-nx |  | unbaked.games (13 Oct) |
| Shadow of the Colossus | LightVelox/sotc-vibe-pc | RSAM2WFB | unbaked.games (13 Oct) |
| Shadow the Hedgehog (PS2) - PC Recompilation | nicolasserpa/shadow-the-hedgehog-recomp |  | recomp.fyi (13 Oct) |
| Shin Megami Tensei: Persona 4 | tojikoumori/P4toPSP | R | unbaked.games (13 Oct) |
| SOCOM II: U.S. Navy SEALs | Scotho/socom-unzipped | RGAMB | unbaked.games (13 Oct) |
| SOCOM: U.S. Navy SEALs | BLVCKBURN/socom-native | R2 | unbaked.games (13 Oct) |
| Spider-Man 2 — Xbox Recomp | SirSymo/SM2-Recomp |  | unbaked.games (13 Oct) |
| (Spider-Man-2-PS2-Recomp) | saltyboosack-blip/Spider-Man-2-PS2-Recomp | RGMB | unbaked.games (13 Oct) |
| (split2-recomp) | phobos665/split2-recomp |  | unbaked.games (13 Oct) |
| SSX Tricky: PC Port | MatiasRiveraC/SSX-Tricky-PC |  | unbaked.games (13 Oct) |
| (ssx-tricky-recomp) | reindance/ssx-tricky-recomp |  | unbaked.games (13 Oct) |
| Star Wars: Bounty Hunter | uran39281-bit/bounty-hunter- | R2 | unbaked.games (13 Oct) |
| STOR PES6 Web | danilostorm/pes6 |  | unbaked.games (13 Oct) |
| Super Robot Taisen Z | SloppyWyn/SRWZ-native | RGVIM2B | unbaked.games (13 Oct) |
| (Sword-Of-Etheria-PC-Recomp) | AncletoCEO/Sword-Of-Etheria-PC-Recomp | RF | unbaked.games (13 Oct) |
| (tales-of-legendia-ps2recomp) | vamidi/tales-of-legendia-ps2recomp | R | unbaked.games (13 Oct) |
| Test Drive: Eve of Destruction | pareidolico/eod-recomp | IM2UWPB | unbaked.games (13 Oct) |
| TimeSplitters 2, recompiled | phobos665/Split-Again-Recomp |  | unbaked.games (13 Oct) |
| Tom & Jerry: War of the Whiskers — native PC port | shihabal3amri/tom-jerry-war-of-the-whiskers-pc |  | unbaked.games (13 Oct) |
| Ultraman Fighting Evolution 3 | idiazraz/ufe3-recomp | RMB | unbaked.games (13 Oct) |
| (war-of-the-monsters-pc) | scruffy10K/war-of-the-monsters-pc | RMWF | unbaked.games (13 Oct) |
| (War_of_the_Monsters_recomp) | yanm1103/War_of_the_Monsters_recomp | R | unbaked.games (13 Oct) |
| Way of the Samurai | InitialDad/PS2Recomp-WoS-Findings | LPB | unbaked.games (13 Oct) |
| (Xbox360-Native-Ports) | CrownParkComputing/Xbox360-Native-Ports |  | unbaked.games (13 Oct) |
| (Xerenge) | shipa-2/Xerenge |  | unbaked.games (13 Oct) |
