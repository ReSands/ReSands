# DARE byte-matching decomp (roadmap 2.0, DARE first for 0.3)

Started the night of 27-28 Oct 2026. Goal: set up Qwen to byte-match the DARE sound engine's EE code, starting
with what 0.3 (native sound) needs. Byte matching was chosen over a readable decomp (user, 27 Oct). Background and
the compiler check: `docs/roadmap.md` "2.0".

## State when stopped (28 Oct morning)

- **Done:** DWARF reader, header generator, disassembler and match judge (commit cb17d50). The generated
  `dare.h` compiles cleanly with every size and offset check passing. **First DARE function byte-matched on the
  first try against it:** `GetPos__17OneShotSAMPLEPathFv` (0x167e40, 40 bytes).
- **Not done:** the Qwen runner, the queue, the night job and the PC desktop shortcut. **There is no Qwen shortcut
  yet.** The planned name is "Qwen DARE audio decomp"; only put it on the desktop after a smoke run passes end to end.
- **Was in progress:** hand-matching a C function and a constructor before trusting the header more widely:
  `SND_fn_vSndRealTwinToSndReal` (0x1612c0, 40 bytes, sndreal.c, still to do). Their assembly and Ghidra C are in `game/decomp/dare/asm/` and
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

## Tools (all in `tools/decomp/`)

- `dwarf1.py`: DWARF version 1 reader (`dies(elf)` -> DIE tree).
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

1. Finish the three hand matches above (C function, `if` / `else` call shape, constructor with vptr store). If the C
   function only matches when compiled as C, `match.py` needs a per-file language switch (sources ending in `.c`).
2. Qwen runner (`tools/decomp/qwen_match.py` or under `tools/llm_port/`, reusing `ask()` from
   `tools/llm_port/meshdecode/port.py`, with a fresh seed per request). Prompt: the retail assembly (`mipsdis` text,
   callee names from `units.tsv` / Ghidra), Ghidra's C, the function's declaration, parameter and local names from
   `protos.tsv`, the definitions of its class and of the types it touches (trimmed from `dare.h`; the whole header
   is ~265 KB, too big for a prompt). Qwen writes one `.cpp` that includes `dare.h`; it may add extern declarations
   but must not edit the header (it reports "needs header change"). Judge = `match.py`; feedback = its diff.
   Results per function under `game/analysis/qwen/dare_<addr>/` (`PASS` file + `best.cpp`).
3. Test the runner end to end with GetPos (known good) before any night.
4. Queue: the 311 same-size functions first, smallest first, then the other 358.
5. Night job `tools/windows/<date>.ps1` copied from `oct26.ps1` (llama-server checks, passes until two bring nothing
   new, then shut down; "Stop Qwen" cancels), plus the desktop shortcut **"Qwen DARE audio decomp"**.
6. Later: a whole-unit build and link check, splitting the rest of the ELF, decomp-permuter for near-misses.
