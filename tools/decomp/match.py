# match.py: compile one source file with CodeWarrior and compare one function with the retail bytes.
#   python match.py <src.cpp> <mangled name> <retail addr hex> <size> [--cc <compiler dir>] [--elf <SLUS_207.43>]
# Prints MATCH or a per-instruction diff (relocated fields masked on both sides, so call targets and
# addresses of globals only need the right relocation, not the final address). Exit 0 = match.
# Runs where mwccps2.exe runs (the PC); objdiff() alone also works on any machine with an object file.
import os, re, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mipsdis import dis

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CC = os.path.join(ROOT, 'game', 'decomp', 'compilers', '3.0.3-020716', 'mwccps2.exe')
FLAGS = ['-c', '-nostdinc', '-O4,p', '-enum', 'min']
ELF = os.path.join(ROOT, 'game', 'ps2', 'disc', 'SLUS_207.43')
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
    nw = max(len(code), size) // 4
    lines, same = [], 0
    for k in range(nw):
        a = struct.unpack_from('<I', want, 4 * k)[0] if 4 * k < size else None
        b = struct.unpack_from('<I', code, 4 * k)[0] if 4 * k < len(code) else None
        m = mask.get(k, (0xffffffff, None))
        ok = a is not None and b is not None and (a & m[0]) == (b & m[0])
        same += ok
        ta = dis(a, addr + 4 * k) if a is not None else '-'
        tb = dis(b, addr + 4 * k) if b is not None else '-'
        if m[1] and b is not None:
            tb += '   <%s>' % m[1]
        lines.append('%s %3x  %-34s | %s' % ('  ' if ok else '>>', 4 * k, ta, tb))
    matched = same == nw and len(code) == size
    head = 'MATCH' if matched else '%d of %d instructions differ (retail %d bytes, yours %d)' % (
        nw - same, nw, size, len(code))
    return matched, head + (' [compiled as %s]' % cand[0] if cand[0] != name else '') + '\n' + \
        '     off  retail                             | yours\n' + '\n'.join(lines)


def compile_src(src, out_obj, cc=CC):
    r = subprocess.run([cc] + FLAGS + ['-o', out_obj, os.path.basename(src)], cwd=os.path.dirname(os.path.abspath(src)),
                       capture_output=True, text=True, errors='replace', timeout=120)
    msg = (r.stdout + r.stderr).strip()
    return os.path.exists(out_obj) and r.returncode == 0, msg


def main(a):
    cc = a[a.index('--cc') + 1] if '--cc' in a else CC
    elf = open(a[a.index('--elf') + 1] if '--elf' in a else ELF, 'rb').read()
    src, name, addr, size = a[0], a[1], int(a[2], 16), int(a[3])
    obj = os.path.splitext(os.path.abspath(src))[0] + '.o'
    if os.path.exists(obj):
        os.remove(obj)
    ok, msg = compile_src(src, obj, cc)
    if not ok:
        print('COMPILE ERROR\n' + msg)
        return 2
    matched, text = objdiff(obj, name, addr, size, elf)
    print(text)
    return 0 if matched else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
