"""Local-model port of a VU1 program stage (tools/vu1port/): the model writes C++ stage() (vu_task.h) from the
stage's annotated disassembly; the judge (judge.cpp, built by build.sh) runs it on every case vu1replay
recorded (cases.sh) and compares with the runtime's VU1 interpreter. The model gets the judge's report and
tries again. Results: game/analysis/qwen/vu1_<stage>/{port.log, spec.md, attempts/, best.cpp, PASS}. Resumable.

Usage: python3 -I tools/vu1port/port.py <stage> [--url http://127.0.0.1:8033] [--rounds 12] [--spec-only]
       python3 -I tools/vu1port/port.py <stage> --judge <file.cpp>     (judge one file, e.g. a known-good answer)
       python3 -I tools/vu1port/port.py --chain                        (whole P6 from the passed stages, judged
                                                                         on whole runs: game/analysis/vu1port/whole.cases)
"""
import importlib.util
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from vudis import disasm  # noqa: E402

WORK = os.path.join(ROOT, 'game', 'analysis', 'vu1port')
IMAGE = os.path.join(ROOT, 'game', 'analysis', 'vu1trace', 'vu1_1d5b02aa79c05d41.bin')  # P6 (+ an earlier tail)

# P6, the water program (ELF 0x4DAC8C): stages along its loops (coverage of the two water traces, 19 Oct).
# ranges: the code the stage executes; exits: where it hands over (0xffffffff = the program's end).
STAGES = {
    'p6_setup': dict(entry=0x000, exits=[0x268, 0x1648], ranges=[(0x000, 0x260)]),
    # variant A: grid 1 (outer loop 0x288-0x5d8 around the inner 0x338-0x580), the clip check, grid 2 (outer
    # 0x930-0xce0 around 0x9e0-0xc88), the tail. Loops are split at their heads (a whole grid overran the
    # model's 32k-token budget twice); a loop's exit is the pair after its branch's delay slot. Inner loops
    # (51-75 pairs, also too long) are split where no DIV result is pending and no FMAND is apart from the
    # instruction whose flags it reads.
    'p6_a_pre': dict(entry=0x268, exits=[0x288, 0x930], ranges=[(0x268, 0x280)]),
    'p6_a_g1_row': dict(entry=0x288, exits=[0x338], ranges=[(0x288, 0x330)]),
    # 22 Oct: the inner loops that failed as 20-41 pairs, split again at pairs where no DIV is pending and
    # no FMAND is apart from its MAC source (tools/vu1port cut scan: vu1replay --stage <entry>:<cut>)
    'p6_a_g1_in1a': dict(entry=0x338, exits=[0x3b0], ranges=[(0x338, 0x3a8)]),
    'p6_a_g1_in1b': dict(entry=0x3b0, exits=[0x448], ranges=[(0x3b0, 0x440)]),
    'p6_a_g1_in2a': dict(entry=0x448, exits=[0x4a0], ranges=[(0x448, 0x498)]),
    'p6_a_g1_in2b': dict(entry=0x4a0, exits=[0x4f8], ranges=[(0x4a0, 0x4f0)]),
    'p6_a_g1_in2c': dict(entry=0x4f8, exits=[0x338, 0x590], ranges=[(0x4f8, 0x588)]),
    'p6_a_g1_tail': dict(entry=0x590, exits=[0x288, 0x5e0], ranges=[(0x590, 0x5d8)]),
    'p6_a_check': dict(entry=0x5e0, exits=[0xcf0, 0x830, 0x14e8, 0x1528], ranges=[(0x5e0, 0x828), (0x1568, 0x1640)]),
    'p6_a_g2_head': dict(entry=0x930, exits=[0x9e0], ranges=[(0x930, 0x9d8)]),
    'p6_a_g2_in1a': dict(entry=0x9e0, exits=[0xa58], ranges=[(0x9e0, 0xa50)]),
    'p6_a_g2_in1b': dict(entry=0xa58, exits=[0xae8], ranges=[(0xa58, 0xae0)]),
    'p6_a_g2_in2a': dict(entry=0xae8, exits=[0xb60], ranges=[(0xae8, 0xb58)]),
    'p6_a_g2_in2b': dict(entry=0xb60, exits=[0xbd8], ranges=[(0xb60, 0xbd0)]),
    'p6_a_g2_in3a': dict(entry=0xbd8, exits=[0xc30], ranges=[(0xbd8, 0xc28)]),
    'p6_a_g2_in3b': dict(entry=0xc30, exits=[0x9e0, 0xc98], ranges=[(0xc30, 0xc90)]),
    'p6_a_g2_tail': dict(entry=0xc98, exits=[0x930, 0xcf0], ranges=[(0xc98, 0xce8)]),
    'p6_tail': dict(entry=0xcf0, exits=[0xffffffff], ranges=[(0xcf0, 0xd28)]),
    # variant B: the same shape
    'p6_b_pre': dict(entry=0x1648, exits=[0x1698, 0x1b60], ranges=[(0x1648, 0x1690)]),
    'p6_b_g1_row': dict(entry=0x1698, exits=[0x1770], ranges=[(0x1698, 0x1768)]),
    'p6_b_g1_in1a': dict(entry=0x1770, exits=[0x17f0], ranges=[(0x1770, 0x17e8)]),
    'p6_b_g1_in1b': dict(entry=0x17f0, exits=[0x1840], ranges=[(0x17f0, 0x1838)]),
    'p6_b_g1_in2': dict(entry=0x1840, exits=[0x1770, 0x1910], ranges=[(0x1840, 0x1908)]),
    'p6_b_g1_tail': dict(entry=0x1910, exits=[0x1698, 0x1960], ranges=[(0x1910, 0x1958)]),
    'p6_b_check': dict(entry=0x1960, exits=[0x1dd0, 0x14e8, 0x1528, 0x1b28], ranges=[(0x1960, 0x1b20), (0x1568, 0x1640), (0x2550, 0x2628)]),
    'p6_b_g2_head_a': dict(entry=0x1b60, exits=[0x1bd0], ranges=[(0x1b60, 0x1bc8)]),
    'p6_b_g2_head_b': dict(entry=0x1bd0, exits=[0x1c40], ranges=[(0x1bd0, 0x1c38)]),
    'p6_b_g2_in1a': dict(entry=0x1c40, exits=[0x1c98], ranges=[(0x1c40, 0x1c90)]),
    'p6_b_g2_in1b': dict(entry=0x1c98, exits=[0x1ce0], ranges=[(0x1c98, 0x1cd8)]),
    'p6_b_g2_in2a': dict(entry=0x1ce0, exits=[0x1d28], ranges=[(0x1ce0, 0x1d20)]),
    'p6_b_g2_in2b': dict(entry=0x1d28, exits=[0x1c40, 0x1d78], ranges=[(0x1d28, 0x1d70)]),
    'p6_b_g2_tail': dict(entry=0x1d78, exits=[0x1b60, 0x1dd0], ranges=[(0x1d78, 0x1dc8)]),
    'p6_b_tail': dict(entry=0x1dd0, exits=[0xffffffff], ranges=[(0x1dd0, 0x1e08)]),
}

PRIMER = r"""## How to read the VU1 code
Each line is one instruction pair at a byte address (hex): the upper (floating-point) instruction, then the
lower one. A trailing "E" marks the program's end: the pair after it (its delay slot) still executes, then
the program stops.
- Order: execute the pairs in address order. A result is visible to the next pair (the hardware stalls until
  it is ready), with these exceptions, all marked on the lines: within one pair both instructions read their
  operands before either writes ([same pair ...]); Q, MAC and clip flags are read without waiting, so a
  reader may see an older value ([Q = ...], [MAC flags of ...], [clip flags as of ...]); a branch right after
  an integer instruction that writes the register it tests sees the old value ([branch sees ...]).
- vfN = four floats x, y, z, w (vu.vf[N][0..3]). The suffix after the opcode (".xyzw", ".xy", ".z") is the
  destination mask: only those lanes are written. A broadcast letter on the opcode (ADDx, MULAy, MADDw,
  SUBz, ...) and the matching operand (vf0x, vf14z, ...) means that single lane of the last operand is used
  for every lane: "ADDx.xyz vf21, vf9, vf0x" is vf21[c] = vf9[c] + vf0[x] for c = x, y, z. The letter on the
  opcode is never a destination lane: "SUBx.z vf6, vf20, vf20x" writes ONLY vf6.z (= vf20.z - vf20.x) and
  leaves vf6.x, y, w unchanged; "ADDy.x vf5, vf0, vf5y" writes only vf5.x (= 0 + vf5.y). Suffix i or q
  (ADDi, MULq, MADDAq, ...): the I or Q register instead of the last operand.
- vf0 is always (0, 0, 0, 1): writes to vf0 are discarded (such instructions only set flags). vi0 is 0.
- ACC: MULA, ADDA, SUBA, MADDA, MSUBA write ACC (the masked lanes) instead of a vf register. Per lane, with
  b = the second operand, its broadcast lane, I or Q:
  ADD d = a + b; SUB d = a - b; MUL d = a * b; MADD d = ACC + a * b; MSUB d = ACC - a * b;
  ADDA ACC = a + b; SUBA ACC = a - b; MULA ACC = a * b; MADDA ACC = ACC + a * b; MSUBA ACC = ACC - a * b.
  ADDA / SUBA / MULA do NOT read the old ACC. "ADDAi.xyzw ACC, vf14, I" is ACC = vf14 + I;
  "MSUBi.xyzw vf13, vf13, I" is vf13 = ACC - vf13 * I. Product first (rounded), then the add.
- MAX / MINI: per-lane maximum / minimum. ABS: absolute value. FTOIn: float * 2^n converted to int32
  (truncated toward zero, saturating), its bits stored in the lane. ITOFn: the lane's int32 bits converted
  to float, divided by 2^n (n = 0, 4, 12, 15).
- DIV Q, vfSs, vfTt: Q = vfS[s] / vfT[t]; SQRT Q, vfTt: Q = sqrt(vfT[t]); RSQRT Q, vfSs, vfTt: Q = vfS[s] / sqrt(vfT[t]).
  The new Q appears several pairs later: each reader of Q says which DIV it sees.
- LOI v: the pair's lower word is the value v loaded into I; the upper of the same pair still reads the old I.
- Integer registers vi1..vi15 hold 16 bits (keep them as int32 sign-extended from 16 bits: int16_t casts).
  IADD/ISUB/IAND/IOR vD, vS, vT; IADDI/IADDIU/ISUBIU vT, vS, imm. MTIR viT, vfSs: viT = low 16 bits of the
  lane's bits. MFIR.dest vfT, viS: each masked lane's bits = the sign-extended viS.
- Data memory: vu.mem, 16 bytes per qword address. LQ.dest vfT, imm(viS): vfT[masked lanes] = qword (viS + imm).
  SQ.dest vfS, imm(viT): store the masked lanes to qword (viT + imm). LQI / SQI (vi++): use the address, then
  add 1 to the register; LQD / SQD (--vi): subtract 1 first. ILW.lane viT, imm(viS): viT = low 16 bits of that
  32-bit word of qword viS + imm; ISW.lane viT, imm(viS): that word = viT & 0xFFFF (upper 16 bits zero).
  ILWR / ISWR (viS): the same with no offset. Addresses wrap at 1024 qwords.
- XTOP viT: viT = vu.top. XGKICK viS: call xgkick(vu, vu.vi[S]) (the harness copies the GIF packet).
- CLIPw.xyz vfS, vfT: flags of vfS x, y, z against |vfT.w|: bit0 x > +|w|, bit1 x < -|w|, bit2 y > +|w|,
  bit3 y < -|w|, bit4 z > +|w|, bit5 z < -|w|; clip = ((clip << 6) | flags) & 0xFFFFFF.
- FCSET imm: clip = imm. FCAND vi1, imm: vi1 = (clip & imm) != 0. FCOR vi1, imm: vi1 = ((clip | imm) &
  0xFFFFFF) == 0xFFFFFF. FCEQ vi1, imm: vi1 = (clip & 0xFFFFFF) == imm. FCGET viT: viT = clip & 0xFFF.
- MAC flags: the upper arithmetic instructions (ADD, SUB, MUL, MADD, MSUB and their A / i / q / broadcast
  forms, OPMULA, OPMSUB) set, per written lane, a zero bit and a sign bit; unwritten lanes 0. Zero bits:
  x = 8, y = 4, z = 2, w = 1; sign bits: x = 0x80, y = 0x40, z = 0x20, w = 0x10. FMAND viT, viS: viT = mac & viS;
  FMOR: mac | viS; FMEQ: mac == viS. The marked line says which instruction's flags a reader sees.
- Branches: B target; BAL viT, target (viT = (address of the pair after the delay slot) / 8); JR viS (jump to
  viS * 8); IBEQ / IBNE viT, viS, target; IBLTZ / IBGTZ / IBLEZ / IBGEZ viS, target. The pair after a
  branch (its delay slot) always executes before the jump. Use goto labels in C++.
"""

TASK = """Translate one stage of a PlayStation 2 VU1 microprogram (the water renderer of Prince of Persia: The Sands
of Time) into C++17. Correctness is judged automatically: the judge runs your stage() on {cases} recorded
states (the real program's state when it reached the stage's entry, in two water scenes) and compares the
state at the exit (vf, vi, ACC, Q, P, I, clip flags, data memory, kicked packets) with the original's. Floats
may differ by at most 4 units in the last place; integers must match exactly.

## Fixed API (vu_task.h, do not change; your file must #include "vu_task.h")
```cpp
{header}
```

{primer}
## Example: an earlier stage of this program, translated (correct: it passes its judge bit-exactly)
Its code:
```
{example_code}
```
Its translation:
```cpp
{example_cpp}
```

## Your stage: {name}
Entry: pc 0x{entry:x}. It ends when it is about to execute one of these addresses (set vu.pc to it and
return): {exits}. {exit_notes}
Translate every instruction in order, as the example does (comment each pair's address), honouring the
marked exceptions. Branch targets inside the stage become labels.
```
{code}
```

Write the complete file in ONE ```cpp code block: #include "vu_task.h", helper functions as you need, and
void stage(VuState &vu). No main(), no other files.
"""


def stage_code(ranges, hints):
    """The stage's disassembly with the replayer's hints on the lines where they matter."""
    img = open(IMAGE, 'rb').read()
    q, mac, clip, vib = {}, {}, {}, {}
    for line in open(hints):
        f = line.split()
        pc = int(f[0], 16)
        k = None
        for tok in f[1:]:
            if tok in ('q', 'mac', 'clip', 'vibackup'):
                k = tok
                continue
            {'q': q, 'mac': mac, 'clip': clip, 'vibackup': vib}[k].setdefault(pc, set()).add(tok)
    out = []
    for lo, hi in ranges:
        if out:
            out.append('...')
        prev_lower = ''
        for line in disasm(img[lo:hi + 8], lo):
            pc = int(line[:4], 16)
            upper, lower = line[6:41].strip(), line[41:].strip()
            notes = []
            if upper and re.search(r'\b[A-Z]+A?q\.', upper) and pc in q:
                src = sorted(q[pc])
                notes.append('Q = result of the DIV at ' + ' or '.join('0x' + s if s != 'entry' else 'the entry state' for s in src))
            if re.match(r'FM(AND|OR|EQ)', lower) and pc in mac:
                notes.append('MAC flags of the upper instruction at ' + ' or '.join('0x' + s if s != 'entry' else 'the entry state' for s in sorted(mac[pc])))
            if re.match(r'FC(AND|OR|EQ|GET)', lower) and pc in clip:
                notes.append('clip flags as of the CLIP at ' + ' or '.join('0x' + s if s != 'entry' else 'the entry state' for s in sorted(clip[pc])))
            m = re.match(r'(IBEQ|IBNE|IBLTZ|IBGTZ|IBLEZ|IBGEZ) (vi\d+)(?:, (vi\d+))?', lower)
            if m and pc in vib:
                tested = {m.group(2), m.group(3)} - {None}
                old = sorted(tested & {v for v in vib[pc]})
                if old and re.search(r'\b(%s)\b' % '|'.join(old), prev_lower.split(',')[0] if prev_lower else ''):
                    notes.append('branch sees %s from before the previous pair wrote it' % ', '.join(old))
            same = same_pair(upper, lower)
            if same:
                notes.append(same)
            if re.match(r'(B|BAL|JR|JALR|IBEQ|IBNE|IBLTZ|IBGTZ|IBLEZ|IBGEZ)\b', prev_lower or ''):
                notes.append('delay slot of the branch above: executes before the jump, also when it is taken')
            if lower.startswith('LOI') and upper and re.search(r'\b[A-Z]+A?i\.', upper):
                notes.append('same pair: the upper reads the I loaded before this LOI')
            out.append(line.rstrip() + ('   [' + '; '.join(notes) + ']' if notes else ''))
            prev_lower = lower
    return '\n'.join(out)


def regs(text):
    return re.findall(r'vf\d+', text)


def same_pair(upper, lower):
    """A register written by one half of a pair and read by the other: the reader sees the old value."""
    if not upper or upper == 'NOP' or not lower or lower == 'NOP':
        return ''
    u_ops = upper.split(None, 1)[1] if ' ' in upper else ''
    l_ops = lower.split(None, 1)[1] if ' ' in lower else ''
    u_regs = regs(u_ops)
    u_dst = u_regs[0] if u_regs and not upper.startswith(('CLIP', 'MULA', 'ADDA', 'SUBA', 'MADDA', 'MSUBA', 'OPMULA')) else None
    u_src = u_regs[1:] if u_dst else u_regs
    l_name = lower.split()[0].split('.')[0]
    l_regs = regs(l_ops)
    l_dst = l_regs[0] if l_regs and l_name in ('LQ', 'LQI', 'LQD', 'MOVE', 'MR32', 'MFIR', 'MFP') else None
    l_src = [r for r in l_regs if r != l_dst] if l_dst else l_regs
    if l_name in ('MOVE', 'MR32') and len(l_regs) > 1:
        l_src = l_regs[1:]
    notes = []
    if u_dst and u_dst != 'vf0' and u_dst in l_src:
        notes.append('same pair: the lower reads %s before the upper writes it' % u_dst)
    if l_dst and l_dst in [r.rstrip('xyzw') for r in u_src]:
        notes.append('same pair: the upper reads %s before the lower writes it' % l_dst)
    return '; '.join(notes)


def build_judge(cpp, exe):
    if os.name == 'nt':
        # the PC (inside the MSVC developer environment, tools/windows/qwen-night.ps1): build.sh's flags in
        # MSVC terms (no FMA contraction: /fp:precise; AddressSanitizer)
        obj = os.path.dirname(exe)
        cmd = ['cl', '/nologo', '/std:c++17', '/EHsc', '/O1', '/Zi', '/fp:precise', '/fsanitize=address', '/I', HERE,
               '/Fo' + obj + os.sep, '/Fd' + os.path.join(obj, 'judge.pdb'), os.path.join(HERE, 'judge.cpp'), cpp, '/Fe' + exe]
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=obj)
    else:
        r = subprocess.run(['bash', os.path.join(HERE, 'build.sh'), cpp, exe], capture_output=True, text=True)
    return r.returncode == 0 and os.path.exists(exe), (r.stdout + r.stderr)


def judge(stage, cpp, out_dir):
    exe = os.path.join(out_dir, 'judge_bin' + ('.exe' if os.name == 'nt' else ''))
    if os.path.exists(exe):
        os.remove(exe)
    ok, log = build_judge(cpp, exe)
    if not ok:
        errs = [l for l in log.splitlines() if 'error' in l][:15]
        return False, 'Your file did not compile:\n' + '\n'.join(errs or log.splitlines()[-15:])
    r = subprocess.run([exe, os.path.join(WORK, stage[3:] + '.cases')], capture_output=True, text=True, timeout=1200)
    text = (r.stdout + r.stderr).strip()
    if r.returncode == 3 or (r.returncode not in (0, 1) and 'AddressSanitizer' not in text):
        text = 'Your code did not finish (an endless loop, or it crashed); judge exit %d.\n%s' % (r.returncode, text[-3000:])
    elif 'AddressSanitizer' in text:
        text = 'Your code crashed (AddressSanitizer):\n' + '\n'.join(text.splitlines()[:25])
    return r.returncode == 0 and text.startswith('PASS'), text[:6000]


def passed_port(name):
    """The passing port of a stage (hand port first), or None."""
    for path in (os.path.join(HERE, 'known_good', name + '.cpp'),
                 os.path.join(ROOT, 'game', 'analysis', 'qwen', 'vu1_' + name, 'best.cpp')):
        if os.path.exists(path):
            return path
    return None


def example_for(name):
    """The example stage: the nearest earlier stage that passed (the same loop's code is the closest
    pattern: 22 Oct), else the hand-ported setup stage."""
    names = list(STAGES)
    for k in range(names.index(name) - 1, -1, -1):
        if passed_port(names[k]) and os.path.exists(os.path.join(WORK, names[k][3:] + '.hints')):
            return names[k]
    return 'p6_setup'


def spec(name):
    st = STAGES[name]
    ex_name = example_for(name)
    ex = STAGES[ex_name]
    hints = os.path.join(WORK, name[3:] + '.hints')
    cases = os.path.join(WORK, name[3:] + '.cases')
    n = 0
    with open(cases, 'rb') as f:
        data = f.read()
    n = data.count(b'CASE')  # approximate (magic per case)
    exit_notes = ''
    if any(e in (0x830, 0x14e8, 0x1528, 0x1b28) for e in st['exits']):
        exit_notes = ('0x830, 0x14e8, 0x1528 and 0x1b28 are calls into the polygon clipper (BAL): the recorded scenes never '
                      'reach them; if your code does, stop there (after the BAL and its delay slot).')
    slots = [e for e in st['exits'] if any(lo <= e - 8 <= hi for lo, hi in st['ranges']) and
             re.search(r'\b(B|BAL|IBEQ|IBNE|IBLTZ|IBGTZ|IBLEZ|IBGEZ) ', disasm(open(IMAGE, 'rb').read()[e - 8:e], e - 8)[0][41:])]
    for e in slots:
        exit_notes += (' Exit 0x%x is also the delay slot of the branch at 0x%x: if that branch is taken, execute 0x%x and '
                       'then continue at the target; if it is not taken, stop before 0x%x (vu.pc = 0x%x; the next stage '
                       'executes it).' % (e, e - 8, e, e, e))
    if 0xffffffff in st['exits']:
        exit_notes = 'The stage ends with the program end (the pair marked E and its delay slot): set vu.pc = 0xffffffff.'
    return TASK.format(
        cases=n, header=open(os.path.join(HERE, 'vu_task.h')).read(), primer=PRIMER, name=name, entry=st['entry'],
        exits=', '.join('0x%x' % e if e != 0xffffffff else 'the program end' for e in st['exits']), exit_notes=exit_notes,
        code=stage_code([(lo, hi + 8 if hi + 8 in slots else hi) for lo, hi in st['ranges']], hints),
        example_code=stage_code(ex['ranges'], os.path.join(WORK, ex_name[3:] + '.hints')),
        example_cpp=open(passed_port(ex_name)).read())


def chain():
    """One program from the stages' passing ports: each file inside its own namespace (helpers may share
    names), a dispatcher on vu.pc. Judged on whole runs (entry 0, exit the program end)."""
    out = os.path.join(ROOT, 'game', 'analysis', 'qwen', 'vu1_p6_whole')
    os.makedirs(out, exist_ok=True)
    src = ['// generated by tools/vu1port/port.py --chain',
           '#include "vu_task.h"'] + ['#include <%s>' % h for h in (
           'algorithm', 'array', 'cfloat', 'climits', 'cmath', 'cstddef', 'cstdint', 'cstdio', 'cstdlib', 'cstring',
           'limits', 'type_traits', 'utility', 'vector')]
    table = []
    for name, st in STAGES.items():
        best = passed_port(name)
        if not best:
            print('%s: no passing port yet' % name)
            return 1
        src += ['namespace %s {' % name, '#include "%s"' % best.replace('\\', '/'), '}']
        table.append('        case 0x%x: %s::stage(vu); break;' % (st['entry'], name))
    src += ['void stage(VuState &vu)', '{', '    for (int n = 0; vu.pc != 0xffffffffu && n < 100000; ++n)',
            '        switch (vu.pc)', '        {'] + table + ['        default: return; // no stage starts here', '        }', '}']
    path = os.path.join(out, 'whole.cpp')
    open(path, 'w').write('\n'.join(src) + '\n')
    ok, report = judge('p6_whole', path, out)
    print(report)
    return 0 if ok else 1


def main(argv):
    if argv[0] == '--chain':
        return chain()
    name = argv[0]
    opt = lambda k, v: argv[argv.index(k) + 1] if k in argv else v
    # --rounds N: N more rounds on top of those already on disk (until 23 Oct it was the round number to
    # stop at, so a stage with old rounds got none: b_check ran no round on the night of 22-23 Oct)
    url, rounds = opt('--url', 'http://127.0.0.1:8033'), int(opt('--rounds', '12'))
    # --fresh N: every N rounds start a new conversation without the failed answer (a fresh start beat
    # continuing a failed one on 19-20 Oct); --until T: no new round after unix time T (overnight jobs)
    fresh, until = int(opt('--fresh', '0')), float(opt('--until', '0'))
    out = os.path.join(ROOT, 'game', 'analysis', 'qwen', 'vu1_' + name)
    att = os.path.join(out, 'attempts')
    os.makedirs(att, exist_ok=True)
    prompt = spec(name)
    open(os.path.join(out, 'spec.md'), 'w').write(prompt)
    if '--spec-only' in argv:
        print('%s: spec %d chars -> %s' % (name, len(prompt), os.path.join(out, 'spec.md')))
        return 0
    if '--judge' in argv:
        ok, report = judge(name, os.path.abspath(opt('--judge', '')), out)
        print(report)
        return 0 if ok else 1
    if os.path.exists(os.path.join(out, 'PASS')):
        print(name, 'already passed')
        return 0
    spec_m = importlib.util.spec_from_file_location('mdport', os.path.join(ROOT, 'tools', 'llm_port', 'meshdecode', 'port.py'))
    md = importlib.util.module_from_spec(spec_m)
    spec_m.loader.exec_module(md)
    log = open(os.path.join(out, 'port.log'), 'a')
    say = lambda s: (print(s, flush=True), log.write(time.strftime('%H:%M:%S ') + s + '\n'), log.flush())
    # a round interrupted between its answer and its judge report (job stopped in the morning): judge it again
    for f in sorted(os.listdir(att)):
        if f.endswith('.cpp') and not os.path.exists(os.path.join(att, f[:-4] + '.report.txt')):
            ok, report = judge(name, os.path.join(att, f), out)
            open(os.path.join(att, f[:-4] + '.report.txt'), 'w').write(report)
            if ok:
                open(os.path.join(out, 'best.cpp'), 'w').write(open(os.path.join(att, f)).read())
                open(os.path.join(out, 'PASS'), 'w').write(f[:-4] + '\n')
                say('PASS in %s (judged on resume)' % f[:-4])
                return 0
    done = sorted(f for f in os.listdir(att) if f.endswith('.cpp'))
    prev_code = open(os.path.join(att, done[-1])).read() if done else None
    prev_report = open(os.path.join(att, done[-1][:-4] + '.report.txt')).read() if done else None
    # a round without code leaves roundNN.nocode.txt (the end of its reasoning), so it counts too
    last = max((int(f[5:7]) for f in os.listdir(att) if f.startswith('round') and f.endswith(('.cpp', '.nocode.txt'))),
               default=0)
    overran = False
    kOverrun = ('\n\nYour last answer spent the whole token budget reasoning and contained no code. This time reason '
                'briefly, then write the complete file.')
    for n in range(last + 1, last + rounds + 1):
        if until and time.time() > until:
            say('round%02d: past the deadline; stopping' % n)
            return 1
        if fresh and n > 1 and (n - 1) % fresh == 0:
            prev_code = prev_report = None
        messages = [{'role': 'user', 'content': prompt + (kOverrun if overran and not prev_code else '')}]
        if prev_code:
            messages += [{'role': 'assistant', 'content': '```cpp\n' + prev_code + '```'},
                         {'role': 'user', 'content': 'Judge report for that file:\n\n' + prev_report +
                          '\n\nFind the cause by checking your translation of the instructions that write the differing '
                          'registers or memory against the code and the rules above, fix it, and reply with the complete '
                          'corrected file in one ```cpp block. Keep your reasoning short.' + (kOverrun if overran else '')}]
        try:
            content, reasoning, usage, secs = md.ask(url, messages)
        except Exception as e:  # server hiccup: try the round again later
            say('round%02d: request failed (%s); waiting 60 s' % (n, str(e)[:100]))
            time.sleep(60)
            continue
        code = md.extract(content)
        rname = 'round%02d' % n
        if code is None:
            # 34% of the rounds up to 23 Oct hit the token cap while reasoning; a file drafted there is still
            # worth judging (the judge decides, so a partial draft only costs a compile)
            draft = md.extract(reasoning)
            if draft and 'stage(' in draft:
                code = draft
                say('%s: no code block in the answer; judging the file drafted in the reasoning' % rname)
        if code is None:
            say('%s: no code block (%.0f s, %s tokens)' % (rname, secs, usage.get('completion_tokens')))
            open(os.path.join(att, rname + '.nocode.txt'), 'w').write(reasoning[-6000:])
            overran = True
            continue
        overran = False
        path = os.path.join(att, rname + '.cpp')
        open(path, 'w').write(code)
        ok, report = judge(name, path, out)
        open(os.path.join(att, rname + '.report.txt'), 'w').write(report)
        say('%s: %s; %.0f s, %s tokens' % (rname, (report.splitlines() or ['(empty report)'])[0][:120], secs, usage.get('completion_tokens')))
        prev_code, prev_report = code, report
        if ok:
            open(os.path.join(out, 'best.cpp'), 'w').write(code)
            open(os.path.join(out, 'PASS'), 'w').write(rname + '\n')
            say('PASS in %s' % rname)
            return 0
    say('stopped after %d more rounds' % rounds)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
