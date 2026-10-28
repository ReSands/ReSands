# DARE byte-matching decomp (roadmap 2.0, DARE first for 0.3)

Started the night of 27-28 Oct 2026. Goal: set up Qwen to byte-match the DARE sound engine's EE code, starting
with what 0.3 (native sound) needs. Byte matching was chosen over a readable decomp (user, 27 Oct). Background and
the compiler check: `docs/roadmap.md` "2.0".

## State (28 Oct, late morning)

- **Done:** DWARF reader, header generator, disassembler and match judge (commit cb17d50). The generated
  `dare.h` compiles cleanly with every size and offset check passing. **First DARE function byte-matched on the
  first try against it:** `GetPos__17OneShotSAMPLEPathFv` (0x167e40, 40 bytes).
- **Ready for a night (28 Oct):** runner, callee names from POP1, queue, night job `tools/windows/oct28.ps1` and the
  PC desktop shortcut **"Qwen DARE audio decomp"** (cancel: "Stop Qwen"). Not run yet beyond the `GetPos` smoke test;
  first do a short trial (about 10 functions: `oct28.ps1 -Until <in ~1 h> -NoShutdown`) to see the hit rate.
- **Hand matches (C function, `switch` shape, constructor with vptr store): all three match (below).** Their assembly and Ghidra C are in `game/decomp/dare/asm/` and
  `ghidra_c/`.
- **28 Oct: `SND_fn_bIsResourceStreamedSnd` (0x17dc40, 76 bytes) MATCHES** compiled as C++ against `dare.h`
  (`game/decomp/dare/snd_b.cpp`): a `switch (pstBRDisk->eType)` with `TYPE_SAMPLE` -> `SND_fn_bIsResourceStreamedSxd`,
  `TYPE_MULTITRACK` -> `..MultiTrack`, `default: return 0`. The `if` / `else if` version of the same logic does not
  match (it inverts the branches): **two `beq`s up front, then the default, then the case bodies = a `switch`**
  (a hint for the Qwen prompt). The callees are unnamed in retail (0x173140 / 0x1731c0); the mapping comes from the
  enum, and the judge masks call targets. Compiling a `.c` source as C++ was fine here (no language switch needed yet).
- **28 Oct: `__ct__5CToolFv` (0x161380, 32 bytes) MATCHES** (`game/decomp/dare/ct_c.cpp`): body assignments
  `m_lNbChildren = 0; m_eState = C_PLAYING-equivalent 0; m_pRes = 0;`. **CodeWarrior keeps the source order of the
  stores**: forward order and an initialiser list both swap the +4 / +0xc stores. The vptr goes in first by itself, so
  the header's vtable symbol (`__vt__5CTool`) and the member offsets are right.
- **28 Oct: `SND_fn_vSndRealTwinToSndReal` (0x1612c0, 40 bytes) MATCHES** on the first try
  (`game/decomp/dare/rt_a.cpp`): `if (pr1) *pr1 = (rtwin >> 16) << 8; if (pr2) *pr2 = (rtwin << 16) >> 8;`. A
  `(short)` cast version adds `dsll32` / `dsra32` pairs: plain `int` shifts are what DARE wrote. **The hand matches
  are done; next is step 2 (the Qwen runner).**

## Tools (all in `tools/decomp/`)

- `dwarf1.py`: DWARF version 1 reader (`dies(elf)` -> DIE tree).
- `dare_callees.py <POP1.ELF> <SLUS_207.43> <units.tsv> <callees.tsv>`: names retail call targets: the k-th `jal`
  of a retail function calls what the k-th `jal` of POP1's copy calls (only when both have the same count; 322
  functions differ, probably POP1's extra asserts), votes across all functions. 406 targets named, 14 with
  disagreeing votes (the prompt uses a name only when it outvotes all others): **704 of 863 unnamed retail calls
  named.** Confirmed 0x173140 = `SND_fn_bIsResourceStreamedSxd`. Self-check: 0x163100 = `BranchPath::GetPos`.
- `dare_units.py <POP1.ELF> <retail_names_from_proto.tsv> <out.tsv>`: every DARE function in POP1, joined by
  mangled name with retail (columns: file, mangled, POP1 size, retail addr, retail size, same size).
- `dare_header.py <POP1.ELF> <dare.h> [protos.tsv]`: the header. `protos.tsv` = per function: file, declaration,
  parameters, locals with their original names (for the prompts).
- `mipsdis.py`: small R5900 disassembler (integer, COP1, scalar MMI); its self-check runs as `python3 mipsdis.py`.
- `match.py <src> <mangled> <addr hex> <size>`: compiles with CodeWarrior 3.0.3 and prints MATCH or a
  per-instruction retail | yours diff (relocated fields masked; the relocation symbol is shown). Exit 0 = match.
  Runs on the PC (`python tools\decomp\match.py game\decomp\try\t1.cpp ...`, with `dare.h` / `dare_fix.h` beside
  the source).
- Regenerate the working files on the Mac (`game/decomp/dare/`, git-ignored):
  `python3 tools/decomp/dare_units.py game/proto/disc/POP1/POP1.ELF game/proto/analysis/retail_names_from_proto.tsv game/decomp/dare/units.tsv`,
  `python3 tools/decomp/dare_header.py game/proto/disc/POP1/POP1.ELF game/decomp/dare/dare.h game/decomp/dare/protos.tsv`
  (create an empty `dare_fix.h` beside it). Retail assembly and Ghidra C for the 669 functions: `analyzeHeadless`
  with `PoPDisasm.java` and `PoPDecompileTo.java` on `game/decomp/dare/addrs.txt` (outputs `asm/`, `ghidra_c/`).
  **Ghidra headless needs `JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home`** (as in
  `tools/rebuild.sh`), or it fails with "Unable to prompt user for JDK path".
- `qwen_match.py <mangled | addr hex> [--rounds 8] [--url ...]` (PC): the Qwen loop for one function. Prompt =
  retail asm, Ghidra C, declaration, parameter / local names from `protos.tsv`, the dare.h excerpt the function needs
  (its class, bases, member types, enums and C declarations it names; capped at 30 KB) and the compiler lessons
  from the hand matches. Feedback = `match.py`'s diff. Results: `game/analysis/qwen/dare_<addr>/` (`match.log`,
  `attempts/`, `best.cpp` = most matching instructions, `PASS`, or `HEADER` when Qwen reports a header change is
  needed). `--prompt` prints the prompt (works on the Mac); `--try <file>` judges a file like a round. Needs
  `game/decomp/dare/` on the PC (copied 28 Oct; recopy after regenerating: `tar czf - -C game/decomp dare | ssh ...`).
- `game/decomp/dare/pccc.sh <file> [flags]`: copies `dare.h` + the file to the PC and compiles it (error check only).

## Numbers

- POP1.ELF's DWARF puts **1,250 functions in DARE source files** (112 source files overall; DARE file list with
  sizes from `dare_units.py`). **669 of them are in retail SoT by name**, and **311 of those are the same size**
  (the most likely to match with POP1's layouts). About 159 KB of retail code.
- Header: 286 records, 51 enums, 705 methods, 331 C functions, 84 globals, 27 vtables.
- **Relevant to 0.3 (unverified, from the file list only):** the EE side has the whole DARE "ToolArch" mixing chain
  (`Renderers/PS2/RPS2.cpp`, `Tools/Adder`, `Pitch/PS2`, `VolPan/PS2`, `IMA-adpcm`, `SndManager`, the path
  classes). If the EE really mixes and the IOP only outputs, native sound could replace just the renderer end
  (RPS2) and keep the recompiled chain. Check this before designing 0.3.

## What the DWARF and compiler taught us

- **Compiler flags: `-O4,p -enum min`.** DWARF gives many enums a byte size of 1; `-enum min` reproduces that
  (without it every struct with a small enum lays out wrong). The four 26 Oct test functions have no enums.
- CodeWarrior makes **one compile unit per function**, with that function's types copied into it. Types are
  deduplicated by name (the most common layout wins; only `sndCNode` has two: 16 or 12 bytes).
- **Attribute 0x200 = the mangled name** (MW extension). There are **no typedef DIEs**: members refer straight to
  the struct. **Anonymous types are all named `@class` / `@anon0`**, so they get a name from a hash of their layout.
- **Unions are tagged as classes:** every member at offset 0 means a union.
- **Enum `element_list`:** value (the enum's own byte size) then the name. The same enumerator name appears in two
  enums from different files; the later ones get an `<enum>__` prefix.
- **`long` is 8 bytes** (fund types 0xa-0xc), as in ee-gcc.
- **Some methods have no formal parameters in DWARF.** Parameter types come from the mangled name (small cfront-style
  demangler in `dare_header.py`), names and return type from DWARF. Nested types (`Q2...`) use their last name (the
  DWARF has them at top level).
- **vptr placement:** CodeWarrior puts the vptr where the first virtual is declared. A root class whose first member
  is at 4 has the vptr at 0, so its virtuals are declared before the data members.
- **Vtables:** `__vt__<n><Class>` symbols in POP1, entries from +8. Unknown slots in a root class (CTool slots 0-3)
  are named after a derived class's override, so derived overrides keep their slots.
- A member typed `void` with 12 bytes of room is a **pointer to member function** (MW = 12 bytes): fixed by hand in
  `MEMBER_TYPES` (`TAdder::m_pGetDataSampleProc`).
- **`-S` / `-dis` need `mwldps2`**, which only the 2.4 build has, and that wants a FlexLM licence. Hence our own
  `mipsdis.py`.

## Next steps (in order)

1. ~~Finish the three hand matches~~ done 28 Oct (all three matched; no per-file C switch needed).
2. ~~Qwen runner~~ built 28 Oct: `tools/decomp/qwen_match.py` (see Tools). Callee names: `dare_callees.py` (see
   Tools). Known gap: free C++ `extern` declarations are pulled in by short name next to methods of the same name
   (e.g. `extern int GetPos(int)` beside `BranchPath::GetPos`).
3. ~~Smoke test~~ done 28 Oct: Qwen matched `GetPos` in round 5 of 8 (about 6 min in all; rounds 1-4 each had one
   instruction wrong, the `0xffff0000` return). **Run it with output to a file** (`... > game\jobs\<x>.log 2>&1`,
   or under `start-job.ps1`): the first run, with stdout over plain ssh, died between judging a MATCH and writing
   PASS (no traceback reached the Mac), and both ssh sessions hung after Python had exited. A restart now finds such
   a MATCH and writes PASS.
4. ~~Queue~~ built 28 Oct: `qwen_match.py --queue` = functions without PASS / HEADER, untried first, then the 311
   same-size ones, smallest first, then the other 358. `--summary` = counts plus the 15 nearest misses.
- **First run, 28 Oct 15:37-17:44 (`oct28.ps1`, 4 rounds, stopped by hand): 17 of 29 functions matched (59%)**, plus
  `GetPos` from the smoke test = 18 of 669. All were the smallest same-size functions (32-44 bytes); ~4.2 min per
  function on average, many PASS in round 1-2. 12 missed: best attempts 0-6 of 8-12 instructions; 2 never compiled
  (`SND_fn_rGetLengthMultiTrack`, `SndManager::AddCallBack`). Not looked into yet: why the misses fail (read their
  `attempts\roundNN.report.txt` on the PC). Resume: start "Qwen DARE audio decomp" again (it continues the queue).
- **Why the 12 misses missed (28 Oct evening; attempts copied to `game/analysis/qwen_pc/`). Fixes, in order of
  value; 1 done, the rest NOT done yet:**
  1. *Globals have no names.* DARE reaches every global as `lui at,hi` + `lw lo(at)` (851 accesses in 213 functions,
     zero `gp`). **Done:** `dare_callees.py` now also writes `game/decomp/dare/globals.tsv` (retail data address ->
     POP1 symbol [+offset], size; 237 named, e.g. 0x680c80 `gs_bStackInitialised`, 0x680c98 `g_pStackHead`,
     0x6a42b0 `g_pThemeManager`). To do: annotate the asm (`; name`) and Ghidra's `DAT_` names in the prompt, and
     tell Qwen to declare globals that are not in dare.h as `extern <type> <name>;` (most are file statics).
  2. *Compiler flag missing: `-sdatathreshold 0`.* Without it an `extern` global compiles to `lw 0(gp)`; with it
     to retail's `lui at` / `lw (at)` (tested on the PC 28 Oct). Add to `match.py` FLAGS, then re-judge the 18
     passes (`qwen_match.py <addr> --try game\analysis\qwen\dare_<addr>\best.cpp`) to make sure none breaks.
  3. *No member offsets in the header excerpt* (`header_blocks` keeps the struct but drops its DARE_CHECK lines):
     Qwen picks the wrong member (`LoadBinaryResourceSilence`) or casts raw offsets (`TTransition::RemoveChild`).
     Fix: comment each member with its offset from the `DARE_OFS(...) == N` checks.
  4. *Float constants:* `GetLatency` returns `(int)3276.8f` (`lui 0x454c; ori 0xcccd; mtc1`); Qwen wrote 1000.
     Fix: annotate lui/ori pairs that feed mtc1 with the float value.
  5. *The switch hint misleads:* `TVirtualSample::Resume` (one test) came out as a switch. Reword: "two or more
     tests of the same value against different constants".
  6. *Header problems Qwen cannot see:* `RPS2::GetResLength` is called with no `this` (a0 = the union by value),
     so it is probably static (DWARF lost it); `sndCList` got redefined by Qwen (tell it never to redefine a
     dare.h type); `fn_lSendSoundRequestOnChannelWithFadeIn` (0x15d940, 7 args incl. a float) is not in dare.h or
     the callee map, and retail tail-calls it (`j`).
5. ~~Night job~~ built 28 Oct: `tools/windows/oct28.ps1` (from `oct26.ps1`: llama-server checked before every
   function, one pass of `-Rounds 4` per function, summary into `game\jobs\qwen-night.log`, then shutdown; `-Until`,
   `-NoShutdown`, `-Shortcut` remakes the desktop shortcut). "Stop Qwen" now also ends `qwen_match.py`. A pass over
   all 668 at 4 rounds is several nights (estimate: 1-4 min per round); it resumes where it stopped.
6. Later: a whole-unit build and link check, splitting the rest of the ELF, decomp-permuter for near-misses.
