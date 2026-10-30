"""Census of the 2D pass over save states (docs/native_2d.md): per save, the kinds in draw list +0xB8 (draw manager
0x700000C0; entries of 16 bytes at *list, count at list + 4, kind = (type byte +6 & 0x3F) >> 2) and the GFX object
types in GFX_Render's list (head at **0x674D04; node: type byte +0, next +0x14). Saves with the same file name are
counted once.

Usage: python3 -I tools/b8census.py [STATE ...]   (default: every game/analysis/**/*.state)
"""
import glob
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402


def census(s):
    kinds = Counter()
    ents, cnt = s.u32(0x700000C0 + 0xB8), s.u32(0x700000C0 + 0xBC)
    for i in range(min(cnt, 512)):
        t = s.mem(ents + 16 * i + 6, 1)
        if not t:
            break
        kinds[f'k{(t[0] & 0x3F) >> 2}'] += 1
    gfx, head = Counter(), s.u32(0x674D04)
    node = s.u32(head) if head else 0
    for _ in range(200):
        b = s.mem(node, 1) if node else b''
        if not b:
            break
        gfx[f'{b[0]:#x}'] += 1
        node = s.u32(node + 0x14)
    return kinds, gfx


def main(argv):
    paths = argv or sorted(glob.glob('game/analysis/**/*.state', recursive=True))
    seen, kt, gt = set(), Counter(), Counter()
    for p in paths:
        if os.path.basename(p) in seen:
            continue
        seen.add(os.path.basename(p))
        try:
            s = State(p)
        except ValueError:
            continue
        kinds, gfx = census(s)
        kt.update(kinds.keys())
        gt.update(gfx.keys())
        print(p, dict(kinds), dict(gfx))
    print('saves', len(seen))
    print('+0xB8 kinds (saves holding):', dict(kt))
    print('GFX types (saves holding):', dict(gt))


if __name__ == '__main__':
    main(sys.argv[1:])
