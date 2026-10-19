"""Which of PoP's VU1 microprograms each save's last frame runs, and how often (+ the native draw paths).

Walks the save's last VIF1 chain (tools/statechain.py) keeping a model of VU1 code memory: it starts as the
save's VU1 code (the program left resident at the end of a frame is normally also resident at the start of
the next), MPG commands write into it, and each MSCAL/MSCALF/MSCNT is attributed to the ELF program
(tools/vu1progs.py) that the code memory then starts with. Programs are numbered by ELF order (P0..P9).
With --native DIR, also counts the draw paths in DIR/<save>.log (tools/native/score_saves.sh logs).

Usage: python3 -I tools/vu1inventory.py ELF [--native DIR] STATE [STATE ...]
"""
import os
import re
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402
from statechain import chain_words  # noqa: E402
from vifwalk import UNPACK_BYTES  # noqa: E402
from vu1progs import find_programs  # noqa: E402

PATHS = [('static/skinned', r'frame written .*?: (\d+) draws'), ('idxmesh', r'CJIdxMesh::Display: '),
         ('sprite', r'CJadeSpriteElement::Display: '), ('water', r'CJSmWaterElement::Display: '),
         ('type18', r'FUN_00469C00 \(list type 0x18\): '), ('gfx', r'GFX draws \(FUN_00458FC0.*?\): (\d+)'),
         ('lightmap', r'lightmap pass: (\d+) draws'), ('halo adds', r'halos: \d+ in the array at the frame.s end, (\d+) adds')]


def native_counts(log):
    text = open(log, errors='replace').read()
    out = {}
    for name, pat in PATHS:
        m = re.findall(pat, text)
        out[name] = sum(int(x) for x in m) if '(' in pat.replace(r'\(', '') else len(m)
    return out


def runs(s, progs):
    code = bytearray(s.vu1code)
    g = 0x70000620
    start = s.u32(g + 0xE4 + 4 * (s.u32(g + 0xF4) ^ 1))
    current, dirty = None, True
    count, mpg = Counter(), Counter()
    it = chain_words(s, start)
    for _rec, w in it:
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        if cmd >= 0x60:
            words = (UNPACK_BYTES.get(cmd & 0xF, 4) * (num or 256) + 3) // 4
        elif cmd == 0x4A:  # MPG: num doublewords of code at imm * 8
            n = num or 256
            data = bytearray()
            for _ in range(n * 2):
                data += next(it)[1].to_bytes(4, 'little')
            code[imm * 8:imm * 8 + len(data)] = data
            dirty = True
            mpg[imm * 8] += 1
            continue
        elif cmd in (0x50, 0x51):
            words = (imm or 65536) * 4
        elif cmd == 0x20:
            words = 1
        elif cmd in (0x30, 0x31):
            words = 4
        else:
            words = 0
            if cmd in (0x14, 0x15, 0x17):
                if dirty:
                    hits = [(len(c), i) for i, (_v, _a, c) in enumerate(progs) if code[:len(c)] == c]
                    current = 'P%d' % max(hits)[1] if hits else '?'
                    dirty = False
                count[(current, 'cnt' if cmd == 0x17 else '%04x' % (imm * 8))] += 1
        for _ in range(words):
            next(it, None)
    return count, mpg


def main(argv):
    native = None
    if '--native' in argv:
        native = argv[argv.index('--native') + 1]
        argv = [a for a in argv if a not in ('--native', native)]
    progs = [p for p in find_programs(open(argv[0], 'rb').read()) if p[1] == 0]
    for i, (vaddr, _a, c) in enumerate(progs):
        print('P%d = ELF 0x%06X, 0x%X bytes' % (i, vaddr, len(c)))
    total = Counter()
    for path in argv[1:]:
        name = os.path.basename(path).replace('.state', '')
        count, mpg = runs(State(path), progs)
        per = Counter()
        for (p, _e), n in count.items():
            per[p] += n
        total.update(per)
        line = '%s  runs %s  entries %s  mpg %d' % (name, ' '.join('%s:%d' % kv for kv in sorted(per.items())),
                                                    ' '.join('%s@%s:%d' % (p, e, n) for (p, e), n in sorted(count.items())),
                                                    sum(mpg.values()))
        if native and os.path.exists(os.path.join(native, name + '.log')):
            line += '  paths ' + ' '.join('%s:%d' % kv for kv in native_counts(os.path.join(native, name + '.log')).items())
        print(line)
    print('total runs ' + ' '.join('%s:%d' % kv for kv in sorted(total.items())))


if __name__ == '__main__':
    main(sys.argv[1:])
