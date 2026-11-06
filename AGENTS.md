# ReSands decomp: instructions for AI agents (and their humans)

You are helping rebuild part of *Prince of Persia: The Sands of Time* (PS2, US, SLUS-20743) as C++ source that
compiles to **exactly** the original machine code, one function at a time. We start with the game's sound engine,
**DARE** (Ubisoft's sound engine; 669 functions); the rest of the game comes later. A function counts as done when `tools/decomp/match.py` prints
`MATCH` for it.

This file is for outside contributors. `CLAUDE.md` holds the maintainer's private working notes (their PC, their
schedule, their rules about booting the game): ignore it. `docs/decomp_dare.md` explains how the matching was set up
and what the work taught us so far.

## 1. What your human needs (once)

1. **Their own copy of the game.** Copy `SLUS_207.43` from their own US PS2 disc to `game/ps2/disc/SLUS_207.43`.
   Check it: `shasum SLUS_207.43` (or `certutil -hashfile SLUS_207.43 SHA1` on Windows) must give
   `a5bc95777d458e1dc36a9bd3d3cb7d3523d06c7d`. PAL, Japanese and other discs will not match. `game/` is
   git-ignored: nothing in it is ever committed.
2. **The compiler: Metrowerks CodeWarrior for PS2, build 3.0.3-020716 (also called 3.0b22-020716).** It is in the
   archive that decomp.me uses (`github.com/decompme/compilers`; the PS2 builds named `mwcps2-...`). Put its files
   (`mwccps2.exe` and the files that come with it) in `game/decomp/compilers/3.0.3-020716/`. No other build matches.
3. **Windows, or Wine on macOS/Linux** (`mwccps2.exe` is a 32-bit Windows program; `match.py` calls it through
   `wine` when not on Windows), and **Python 3** (standard library only).
4. **Check the setup:** `python3 tools/decomp/match.py --all` compiles every function already matched and must end
   with `N of N match`. If it does not, stop and fix the setup before writing any code.

The prototype is not needed: everything taken from it (names, struct layouts, the source-file split) is already in
`decomp/dare/`.

## 2. Files

- `decomp/dare/dare.h`: the shared header (every class, struct, enum and declaration DARE uses). Generated from the
  developer's debug information in a 2003 prototype by `tools/decomp/dare_header.py`. Do not edit it (see 4).
- `decomp/dare/dare_fix.h`: small hand additions, included at the end of `dare.h`.
- `decomp/dare/units.tsv`: every DARE function, tab-separated, no header line. Columns: original source file,
  mangled name, size in the prototype, **retail address** (hex, `-` if not in this game), **retail size** in bytes,
  1 if both sizes agree.
- `decomp/dare/callees.tsv`, `globals.tsv`: names for call targets and global data addresses (from the prototype's
  symbols; a name with one vote is wrong about 8% of the time).
- `decomp/dare/<addr>.cpp`: one matched function each, named by its 8-digit retail address (e.g. `0015a640.cpp`).

## 3. Work loop

1. **Pick a function.** A row of `units.tsv` with a retail address and no `decomp/dare/<addr>.cpp` is open. Start
   small: retail size under ~200 bytes, and a 1 in the last column (the prototype has the same version of it).
2. **Claim it.** Look at the open pull requests: if one already names the address, pick another. Otherwise open a
   **draft pull request** titled `dare <addr> <name>` right away, so others skip it. A draft with no update for
   14 days may be taken over.
3. **Look at the target:** `python3 tools/decomp/match.py --asm <addr>` prints the retail disassembly, with call
   targets named where known. Branch delay slots run before the branch takes effect.
4. **Write `decomp/dare/<addr>.cpp`.** It starts with `#include "dare.h"` and defines only this one function. You
   may add `extern` declarations for functions, and for globals that `dare.h` lacks. Never define a type yourself.
5. **Check:** `python3 tools/decomp/match.py decomp/dare/<addr>.cpp`. It prints `MATCH`, or a side-by-side diff
   (`retail | yours`, rows marked `>>` differ). Call targets and global addresses only need the right symbol name.
   Repeat 4-5 until `MATCH`.
6. **Submit:** commit only `decomp/dare/<addr>.cpp`, mark the pull request ready. In its description: the function,
   and which model and tools helped. Run `match.py --all` first if you touched `dare_fix.h`.

Stuck? Close your draft with a comment saying how close you got (e.g. `3 of 40 instructions differ`) and what
you tried, with your best `.cpp` in the comment (never the disassembly).

## 4. Rules

- **Never commit, attach or paste game material:** the ELF, its bytes, disassembly (`--asm` output, diff output),
  disc files or builds of the game. This covers pull requests, issues, comments and public sites such as
  decomp.me scratches. Your own `.cpp` source, and a diff's first line, are fine.
- **Clean sources only.** Work from the retail machine code, `dare.h`, and public documentation. Never use leaked
  Ubisoft source code (Jade or anything else), leaked Sony SDK source, headers or samples, or code written from
  memory of either. A pull request found to contain such code is removed.
- **Real C++ that a person could have written:** no inline assembly, no raw instruction bytes, no hard-coded
  addresses, no mangled names, `@` or `$` symbols as identifiers. Use members by name, never `*(int *)(p + 0x10)`.
- **Never edit `dare.h`.** If a function cannot be written without a header change (a missing member, type,
  declaration, or a method called without `this` that is not declared `static`), say what is needed in your pull
  request or an issue titled `header: <what>`. The maintainer changes the generator, then checks every matched
  function still matches. Small additions (an inline helper, a declaration) can go in `dare_fix.h` with your
  function, if `match.py --all` still passes.
- One function per pull request. Do not change other people's matched files without a reason in the description.
- Every pull request is checked again by the maintainer with `match.py --all` before it is merged (no CI: the game
  cannot be on GitHub).

## 5. What this compiler taught us (read before your first function)

- **Switch vs if:** two or more tests of the same value against different constants up front, then the default
  path, then the case bodies = a `switch`. The tests come out in **reverse** source order, the bodies in source
  order: write the cases in the order of the bodies. A single test is an `if`.
- **`||`:** two tests that lead to the same return, with a branch over the second return or the second test
  inverted = one condition: `if (a == 0 || a == -2) return;`. Separate `if`s compile differently.
- **Signedness:** `slt`/`slti` compare signed, `sltu`/`sltiu` unsigned. A mismatch there is a type difference.
- **Floats:** compare with `f() == -2.0f`, no `(float)` cast (the cast swaps `$f0`/`$f1`). SND "real" numbers are
  16.16 fixed point (`lui v0,0xffff` = -1.0).
- **Order:** stores come out in source order; locals sit on the stack in declaration order, first at the lowest
  address. A value loaded before a call that does not need it means it was read into a local before the call.
- **Shifts and casts:** plain int shifts give `sll`/`sra`; casts to `short` add `dsll32`/`dsra32` pairs. A field
  copied with shifts by 31 and masks of -2 and 1 is a 1-bit bitfield: write a plain assignment.
- **Booleans:** `sltu v0,zero,v0; xori v0,v0,1` with no `andi v0,v0,0xff` after = `(x != 0) ^ 1`. `!x` and
  `x == 0` add the `andi`.
- **Arrays:** a loop over a global array with index times element size = `g_arr[i].member`; a pointer bumped each
  iteration goes in the `for` header (`i++, p++`).
- **Enums:** assigning a number needs a cast: `m_eState = (_SND_tdeState)0;`.
- **C++ machinery:** the constructor's vtable store is generated (write only the member assignments). A destructor
  gets a hidden `short` argument (`a1`, delete when > 0) and the `this != 0` test from the compiler. `operator new`
  then the constructor if non-zero = `p = new Class;`. `__dt__(p, 1)` = `delete p;`. Virtual calls: Metrowerks
  vtables start their entries at +8.
- **Locking:** a call with (file string, line number) between two others = `EnterCriticalSectionTool_();
  MouchardThreadTool(__FILE__, <line>); ... QuitCriticalSectionTool();`.
- **`mfc0 rX,Count`** = call `SND_fn_ulGetCount()` (in `dare_fix.h`); the compiler rejects inline `mfc0`.
- **Offsets:** an offset `dare.h` does not list is usually inside a nested struct or union: member offset + field
  offset.
- The compiler's odd `nop` pairs after calls come out by themselves: do not try to produce them.
