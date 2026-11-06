# match.py: compile one source file with CodeWarrior and compare one function with the retail bytes.
#   python match.py <src.cpp> <mangled name> <retail addr hex> <size> [--cc <compiler dir>] [--elf <SLUS_207.43>]
#   python match.py decomp/dare/<addr>.cpp        name and size from decomp/dare/units.tsv
#   python match.py --all                         every decomp/dare/<addr>.cpp (exit 1 if any fails)
#   python match.py --asm <addr hex>              the retail disassembly of one DARE function (local use only)
# Prints MATCH or an aligned per-instruction diff (relocated fields masked on both sides, so call targets and
# addresses of globals only need the right relocation, not the final address). Exit 0 = match.
# mwccps2.exe is Win32: run directly on Windows, through Wine elsewhere.
import difflib, os, re, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mipsdis import dis

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CC = os.path.join(ROOT, 'game', 'decomp', 'compilers', '3.0.3-020716', 'mwccps2.exe')
# -sdatathreshold 0: DARE reaches every global via lui at / lw (at), never gp (checked 28 Oct)
FLAGS = ['-c', '-nostdinc', '-O4,p', '-enum', 'min', '-sdatathreshold', '0']
ELF = os.path.join(ROOT, 'game', 'ps2', 'disc', 'SLUS_207.43')
HDR = os.path.join(ROOT, 'decomp', 'dare')  # shared dare.h, dare_fix.h, units.tsv and the matched <addr>.cpp files
R_MIPS_26 = 4


def obj_funcs(path):
    # {name: (code bytes, [(word index, mask, reloc symbol name)])}
    d = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    es, n, si = struct.unpack_from('<HHH', d, 0x2e)
    sh = [struct.unpack_from('<10I', d, shoff + i * es) for i in range(n)]
    symtab = next(s for s in sh if s[1] == 2)
    strs = sh[symtab[6]][4]
    syms = []
    for i in range(symtab[5] // 16):
        nm, val, size, info, oth, shn = struct.unpack_from('<IIIBBH', d, symtab[4] + i * 16)
        syms.append((d[strs + nm:d.index(b'\0', strs + nm)].decode(errors='replace'), val, size, info, shn))
    out = {}
    for name, val, size, info, shn in syms:
        if info & 15 != 2 or shn == 0 or shn >= len(sh):
            continue
        sec = sh[shn]
        code = d[sec[4] + val:sec[4] + val + size]
        rel = []
        for s in sh:
            if s[1] in (9, 4) and s[7] == shn:
                ent = 8 if s[1] == 9 else 12
                for j in range(s[5] // ent):
                    off, rinfo = struct.unpack_from('<II', d, s[4] + j * ent)
                    k = (off - val) // 4
                    if 0 <= k < size // 4:
                        rel.append((k, 0xfc000000 if (rinfo & 0xff) == R_MIPS_26 else 0xffff0000, syms[rinfo >> 8][0]))
        out[name] = (code, rel)
    return out


def objdiff(obj, name, addr, size, elf=None):
    # -> (matched, text)
    fs = obj_funcs(obj)
    cand = [n for n in fs if n == name] or [n for n in fs if n.split('__')[0] == name.split('__')[0]
                                             and name.split('F')[0] in n]
    if not cand:
        return False, 'function %s not in the object (it has: %s)' % (name, ', '.join(sorted(fs)) or 'nothing')
    code, rel = fs[cand[0]]
    elf = elf or open(ELF, 'rb').read()
    want = elf[addr - 0x100000 + 0x100:][:size]
    mask = {k: (m, s) for k, m, s in rel}
    A = [struct.unpack_from('<I', want, 4 * k)[0] for k in range(size // 4)]
    B = [struct.unpack_from('<I', code, 4 * k)[0] for k in range(len(code) // 4)]
    # aligned diff (1 Nov; a positional one marked everything after one extra instruction as different):
    # pair instructions with branch offsets and relocated fields masked, then a branch is the same only when both
    # sides jump to paired instructions
    mb = lambda k: mask.get(k, (0xffffffff, None))[0]
    # alignment key: opcode and registers only (retail immediates are final addresses where yours are relocations)
    key = lambda w: w & (0xffffffff if w >> 26 in (0, 0x1c, 0x10, 0x11, 0x12) and branch(w) is None else
                         0xfc000000 if w >> 26 in (2, 3) else 0xffff0000)
    ops = difflib.SequenceMatcher(None, [key(w) for w in A], [key(w) for w in B], autojunk=False).get_opcodes()
    pair = {i1 + d: j1 + d for tag, i1, i2, j1, j2 in ops if tag == 'equal' for d in range(i2 - i1)}
    pair[len(A)] = len(B)  # a branch to the end of the function
    rows = []  # (retail index or None, yours index or None, same)
    for tag, i1, i2, j1, j2 in ops:
        for d in range(max(i2 - i1, j2 - j1)):
            i, j = (i1 + d if i1 + d < i2 else None), (j1 + d if j1 + d < j2 else None)
            same = tag == 'equal'
            if same and branch(A[i]) is not None:
                same = pair.get(i + 1 + branch(A[i])) == j + 1 + branch(B[j])
            elif same:
                same = (A[i] & mb(j)) == (B[j] & mb(j))
            rows.append((i, j, same))
    lines, same = [], 0
    for i, j, ok in rows:
        same += ok
        ta = dis(A[i], addr + 4 * i) if i is not None else '-'
        tb = dis(B[j], addr + 4 * j) if j is not None else '-'
        if j is not None and j in mask and mask[j][1]:
            tb += '   <%s>' % mask[j][1]
        lines.append('%s %3x  %-34s | %s' % ('  ' if ok else '>>', 4 * (i if i is not None else j), ta, tb))
    matched = len(code) == size and all((a & mb(k)) == (b & mb(k)) for k, (a, b) in enumerate(zip(A, B)))
    head = 'MATCH' if matched else '%d of %d instructions differ (retail %d bytes, yours %d)' % (
        len(rows) - same, len(rows), size, len(code))
    return matched, head + (' [compiled as %s]' % cand[0] if cand[0] != name else '') + '\n' + \
        '     off  retail                             | yours\n' + '\n'.join(lines)


def branch(w):
    # PC-relative branch offset in instructions (target = index + 1 + offset), or None
    op, rt = w >> 26, (w >> 16) & 31
    if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19) or \
            op == 0x11 and (w >> 21) & 31 == 8:
        o = w & 0xffff
        return o - 0x10000 if o & 0x8000 else o
    return None


def selftest():
    # an extra instruction in the middle: only that one row differs, and the branch over it still pairs
    nop, ins = 0, 0x24420001  # addiu v0,v0,1
    beq = lambda off: 0x10400000 | (off & 0xffff)  # beq v0,zero,off
    # jal 0x16d180 and lui at,0x68 in retail are relocations (jal 0, lui at,0) in the object
    retail = [beq(4), nop, 0x0c05b460, 0x3c010068, 0x8c820000, 0x03e00008, nop]
    yours = [beq(5), nop, ins, 0x0c000000, 0x3c010000, 0x8c820000, 0x03e00008, nop]
    sec = b''.join(struct.pack('<I', w) for w in yours)
    elf = bytearray(0x100) + b''.join(struct.pack('<I', w) for w in retail)
    global obj_funcs
    keep, obj_funcs = obj_funcs, lambda _p: {'f': (sec, [(3, 0xfc000000, 'g'), (4, 0xffff0000, 'h')])}
    try:
        ok, text = objdiff(None, 'f', 0x100000, 28, bytes(elf))
    finally:
        obj_funcs = keep
    assert not ok and text.startswith('1 of 8 instructions differ'), text
    assert [l[:2] for l in text.splitlines()[2:]] == ['  ', '  ', '>>'] + ['  '] * 5, text
    print('selftest ok')


def compile_src(src, out_obj, cc=CC):
    # out_obj must sit beside src (every caller does this): plain file names survive Wine, unix paths may not
    r = subprocess.run(([cc] if os.name == 'nt' else ['wine', cc]) + FLAGS + ['-o', os.path.basename(out_obj), os.path.basename(src)], cwd=os.path.dirname(os.path.abspath(src)),
                       capture_output=True, text=True, errors='replace', timeout=120)
    msg = (r.stdout + r.stderr).strip()
    return os.path.exists(out_obj) and r.returncode == 0, msg


def units():
    # units.tsv: file, mangled, POP1 size, retail addr hex (or -), retail size, same size -> {addr: row}
    rows = (l.rstrip('\n').split('\t') for l in open(os.path.join(HDR, 'units.tsv')))
    return {int(r[3], 16): r for r in rows if r[3] != '-'}


def check(src, name, addr, size, elf, cc=CC):
    obj = os.path.splitext(os.path.abspath(src))[0] + '.o'
    if os.path.exists(obj):
        os.remove(obj)
    ok, msg = compile_src(src, obj, cc)
    if not ok:
        return 2, 'COMPILE ERROR\n' + msg
    matched, text = objdiff(obj, name, addr, size, elf)
    os.remove(obj)
    return (0 if matched else 1), text


def asm(addr, elf):
    from qwen_match import callee_names  # here: qwen_match imports this module
    u, name = units(), callee_names()
    out = ['; ' + u[addr][1]]
    for k in range(int(u[addr][4]) // 4):
        pc = addr + 4 * k
        w, = struct.unpack_from('<I', elf, pc - 0x100000 + 0x100)
        t = dis(w, pc)
        if w >> 26 == 3:  # jal: the callee's name (from the prototype's symbols) where known
            t += '   ; ' + name.get((w & 0x3ffffff) << 2, '?')
        out.append('%08x  %s' % (pc, t))
    return '\n'.join(out)


def main(a):
    if a[:1] == ['--selftest']:
        return selftest()
    cc = a[a.index('--cc') + 1] if '--cc' in a else CC
    elf = open(a[a.index('--elf') + 1] if '--elf' in a else ELF, 'rb').read()
    if a[0] == '--asm':
        print(asm(int(a[1], 16), elf))
        return 0
    if a[0] == '--all':
        u, bad = units(), []
        srcs = sorted(f for f in os.listdir(HDR) if re.fullmatch(r'[0-9a-f]{8}\.cpp', f))
        for f in srcs:
            r = u.get(int(f[:8], 16))
            if not r:
                bad.append(f)
                print(f, 'not in units.tsv')
                continue
            rc, text = check(os.path.join(HDR, f), r[1], int(r[3], 16), int(r[4]), elf, cc)
            if rc:
                bad.append(f)
                print('%s %s: %s' % (f, r[1], text.splitlines()[0][:120]))
        print('%d of %d match' % (len(srcs) - len(bad), len(srcs)))
        return 1 if bad else 0
    if len(a) == 1 or a[1].startswith('--'):
        r = units()[int(os.path.basename(a[0])[:8], 16)]
        name, addr, size = r[1], int(r[3], 16), int(r[4])
    else:
        name, addr, size = a[1], int(a[2], 16), int(a[3])
    rc, text = check(a[0], name, addr, size, elf, cc)
    print(text)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
