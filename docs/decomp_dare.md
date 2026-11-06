# DARE byte-matching decomp (roadmap 2.0, DARE first for 0.3)

Started the night of 27-28 Oct 2026. Goal: set up Qwen to byte-match the DARE sound engine's EE code, starting
with what 0.3 (native sound) needs. Byte matching was chosen over a readable decomp (user, 27 Oct). Background and
the compiler check: `docs/roadmap.md` "2.0".

## State (6 Nov; start here)

- **Paused for 0.3 (user, 6 Nov):** 0.3 native sound goes ahead without the byte match (`docs/native_sound.md`).
  The "Qwen DARE audio decomp" shortcut stays and resumes the queue whenever the user starts it.
- **Run 13 (5 Nov 22:07 - 6 Nov 07:22, stopped by me at the user's request): 248 of 669** (+9). Qwen 6 of 47
  (173340 176c80 173880 174b00 1952c0 173800; 176c80 likely from the permuter's file, which matched it 7 minutes
  earlier), ~12 min per function; the permuter lane 4 of 61 passes (191680 176c80 18f440 15c400; every match
  started within 9 instructions). One header request (182bc0, 2-argument `SND_fn_pvMallocSnd`: ruled out 4 Nov,
  reopen). Nearest misses: 17c180 `SND_fn_hASyncLoadResBinarySnd` and `CVirtualPlayer::DelVirtualVoice` 1 off,
  `SND_fn_vStopSxdWave` 2 off. Folders not yet copied to the Mac.
- **Outlook (6 Nov, estimate):** Qwen has matched nothing over 104 instructions (0 of 9 tried above 120) and 152 of
  its 183 matches came in rounds 1-3. Local work likely tops out near 300-320 of 669; the 145 functions of 80+
  instructions (21.6k of the 33k unmatched instructions; `TAdder`, `OneShotMULTITRACKPath`, `CVirtualPlayer`,
  `TMultiTrack`, `CThemeChannel`, `RPS2`, ...) need Opus drafts, ideally with Qwen + permuter closing the last
  instructions overnight.

## State (5 Nov evening)

- **239 of 669 matched** (5 Nov evening: run 12's 212 + 24 hand matches + 1 by the permuter + 2 that matched
  as they were once the header changed). Details: "5 Nov: fixes, hand pass, permuter" below. All 29 header
  requests are answered (reopened), 136 functions are marked RETRY, 78 are untried (107-810 instructions).
- **Night job from 5 Nov = two lanes** (same "Qwen DARE audio decomp" shortcut, `oct28.ps1`): Qwen on the GPU
  (queue: RETRY first, closest miss first; then the big untried ones) and `permute.py --lane` on the CPU
  (below-normal priority, 6 workers, 10 minutes per function; misses within 10 instructions, smallest gap first;
  Qwen leaves those to it until it has had them). Logs: `game\jobs\qwen-night.log`, `permute-night.log`.
- **Next:** the morning check below plus `permute-night.log` (lines `perm01: N differ`, `PASS by the permuter`);
  after any header change run `qwen_match.py --rejudge` (every PASS against the new header). Open: 186dc0
  `TImaAdpcm::Reset` stores words at +0x64/+0x68/+0x6c where POP1's `_ima_adpcm_header` has a byte and two shorts
  (retail layout differs or struct copies; unchecked). Before 2.0: the judge masks call targets, so a MATCH does not
  check the callee's name (194640 matched while calling `FUN_00195c80`; fixed by hand).
- **Morning check (when the user asks for progress):** on the PC, `python -I tools\decomp\qwen_match.py --summary`
  (counts + 15 nearest misses) and `game\jobs\qwen-night.log` (one line per function: `[i/N] <addr> PASS in roundNN`
  or `no match after 4 more rounds`); for run 6 also the two checks at the end of "Prepared for run 6". For a miss, read `game\analysis\qwen\dare_<addr>\attempts\roundNN.report.txt`
  (retail | yours, `>>` = differs) and `best.cpp`; copy the folders to the Mac with
  `ssh 10.0.0.202 "cd /d C:\dev\PoP_Decomp\game\analysis\qwen && tar czf - dare_*" | tar xzf - -C game/analysis/qwen_pc`.
  Resume: the "Qwen DARE audio decomp" desktop shortcut (continues the queue). Shut the PC down when done.
- **Shortcuts on the PC desktop:** "Qwen DARE audio decomp" (`oct28.ps1`) and "Stop Qwen" (`qwen-stop.ps1`, ends a
  DARE or VU1 job and llama-server); both are remade by `oct28.ps1 -Shortcut`.

## 29 Oct: run 2 header requests and hand fixes (historical)

- **29 Oct afternoon (Mac + PC compiler, no game boot): 79 of 669 matched**, none waiting on a header change.
  - **Run 2's 9 header requests checked against the retail assembly:** 3 were Qwen misreadings (172380 and 16f500
    are nested offsets: `uRes` +0x10 then `stMultiTrack.hFirstElementOpt` +0x50, `m_stOutputFormat` +0x10 then
    `uFormat.stPCM.ulFreq` +8; 191240's `short` is CodeWarrior's hidden delete flag). The real gaps, fixed in
    `dare_header.py`: **`sndCList` is a template** (`SNDCLIST`: DWARF keeps only the name; arguments per member /
    global in `SNDCLIST_ARGS`, from the mangled method names; retail's mangled `AddTail__51sndCList<...>` comes out
    exactly); **static methods** (`STATIC_METHODS`: `SndManager::GetLengthSample`, `AddCallBack`,
    `RPS2::GetResLength`; DWARF does not mark them); **`MouchardThreadTool` / `SND_fn_vMouchardThreadsnd` take
    (`__FILE__`, `__LINE__`)** (`C_PARAMS`); **25 function declarations whose POP1 symbol is mangled** (e.g.
    `fn_vUbiEELoop__FPv`) moved out of `extern "C"`; **6 static data members** (`SndManager::gs_CallBackRequestList`,
    `m_pRenderer`, `m_tdeAudioMode*`, `m_tdePositionFormatTOOL`, `RPS2::m_eVSyncMode`) declared in their class
    instead of as C globals (found from `name__<n>Class` symbols; function symbols of that form are left alone:
    they collide with overloads). All 9 then matched by hand (`game/decomp/dare/hm_*.cpp`, `hdr2.cpp`).
  - **Near misses by hand: 6 more** (`game/decomp/dare/nm/`): `RPS2::ASyncLoadResBinary` (calls `LoadResBinary`,
    slot +0x68), `Create_Source`, `SND_fn_vSynchroSxd`, `CThemeManager::SynchroTheme` (next read before the call),
    `DesInitMTTrackVolumeArray`, `SND_fn_vStrncpy` (`dest[dest_size - 1]` three times). Plus
    `SND_fn_lGetSpuBiggestReverbSize`: its round 2 was already a MATCH with no PASS written (a runner gap: check
    other "tried" folders for a MATCH report). `SND_fn_bCanFreeSubBlock` still misses (case test order 1, 3, 0;
    seven variants tried). Recorded on the PC as `PASS` = "hand (29 Oct)" + `best.cpp`.
  - **Prompt lessons added** (`qwen_match.py`): hidden destructor flag, nested offsets, locals in declaration
    order, the Mouchard locking pattern.
  - **Ghidra (Mac project only; the PC has its own copy, so PC builds are unchanged):** 158 functions renamed from
    `callees.tsv` (`tools/decomp/dare_names.py` -> `PoPApplyProtoNames.java <tsv> <comment prefix>`), 180 data
    labels from `globals.tsv` (`PoPApplyLabels.java`). **One-vote callee names are ~8% wrong** (14 of 177 checked
    against the prototype names; none of 61 with two or more votes): applied separately, plate comment
    "POP1 DARE name (one vote; about 8% of these are wrong)". The Qwen prompt still uses one-vote names
    unmarked (`callee_names()`); the wrong ones are listed in `game/ghidra/headless_dare_names_1vote.log`
    ("Differs"). Backup before: `game/ghidra/backup_20261029_pre_dare_names.*`.

- **Run 2 is done (stopped 29 Oct 07:31 with "Stop Qwen"); results under "Runs".** The queue resumes from the
  next untried function when the "Qwen DARE audio decomp" shortcut runs again. All attempt folders are copied to
  `game/analysis/qwen_pc/` on the Mac.
- **Matched so far: 64 of 669** (`--summary`, 29 Oct): 42 in run 2, plus earlier 22 = 18 by Qwen (17 in run 1 + `GetPos` in the smoke test) + 4 by hand
  (`SND_fn_bIsResourceStreamedSnd`, and after the fixes `SND_fn_vResetStack`, `SND_fn_pvGetStackHead`,
  `SND_fn_vDesInitTheme`, all three run-1 misses). The 18 Qwen passes and every hand match were re-judged under
  the new flag on 28 Oct: all still MATCH.
## Runs

- **Smoke test (28 Oct morning):** Qwen matched `GetPos` in round 5 of 8 (~6 min).
- **Run 1 (28 Oct 15:37-17:44, stopped by hand): 17 of 29 matched (59%)**, the smallest same-size functions (32-44
  bytes), ~4.2 min per function, many in round 1-2. 12 misses (best attempts 0-6 of 8-12 instructions; 2 never
  compiled), copied to `game/analysis/qwen_pc/` on the Mac. Why they missed -> fixes 1-6.
- **Run 2 (28 Oct 21:44 - 29 Oct 07:31, stopped by the user): 42 of 103 matched (41%)**, ~5.7 min per function;
  9 stopped with NEEDS HEADER CHANGE, ~52 no match after 4 rounds; 544 of the 647 queued not reached. **Fixes 1-6
  barely helped run 1's misses:** of the 10 retried, 1 matched (170d40), 2 reached a header request (172380,
  18ac80), 7 still miss. Most passes are new functions, many in round 1.
  - Header requests (to apply in `dare_fix.h` / `dare_header.py`): member at 0x60 of `_tdstBlockResourceMem`
    (172380), `stCallBackRequest_` missing (18ac80), member at 0x18 of `CTool` (16f500),
    `SndManager::GetLengthSample` static (171a80), `SND_fn_vMouchardThreadsnd` arguments (183780),
    `~CAsyncTransfer` takes `short param_2` (191240), `sndCList::Find` / `sndPos` (194d00), `MouchardThreadTool`
    is `(int, int)` (1711c0), `fn_vUbiEELoop` is C++ (`fn_vUbiEELoop__FPv`), not `extern "C"` (160940).
  - Nearest misses (for a hand fix or the permuter): `RPS2::ASyncLoadResBinary` 16/17, `Create_Source` 13/14,
    `SND_fn_bCanFreeSubBlock` 20/22, `SND_fn_vSynchroSxd` 19/21, `CThemeManager::SynchroTheme` 16/18,
    `SndManager::DesInitMTTrackVolumeArray` 15/17, `SND_fn_lGetSpuBiggestReverb` 13/15, `SND_fn_vStrncpy` 16/19.

- **Run 3 (30 Oct 21:39-22:26, cut short by a PC shutdown; resumed 31 Oct 08:01-10:26, stopped by the user): 5 of 23
  matched (22%)**, 2 header requests (sndCList members `m_pHead` / `RemoveHead`, `sndCListNode`), 16 misses. Now
  **84 of 669**. The queue has reached 100-112-byte functions (runs 1-2: 32-90). **25 of the misses' 64 rounds did not
  compile (39%)**; 165d80 and 182c80 never compiled. Near misses: 166940 (1 of 27: `slt` vs `sltu`, signedness),
  16d640 (4: float compare operands swapped), 173440 (6), 188c00 (8). Folders copied to `game/analysis/qwen_pc/`.
  Compile errors over all runs, by cause (first error of each failed round): raw linker names used as C++ names
  (`@110` string literals, `clock$localstatic3$GetInstance__15SND_ClockNativeFv`, mangled
  `gs_CallBackRequestList__10SndManager` / `RemoveHead__55sndCList<...>Fv` / `..._Fv`): ~20; member names guessed
  under nested unions (`uRes.stMultiTrack.ulDataSize`, `m_pData`): ~15; sndCList with no members: ~5; `int` into an
  enum without a cast: ~5; static methods not marked (`SndManager::GetRendererRate`): ~5; types of free-function
  declarations missing (`TFader` -> "implicit int"): ~6; "NEEDS HEADER CHANGE" written inside the code block (compiled
  as code, so the request was lost): seen in 182c80.

- **Tweaks after run 3 (31 Oct, 6b5d617; `qwen_match.py --selftest`):** readable names in the prompt (the `@NNN`
  constants as their string literal, `__FILE__` for file names, pointer-to-member or plain compiler constants;
  `x$localstaticN$f` as the static local of `f`; static members and callees as `Class::name`, template arguments
  demangled; offsets kept); the `sndCList` template now reaches the header excerpt (`header_blocks` skipped
  `template <...> struct`); member types two levels deep (the `uRes` unions' structs); a "NEEDS HEADER CHANGE" line
  counts wherever the answer puts it; feedback hints for `slt`/`sltu`, an inverted branch and swapped compare operands;
  2 extra rounds once when the best attempt is within 2 instructions; lessons: enum casts, signed vs unsigned
  compares; `SndManager::GetRendererRate` static (0x189180 never reads `a0`, its callers set none). Prompts grew
  (e.g. 171a80 9.7k -> 28.8k characters; the 30 KB header cap holds). **Test (31 Oct 10:36-10:43):** 166940 (1 of 27
  off for 4 rounds) **matched in its next round**; 182c80 (`SND_fn_vSynchroTimer`, never compiled) compiled for the
  first time, 30 of 33 off: it needs `SND_ClockNative` with an inline `GetInstance()` and a COP0 `Count` read, a
  header job, not a runner one. **85 of 669.**

- **Run 4 (31 Oct 11:44 - 1 Nov 00:29, stopped by the user): 18 of 95 matched (19%)**, 14 NEEDS HEADER CHANGE
  (mostly `ASSOCIATION_DESCRIPTION`, `_tdstMapMem`, fade/switch-event structs), 63 misses; 487 of 582 queued not
  reached. Now **103 of 669** (`--summary`; 16 header requests, 128 tried without a match). Nearest misses:
  `TFader::StartFade` 30/31, `SND_fn_eInitSpuReverb` 38/40, `TMultiTrack::FileIsValid` 42/45. Folders copied to
  `game/analysis/qwen_pc/`.
- **Near misses by hand (1 Nov): 13 of the top 15 matched -> 116 of 669** (`game/decomp/dare/nm/`, list in
  `done.txt`; recorded on the PC as `PASS` = "hand (1 Nov)" + `best.cpp`). What Qwen got wrong, as candidate prompt
  lessons: **switch case order** (CodeWarrior tests the cases in reverse source order: `SND_fn_bCanFreeSubBlock` is
  `case 0: case 3: case 1:`, `Create_TDecoder` ADPCM, PCM, IMA, OGG); **no `(float)` cast** on an int compared with
  a float constant (the cast swaps `$f0`/`$f1`); **a 1-bit bitfield copy** is a plain assignment (`bDynamic`); index an
  array of structs (`g_stSndToc[i].lSize`) instead of byte offsets; pointer increments in the `for` header; the
  pointer argument of `SND_fn_vFreeSnd(void *)`; `delete p` for `__dt__(p, 1)`; string literals from the ELF for
  `lui`/`addiu` string addresses; nested member names (`m_stOutputFormat.uFormat.stPCM.ulFreq`). Left:
  **`__dt__9TImaAdpcmFv`** is a header problem: retail stores a malloc'd buffer at +0x34 and its size at +0x30
  (`AllocBuffer` 0x186a00), where POP1's DWARF has `m_bInitDone` / `short m_iExtraSample[2]`; retail `TImaAdpcm`'s
  layout differs. **`SND_fn_vSetDefaultRollOff`** (7 of 47 off, only the first if/else's register choice): 10
  variants tried, a job for the permuter.
- **Prepared for run 5 (1 Nov):** prompt lessons from the hand matches above; unnamed string addresses shown as
  their text (`__FILE__` for file names), also in Ghidra's C (44 prompts had raw addresses); the float-compare hint
  says to drop `(float)` casts; the header excerpt follows member types 4 levels deep and is seeded with POP1's
  parameter and local types (7 of the 16 header requests named types dare.h has but the excerpt left out; median
  prompt 10.8k -> 15.8k characters). Header (`dare_header.py`): `RETAIL_INSERT` adds retail's extra int at +4 of
  `ASSOCIATION_DESCRIPTION_` (size 144: 4 requests), `EXTRA_METHODS` the 3-argument `SndManager::PlayLoopingVoice` /
  `PlayOneShotVoice` retail calls. The other 5 requests were misreadings (t0 in a delay slot, vtable slot +8 =
  `AddChild`, sndCList members present). All 16 reopened: `HEADER` renamed `HEADER.1101`, and a `RETRY` file puts a
  function first in the queue (removed when the runner picks it up). 115 of 116 passes still match the new header;
  0x189180 (`GetRendererRate`) no longer compiles since the 31 Oct static change (its best.cpp calls it on an object).

- **Run 5 (1 Nov 00:54-10:09, stopped by me for the changes below): 7 of 59 matched (12%)**, 5 NEEDS HEADER CHANGE,
  47 misses; ~9.5 min per function. Now **123 of 669**. **88 of its 217 rounds did not compile (41%)**; 11
  functions never compiled in any run. Folders copied to `game/analysis/qwen_pc/`.
- **After run 5 (1 Nov; ideas partly from fnvdecomp.com's agents.md, the FNV byte-matching project's agent guide):**
  - **`match.py` diffs aligned (difflib on opcode + registers; same = equal with the relocation mask, branches equal
    when their targets pair).** The old diff compared instruction k with k: one extra instruction made the rest of
    the function "differ" (165900: 24 of 35 shown, ~3 real), which also skewed `best.cpp` and the near-miss list.
    Reports from earlier runs keep their positional scores (`--summary` mixes both until re-judged).
  - **`dare.h` left out 214 C++ free functions** (`Create_Source`, `Destroy_*`, `KillAndDestroyAsso`: mangled, no
    class, dropped by the generator), **the classes' own `operator new` / `delete`** (12 / 4), and everything
    without POP1 DWARF: the PS2 OS layer at 0x199xxx (`SND_fn_vFreeSnd` 68 calls, `SND_fn_vDisplayError` 67,
    `SND_fn_pvMallocSnd` 38, file and stream functions: `NO_DWARF` in `dare_header.py`, arguments read from the
    retail bodies) and libc. 110 of 406 callees had no declaration; now only Sony kernel / sif calls (1-2 calls each,
    left to the prompt: no Sony signatures from memory). Also `typedef struct X_ X` for the 17 `X_` tags (Qwen
    writes `ASSOCIATION_DESCRIPTION`), `#define NULL 0`, `SndManager::Renderer` static (0x189cc0 never reads a0).
    Re-judging the compile errors in these functions' folders (runs 3-5) with the new header: only 11 of 117 compile; the rest are Qwen's own member /
    type guesses (`ulSwitchValue` for `hSwitchValue`, placement new, function-pointer casts), not header gaps.
  - **Runner:** resumes from the best attempt (was the last, often a compile error); a round worse than the best
    gets the best back as the base ("start from the best", with the compile error's first words); **matched
    functions of the same source file** go into the prompt (up to 4 KB, smallest first) as style examples; lesson:
    `Class::operator new` + constructor = `new Class`.
  - All 123 passes re-judged: 123 match (0x15d840 and 0x1828c0 lost a hand `extern` that now clashes with dare.h:
    removed, originals as `best.cpp.pre1101` on the PC; 0x189180 back with `Renderer` static).
  - Not done: a permuter for the 1-2-instruction misses (decomp-permuter with `match.py` as the compile step), a
    second compiler build for register-only misses (fnvdecomp's were VC++ 2008 RTM vs SP1), a plan/regions step for
    big functions (the queue is at ~250 bytes).
- **Test run (1 Nov from 10:18): the first 12 non-header functions of run 5 again, fresh** (run 5: 4 of 12 matched),
  `oct28.ps1 -NoShutdown -AddrsFile game\jobs\test1101.txt`, 4 rounds each. Run 5's folders held in `game\analysis\qwen_run5_held\` on
  the PC: put back afterwards (keep a new PASS). **Stopped after 9 of 12 (user: little more to learn).** Same 9 in
  run 5 vs test: **4 passes each** (test won 181ac0, lost 18a480 to the seed), **7 of 29 rounds did not compile in
  both**; best re-judged scores (today's diff and header) **higher or equal in 8 of 9** (165900 26->27, 163100
  26->28, 17ae40 none->25, 15ecc0 none->32 of 43; 18a480 36->34) and passes in fewer rounds (193500 r2 vs r5,
  15e940 r1 vs r3, 15ae00 r2 vs r6, 181ac0 r2): ~6 min per function vs ~9.5. Too few to call the pass rate; a full
  night will. Test folders in `game\analysis\qwen_test1101\` on the PC; run 5's put back; 181ac0's PASS kept:
  **124 of 669**. 164580, 180d80, 17e140 were not reached: normal queue.
- **Prepared for run 6 (1 Nov evening; ideas from memdoor's team-solve workflow, github.com/guregodevo/memdoor
  `examples/workflows/team-solve`: a 27B Qwen in a plan / two ideators / solve / test / review / one-repair loop, 47 vs 35
  of 100 problems over a single call, one run):** (1) **compile fixes outside the round count**: an attempt that does
  not compile gets up to 2 "fix only the compile errors" calls (`roundNN.fix1/.fix2.cpp`), against run 5's 41% of
  rounds that never compiled; (2) **two first drafts on a fresh function**: the usual one plus `roundNN.b` written from the
  assembly's control flow (Ghidra's C only for types and names); the better one is the base for round 2. Not
  taken: separate manager / planner roles (our judge is exact, so a manager has nothing to add) and memdoor itself. Each
  function now costs ~1-3 more model calls, so expect fewer functions per night. **To read in the morning:** how many
  `.fix` files compile (`grep -l "^MATCH\|instructions differ" */attempts/*.fix*.report.txt`), and how often `.b`
  beats round 1 (`match.log`).

- **Run 6 (1 Nov 21:58 - 2 Nov 07:36, stopped by me at the user's request): 18 of 47 matched (38%)**, 2 NEEDS HEADER
  CHANGE, 27 misses; ~12 min per function. Now **142 of 669**. Two halves: the last same-size untried functions
  (376-1336 bytes, #1-20) **2 of 20** (15f0c0, 16d400; 15-35 min each, best misses 5-10 off: 167680, 165180, 166000),
  then the untried size-differs ones from 36 bytes up (#21-47) **16 of 27**, mostly round 1. The two checks:
  - **Compile fixes work:** 64 fix calls, 52 compiled (81%), 4 of them a MATCH (191780 passed through `round01.fix1`).
    Main rounds that did not compile: 56 of 181 (31%; run 5: 41%, before the fix calls).
  - **`.b` draft is a wash on average:** better than round 1 in 13 functions, worse in 12, equal in 19; but on big
    functions it is often far better (167680 45->8, 193140 57->20, 15abc0 128->66, 1596c0 89->61). Kept.
  - Fixed after: `.b` also ran when round 1 had already matched (13 passes, ~1 min each); now skipped.
  Folders copied to `game/analysis/qwen_pc/`.

- **Run 7 (2 Nov 07:45-14:57, stopped by me for the header fix below): 23 of 44 matched (52%)**, 4 NEEDS HEADER
  CHANGE (191040, 167c00, 191540, 1608c0), 17 misses; ~10 min per function. Now **165 of 669**.
  - **Header (2 Nov): retail `_tdstBankElement` is 0x30 bytes, POP1's 0x48**: `fn_pstGetBank` 0x175200 mallocs
    0x30, `strncpy(+0xc, name, 0xb)` (`char czBankName[12]`, POP1 33), `pBankHeader` +0x18 (191040's request),
    `eLoadState` a byte at +0x2c (`sb`/`lbu` in 175340-175ac0). `RETAIL_MEMBER` in `dare_header.py` (type, size,
    shift of later members). All passes re-judged: 165 of 165 match.
  - **Runner fix:** a MATCH that tied the best score (a draft with every retail instruction plus extras) did not
    replace `best.cpp` (192f40 kept round01.b's; round02 put back, old as `best.cpp.pre1102`).
  - Reopened with `RETRY`: 191040 (`HEADER` -> `HEADER.1102`) and the 8 misses whose attempts used the struct
    (16b380 175700 176840 176b40 176bc0 17bfc0 18eb80 18f040). Run 8 = run 7 resumed.

- **Run 8 (2 Nov 15:02-18:29, stopped by me at the user's request): 7 of 21 matched (33%)**, no header requests;
  ~10 min per function. Now **172 of 669** (10 need a header change, 215 tried without a match, 272 untried).
  - **The 9 reopened: 2 passed** (18f040 round 7, 191040 round 5). The `_tdstBankElement` fix helped the rest
    (best instructions differing, rounds 1-4 -> 5-8): 16b380 18->5, 175700 50->25, 17bfc0 67->25, 176840 35->27,
    176b40 6->5; 18eb80 3->4 (no gain). Still open for a hand pass: 16b380 and 176b40 (5 off), 18eb80 (3-4 off).
  - **12 new: 5 passed** (16ee00, 190780, 15a640, 15ea00, 162d40); nearest misses 193480 (4 off), 191680 (5 off);
    17ce80 never compiled.
  Folders copied to `game/analysis/qwen_pc/`. 

- **Run 9 (2 Nov 23:10 - 3 Nov 07:33, stopped by me at the user's request): 14 of 43 matched (33%)**, 5 NEEDS HEADER
  CHANGE, 24 misses; ~10 min per function. Now **186 of 669** (15 need a header change, 240 tried without a match,
  228 untried). Untried size-differs functions, 28-46 instructions.
  - **Passed:** 18ea40 1914c0 193d80 18a9c0 18e880 191440 18fd80 162dc0 193300 193c40 15a840 15b500 190880 190580
    (9 in round 1-2).
  - **Nearest misses (hand pass):** 193a40 1 off (branch sense flipped + one store), 160a80 2 off (calls
    `SND_fn_vEnterCriticalSectionThreadSnd` where retail calls 0x160cc0), 169580 3 off (stores zero where retail stores
    t0/t1); then 18d480, 190800 (6), 16b840, 186b00 (7).
  - **177c80 never compiled:** rounds 3-4 lacked the `sndCList<_tdstASyncBankRequest,...>` destructor in the object
    (template instance not emitted). 2 round-1 calls hit the 32768-token cap with no code block (17ce00, 193a40).
  - **Header requests, not yet checked against the assembly:** 17d780 `_tdstVoice` missing from `dare.h` (only
    `_tdstVoicePosAssoc`/`_tdstVoicesData` are there), 162240 `CTool::Pause`/`Resume` virtual, 182600
    `fn_vUpdateVoiceStatusAndTransfer1stStack` returns a value, 18b780 `_tdstTypeSampleMem` member at +0x24,
    1952c0 `_stPartInfo` nested in `CThemeChannel` (mangled `Q213CThemeChannel11_stPartInfo`).
  Folders copied to `game/analysis/qwen_pc/`. Next: run 10 = the shortcut again (continues the queue).

- **Run 10 (3 Nov 22:04 - 4 Nov 07:43, stopped by me at the user's request): 7 of 40 matched (18%)**, 3 NEEDS HEADER
  CHANGE, 30 misses; ~14 min per function (run 9: ~10). Now **193 of 669** (18 need a header change, 271 tried
  without a match, 187 untried). Untried size-differs functions, 36-45 instructions; every round compiled after the
  fix calls.
  - **Passed:** 1803c0 18d180 1904c0 18e980 190040 190f80 190ec0 (5 of 7 in `CASyncBankLoader`/`UnLoader`). The
    misses are mostly the C API files (`sndplay.c`, `sndtimer_ee.c`, `SndManager`, `sndrpc_ee.c`); the POP1-to-retail
    size ratio does not separate passes from misses (median 1.37 both).
  - **Timer family blocks on `mfc0 $9`:** all 7 retail functions that read COP0 Count (160680, 182bc0-182fc0; the
    timer functions inline a static clock) are unmatched, 6 tried. Qwen writes GCC `__asm__("mfc0 %0, 9")`, which
    mwccps2 rejects ("illegal operand"), and spends its fix calls on it. One hand-checked inline helper in
    `dare_fix.h` (CodeWarrior asm syntax) should unblock the family.
  - **Header requests checked:** 195380 and run 9's 1952c0 are the same real gap (`_stPartInfo` must be nested in
    `CThemeChannel`: mangled `Q213CThemeChannel11_stPartInfo`). 168440 real: retail mangled name is
    `Create_TVirtualSample(TVirtualSample**, _SND_tdstLoopSampleInfo*, unsigned int*, int)`, `dare.h` has POP1's.
    197840 (`SND_fn_pvMallocSnd` 2 arguments) plausible: 4 retail callers set `a1 = 0` before `jal 0x199a40`, 182bc0
    does not; 0x199a40 is outside the DARE units, check it in Ghidra. 197840 was 1 off.
  - **Nearest misses (hand pass):** 168dc0 1 off, 194640 1 extra `b` (the summary shows it as "37 of 37": the score
    counts matching retail instructions and ignores extras), 190940 2 off, 16b9c0 and 15a6c0 3 off.
  - **Far misses:** 191c40 `MixBuffers_32_16_MMI` (MMI, 22 off), 1609c0, 17cbc0, 17cc80, 18fe00, 160240, 17ad80 (14-22 off).
  Folders copied to `game/analysis/qwen_pc/`.

- **4 Nov: fixes after run 10.** Two of the three header requests were Qwen misreadings. I first listed 168440 as
  real, but the "retail" name I cited came from Qwen's own draft, and retail has no symbols. In retail, 168440
  passes (format on the stack, loop info, frequency) exactly as POP1 declares. For 197840, the allocator at 0x199a40
  never reads `a1`, and about 37 callers do not set it. Both were reopened as they are.
  - **`_stPartInfo` nested in `CThemeChannel`** (`NESTED` in `dare_header.py`; the demangler keeps the class for
    it). Prompts now name template classes in C++ (`sndCList<CThemeChannel::_stPartInfo, ...>`), not mangled.
    **195380 `RemoveHead` matched by hand** (`nm/removehead_partinfo.cpp`; lesson: `TYPE returnValue = pOldNode->data;`
    copy-initialised, not assigned).
  - **`SND_fn_ulGetCount()` in `dare_fix.h`** (`__asm__ ("mfc0 %0, $9" : "=r" (c))`, the only form mwccps2 took).
    **182fc0 `SND_fn_vResumeTimer` matched by hand** (`nm/resumetimer.cpp`; file-static `init` + `clock`,
    `initTick += (m_current + (unsigned int)(Count - m_previous)) - pauseTick`). It is now an example in the
    other timer prompts.
  - Reopened (`RETRY`; `HEADER` -> `HEADER.1104`): 1952c0, 168440, 197840, timers 160680 182bc0 182c80 182d00
    182e40 182f00.

- **Run 11 (4 Nov 07:52-18:05, stopped by me at the user's request): 14 of 46 matched (30%)**, 7 NEEDS HEADER
  CHANGE, 25 misses; ~13 min per function. Now **209 of 669** (21 need a header change, 291 tried without a match,
  148 untried).
  - **Passed:** 197840 (reopened: the "missing 2nd argument" was indeed a misreading), 182f00 (timer, helped by the
    182fc0 example), 190cc0 180680 190640 16a2c0 18ff80 183e80 1903c0 190a00 16d900 190dc0 18c200 17b840.
  - **Reopened 9:** 2 passed (197840, 182f00); the rest got much closer: 182e40 15 -> 5 off, 182c80 30 -> 11,
    168440 27 -> 15, 182d00 8 off, 1952c0 19 off. 182bc0 asked again for a 2-argument `SND_fn_pvMallocSnd` (already
    ruled out on 4 Nov: the allocator never reads a1).
  - **160680 `SND_fn_rGetRandomSnd` never compiled (9 of 9 rounds):** it is not a timer, so no timer example reaches
    its prompt, and nothing tells Qwen about `SND_fn_ulGetCount()`; it writes `__asm__("mfc0 %0, 9")` (illegal
    operand) or `__mfc0`, then asks for a `GetCount` declaration. Fix: one rule line in `qwen_match.py` (retail
    `mfc0 ..., Count` -> call `SND_fn_ulGetCount()`, the only form mwccps2 takes), then reopen.
  - **Header requests (unchecked against Ghidra yet):** 18fa00 `ReadBankWaveForms` (`TestReadBankWaveForms` return
    type), 18afc0 `SndManager::SetParamTransition` (wants `CPath::SetParam`; retail makes a virtual call, slot 0x34
    of the object at +0x14), 18e780 `SND_fn_vFreeIOP` (wants `__sndPos` members; the asm walks a list whose nodes
    point to `_tdstMemoryCluster` and tests `pvAddress == p`, so probably a misreading), 158580
    `ChangeVolumeForType` (`_SND_tdstTypeObjectSoundInfo`), 16fd40 `TPitchShifter::AddChild` (`GetOutputFormat`
    2 arguments). Common thread: 18fa00, 16fd40, 18e780 and 18afc0 call unnamed `FUN_` targets (0x18fb00, 0x1615c0,
    0x199640, 0x18c400) that `callees.tsv` does not name; naming them may settle most of these.
  - **Compile errors still high in some:** 193cc0 11 of 13 rounds, 16a040 10 of 13, 18b900 9 of 11 (invented members
    `eFmt`, `lNumberOfElementsOpt`; pointer-to-member conversions). Not a new pattern.
  - **Nearest misses (hand pass):** 18f080 `CASyncBankLoader::OpenBank` 2 off (48 of 49 in the summary),
    1821c0 `SND_fn_vStopSxdWave` 4 off, 182e40 `SND_fn_vResetTimer` 5 off, 15a780 `IsAssoDynamic` 6 off.
  Folders copied to `game/analysis/qwen_pc/`.

- **Run 12 (4 Nov 21:49 - 5 Nov 18:54, stopped by me at the user's request): 3 of 70 matched (4%)**, 9 NEEDS
  HEADER CHANGE, 58 misses; ~15 min per function. Now **212 of 669** (29 need a header change, 350 tried without a
  match, 78 untried). The queue had moved on to untried functions of 59-104 instructions (median 80; run 11's were
  mostly under 50) and only one same-size: the drop is size, not a regression. 187 of 489 rounds did not compile.
  - **Passed:** 18c0c0 `SndManager::UpdateTrackVolumes`, 1912c0 `CAsyncTransfer::SynchroTransfer` (round 1),
    18ee40 `CASyncBankLoader::Synchro` (104 instructions, round 2).
  - **The mfc0 rule works:** 160680 `SND_fn_rGetRandomSnd` compiled in every round once it used
    `SND_fn_ulGetCount()` (rounds 7-10), best 4 off; it still wasted rounds 1-6 on its own spellings first.
  - **Header requests (unchecked):** 1975c0 `TAdpcm` ctor (2-argument `SND_fn_pvMallocSnd` again: ruled out 4 Nov),
    180f00 (`_SoundParam::Freq` as a pointer), 158e00 (`_SND_tdstTypeObjectSoundInfo` missing, as 158580 in run 11),
    18dd80 and 18db80 (`_tdstMemoryCluster` member types; `_tdstSoundMemoryAllocatorCluster` size 12 not 8),
    191f80 (`std` namespace), 196540 (`SND_fn_vFreeSnd` return type), 169b40 (placement `CRenderer::operator new`),
    173640 (`_tdstMTTChannelElement` missing `stMTTrackVolume`). 158e00 + run 11's 158580 and 18dd80 + 18db80 +
    run 11's 18e780 are two repeat themes worth checking first.
  - **Nearest misses (hand pass):** 15b040 `KillAndDestroyAsso` and 183a00 `fn_bGeneratePath` 1 off; 17f5c0
    `SND_fn_rDopplerPitch` and 17c180 `SND_fn_hASyncLoadResBinarySnd` 3 off; 16d280 `TMultiTrack` ctor 4 off,
    160680 4 off.
  - **Far misses:** 158e00 (7 of 80), 188e80 (8 of 89), 180f00 (13), 191100 (14), 181040, 190100, 17e500, 17e000
    (24-28).
  Folders copied to `game/analysis/qwen_pc/`.

## 5 Nov: fixes, hand pass, permuter

- **Header requests, all 29 checked:** none needed the change Qwen asked for. The real causes were gaps in what the
  prompt says, fixed in general:
  - **File statics had no types** (the prompt gave only "N bytes; declare it yourself"): `dare_header.py` writes
    `statics.tsv` (161 DWARF declarations; the 5 `sndCList` statics get their template arguments from POP1's
    `__sinit` constructor calls) and the prompt says "write `extern <decl>;`" and pulls the type into the excerpt
    (158580/158e00/15ffc0 `g_d_pstTypeObjectSoundInfos` is a `tdstTypeObjectInfos*`; 173640, 18dd80/18db80/18e780).
  - **More call targets and globals named:** `dare_callees.py` aligns the 322 functions whose call or global counts
    differ (POP1's asserts) on the names already known: 406 -> 547 call targets, 234 -> 305 globals (85 of 87
    checkable new names agree; a function's own retail address now wins over a vote). 0x199640 =
    `SND_fn_vDisplayError` (67 votes), 0x183fc0 `SND_fn_szGetCurrentLangDirectory`, 0x161880 `CRenderer::operator
    new` (by hand, `EXTRA_NAMES`). An address inside a named object gets name+offset (`gs_listMemoryCluster` +0).
  - **Virtual calls named:** `vtables.tsv` (POP1 `__vt__`); `lw R,0(obj); lw R,OFF(R); jalr R` gets
    "; = virtual slot N: Method (classes)". Three near misses had called the wrong slot's method.
  - Constructors, destructors and operators shown as C++ (`CPath::operator new`, `RPS2::~RPS2`).
  - **Header changes:** `SND_fn_vDisplayError(unsigned char, char *)` (retail and POP1 pass a struct member with
    `lbu`; every other caller passes a constant); 14 methods static (`STATIC_METHODS`): `GetHwModuleId`,
    `ASyncLoadBinaryResourceSample` by hand, 12 by a register test (the method reads its parameters from a0 on and
    never the register its last parameter would be in with a `this`; `UpdateTrackVolumes`, which matched as a normal
    method, passes the test as non-static). `--rejudge`: 224 of 225 PASS still matched; 1967c0's own conflicting
    `SND_fn_vDisplayError` extern removed. Re-judging the top attempts of every miss: 2 more matched as they were.
- **Prompt lessons added:** `if (a || b) return;` (two separate ifs compile differently: 183080, 16b740);
  `(x != 0) ^ 1` for `sltu`+`xori` with no `andi 0xff` (`!x` adds it: 166b40, 167c00); `lui v0,0xffff` in a delay
  slot = return -1.0 in 16.16 (18d3c0).
- **Hand matches (24):** 18d3c0 18d340 162240 193a40 168dc0 194640 183a00 15c8c0 166b40 160a80 18f080 183080
  189f40 16a380 167c00 169580 16b9c0 190940 194880 18eb80 16b180 16b800 16b1c0 16b740 (PASS = "hand (5 Nov)").
  Nearly all were wrong meaning, not register allocation: a wrong virtual slot, a wrong argument or member, a
  constant (-1 for -1.0), an `||`, a missing `else`. 18f080: retail 0x174100 takes one argument where POP1's
  `fn_bGetMapsFileName` takes two. 16a380: the semaphore static at 0x4e8fa0 is retail-only (named
  `gs_lWorkerSemaId` by us).
- **`permute.py` (new):** decomp-permuter's random mode parses C only (its README: C++ only through hand-written
  PERM macros) and it does not run on native Windows, so this is a small C++ version on `match.py`: 1-3 random
  edits per candidate (move a statement, flip signedness, swap operands, invert an if/else, split/join an
  initialised declaration, `__typeof__` temporary (mwccps2 takes it), ++ forms, merge/split `if (a || b) return`),
  hill-climbing on differing instructions, N worker processes (~280 candidates/s on 14 workers). Matched 15b040 on
  its 2nd candidate (Qwen freed before clearing members); the 8 others of the day run (3 minutes each) were
  wrong-meaning misses it cannot fix. Improvements are written as `attempts/permNN.cpp` (Qwen resumes from them),
  `PERMUTED` = start hash + minutes (one pass per start attempt).

## Fixes after run 1 (all done 28 Oct night)

1. **Globals named.** DARE reaches every global as `lui at,hi` + `lw lo(at)` (851 accesses in 213 of 669
   functions, zero `gp`), and the prompt did not say which global is where. `dare_callees.py` also writes
   `globals.tsv` (pairs retail and POP1 absolute data accesses like the calls; 234 addresses named, no disagreeing
   votes). The prompt puts `; = name` on each instruction that reaches one, replaces Ghidra's `DAT_` / `iRam...`
   names, and lists the globals with sizes ("declared in dare.h" or "declare it yourself": most are file statics,
   not in dare.h).
2. **Compiler flag `-sdatathreshold 0`** (in `match.py` FLAGS and `pccc.sh`). Without it an `extern` global compiles
   to `lw 0(gp)`; with it to retail's `lui at` / `lw (at)`. Before this, no function with a global could match.
3. **Member offsets in the header excerpt** (`// +0x14` per member, `// size N` per struct, from the DARE_CHECK
   lines). Run 1 picked wrong members (`LoadBinaryResourceSilence`) or cast raw offsets (`TTransition::RemoveChild`).
4. **Float constants**: `; = 3276.8f` on an `mtc1` of a `lui`/`ori` constant (`GetLatency` returns `(int)3276.8f`;
   run 1 wrote 1000).
5. **Switch hint reworded**: "two or more tests of the same value against different constants"; a single test is
   an if (run 1 wrote a switch for `TVirtualSample::Resume`).
6. **Rules**: never write a type definition (run 1 redefined `sndCList`); a method called without `this` that is
   not declared static = NEEDS HEADER CHANGE (`RPS2::GetResLength` is called with a0 = the union by value: probably
   static, DWARF lost it; not fixed in `dare.h`). Still open: `fn_lSendSoundRequestOnChannelWithFadeIn` (0x15d940,
   7 args incl. a float) is neither in dare.h nor in the callee map, and retail tail-calls it (`j`).
- Review fix (28 Oct): the `lui` pairing forgets a register's value when another instruction writes it (a stale
  value gave bogus names such as 0x10000 `@150`; a self-check in the session confirmed no float note survives an
  overwrite).

## Hand matches (28 Oct; sources in `game/decomp/dare/` and `game/decomp/try/`)

- `SND_fn_bIsResourceStreamedSnd` (0x17dc40, `snd_b.cpp`): a `switch (pstBRDisk->eType)`; the `if` / `else if`
  version inverts the branches. Compiling a `.c` source as C++ is fine.
- `__ct__5CToolFv` (0x161380, `ct_c.cpp`): **stores come out in source order** (`m_lNbChildren`, `m_eState`, `m_pRes`);
  the vptr store is generated first.
- `SND_fn_vSndRealTwinToSndReal` (0x1612c0, `rt_a.cpp`): plain `int` shifts; `(short)` casts add `dsll32`/`dsra32`.
- `SND_fn_vResetStack` / `SND_fn_pvGetStackHead` / `SND_fn_vDesInitTheme` (`try/reset.cpp`, `head.cpp`, `theme.cpp`):
  matched once globals had names and the flag was on (`delete g_pThemeManager;` = the `__dt__(p, 1)` call).

## Useful beyond the decomp (for 0.3)

- DARE's real build settings: CodeWarrior 3.0.3, `-O4,p -enum min -sdatathreshold 0`.
- Every match confirms POP1's struct layouts, enums, vtables and member offsets for retail DARE.
- `callees.tsv` (406 call targets) and `globals.tsv` (234 data addresses) name retail code and data that Ghidra
  shows as `FUN_` / `DAT_`: applying them to the Ghidra project `PoP_SLUS20743` would make retail DARE readable for
  the 0.3 design (not done).

## Tools (all in `tools/decomp/`)

- `dwarf1.py`: DWARF version 1 reader (`dies(elf)` -> DIE tree).
- `dare_callees.py <POP1.ELF> <SLUS_207.43> <units.tsv> <callees.tsv> [<globals.tsv>]`: names retail call targets: the k-th `jal`
  of a retail function calls what the k-th `jal` of POP1's copy calls (only when both have the same count; 322
  functions differ, probably POP1's extra asserts), votes across all functions. 406 targets named, 14 with
  disagreeing votes (the prompt uses a name only when it outvotes all others): **704 of 863 unnamed retail calls
  named.** Confirmed 0x173140 = `SND_fn_bIsResourceStreamedSxd`. Self-check: 0x163100 = `BranchPath::GetPos`.
  `globals.tsv`: the same for absolute data addresses (columns: retail addr, POP1 symbol[+off], size, votes, other).
  Rerun after regenerating `units.tsv`, then copy both files to `game\decomp\dare\` on the PC.
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
  (create an empty `dare_fix.h` beside it; it also writes `statics.tsv` and `vtables.tsv`: copy them to the PC with the header). Retail assembly and Ghidra C for the 669 functions: `analyzeHeadless`
  with `PoPDisasm.java` and `PoPDecompileTo.java` on `game/decomp/dare/addrs.txt` (outputs `asm/`, `ghidra_c/`).
  **Ghidra headless needs `JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home`** (as in
  `tools/rebuild.sh`), or it fails with "Unable to prompt user for JDK path".
- `qwen_match.py <mangled | addr hex> [--rounds 8] [--until <unix>] [--url ...]` (PC): the Qwen loop for one
  function. Prompt = retail asm (callees, globals and float constants named), Ghidra C (same names), declaration,
  parameter / local names from `protos.tsv`, the globals it uses with sizes, the dare.h excerpt it needs (class,
  bases, member types with offsets, enums, C declarations, globals; capped at 30 KB) and the compiler lessons. Feedback = `match.py`'s diff. Results: `game/analysis/qwen/dare_<addr>/` (`match.log`,
  `attempts/`, `best.cpp` = most matching instructions, `PASS`, or `HEADER` when Qwen reports a header change is
  needed). `--prompt` prints the prompt (works on the Mac); `--try <file>` judges a file like a round. Needs
  `game/decomp/dare/` on the PC (copied 28 Oct; recopy after regenerating: `tar czf - -C game/decomp dare | ssh ...`).
- `permute.py <addr> [--minutes 20] [--jobs 6]` / `--queue` / `--lane --until <unix>` (PC): the permuter (see 5 Nov).
- `qwen_match.py --rejudge` (PC): every PASS's `best.cpp` against the current `dare.h`; run after any header change.
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

- **Compiler flags: `-O4,p -enum min -sdatathreshold 0`** (the last found 28 Oct: globals via `lui at`, never `gp`). DWARF gives many enums a byte size of 1; `-enum min` reproduces that
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

1. **Done 29 Oct** ("29 Oct" section above): header requests, near misses, Ghidra names.
2. **Done 29 Oct evening:** the 16 one-vote callee names the prototype contradicts are dropped from the prompt
   (`WRONG_NAMES` in `qwen_match.py`; the function then shows as `FUN_<addr>`).
3. Keep the queue running on later nights (pass rate fell from 41% in run 2 to 12% in run 5 as functions grow; run 6
   tests the compile-fix and second-draft changes); run 1's old misses need more than prompt fixes.
4. Later: a whole-unit build and link check, splitting the rest of the ELF, decomp-permuter for near-misses.

Built and done (28 Oct): three hand matches; `qwen_match.py` runner; smoke test; queue (`--queue`, `--summary`);
night job `oct28.ps1`. Lessons: run the runner with output to a file or under `start-job.ps1` (a run with stdout
over plain ssh died between a MATCH and writing PASS, and ssh hung after Python exited; a restart now finds such a
MATCH). `--rounds N` = N more rounds; no-code rounds leave `roundNN.nocode.txt` and count on resume; a draft in
the reasoning is judged when the answer has none. Known gap: free C++ `extern` declarations are pulled into the
excerpt by short name next to methods of the same name (e.g. `extern int GetPos(int)` beside `BranchPath::GetPos`).
