# objcmp.py <retail elf> <targets.txt> <out dir>: for every object in <out dir>/*/ compare each target function's
# code with the retail bytes; fields covered by a relocation in the object are masked on both sides.
# targets.txt lines: <symbol substring> <retail addr hex> <size>
import sys, struct, glob, os
R_MIPS_26 = 4

def sections(d):
    shoff, = struct.unpack_from('<I', d, 0x20)
    es, n, si = struct.unpack_from('<HHH', d, 0x2e)
    sh = [struct.unpack_from('<10I', d, shoff + i * es) for i in range(n)]
    strt = sh[si][4]
    name = lambda o: d[strt + o:d.index(b'\0', strt + o)].decode()
    return [(name(s[0]),) + s for s in sh]   # (name, sh_name, type, flags, addr, offset, size, link, info, ...)

def funcs(path):
    d = open(path, 'rb').read()
    sh = sections(d)
    symtab = next(s for s in sh if s[2] == 2)
    strs = sh[symtab[7]][5]
    out = {}
    for i in range(symtab[6] // 16):
        nm, val, size, info, oth, shn = struct.unpack_from('<IIIBBH', d, symtab[5] + i * 16)
        if info & 15 != 2 or shn == 0 or shn >= len(sh):
            continue
        name = d[strs + nm:d.index(b'\0', strs + nm)].decode(errors='replace')
        sec = sh[shn]
        code = d[sec[5] + val:sec[5] + val + size]
        mask = [0xffffffff] * (size // 4)
        for s in sh:   # REL / RELA sections against this one
            if s[2] in (9, 4) and s[8] == shn:
                ent = 8 if s[2] == 9 else 12
                for j in range(s[6] // ent):
                    off, rinfo = struct.unpack_from('<II', d, s[5] + j * ent)
                    k = (off - val) // 4
                    if 0 <= k < len(mask):
                        mask[k] = 0xfc000000 if (rinfo & 0xff) == R_MIPS_26 else 0xffff0000
        out[name] = (code, mask)
    return out

elf = open(sys.argv[1], 'rb').read()
targets = [l.split() for l in open(sys.argv[2]) if l.strip() and not l.startswith('#')]
for obj in sorted(glob.glob(os.path.join(sys.argv[3], '*', '*.o'))):
    try:
        fs = funcs(obj)
    except Exception as e:
        print(obj, 'unreadable', e)
        continue
    for sub, addr, size in targets:
        a, n = int(addr, 16), int(size)
        want = elf[a - 0x100000 + 0x100:][:n]
        for name, (code, mask) in fs.items():
            if sub not in name:
                continue
            words = max(len(code), n) // 4
            same = 0
            for k in range(min(len(code), n) // 4):
                w1, = struct.unpack_from('<I', code, 4 * k)
                w2, = struct.unpack_from('<I', want, 4 * k)
                if (w1 & mask[k]) == (w2 & mask[k]):
                    same += 1
            tag = 'MATCH' if same == words and len(code) == n else '%d/%d words, size %d vs %d' % (same, words, len(code), n)
            print('%-18s %-12s %-30s %s' % (os.path.basename(os.path.dirname(obj)), os.path.basename(obj), name[:30], tag))
