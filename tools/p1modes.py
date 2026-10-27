"""VU1 program P1 (static world, 0x4CDE8C) batch modes in a save's last DMA chain (renderer audit item 9).

P1 picks a vertex path by VU 17.w & 0xFF: modes 0/1/2/4 copy the packet's V4-8 colour to the GS unchanged (alpha
too), mode 3 (lightmaps, ITOF15 UVs) writes RGB = VU 21 and alpha = clamp(c + d * w, lo, hi) with VU 16 = (hi, lo, c, d).
Per save: P1 batch runs (MSCALF 0x196 or MSCNT) by (mode, any packet colour alpha > 0x80), and mode 3's VU 16.
Usage: python3 -I tools/p1modes.py STATE [STATE ...]
"""
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402
from statechain import chain_words  # noqa: E402
from vifwalk import UNPACK_BYTES  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P1 = open(os.path.join(ROOT, 'game/analysis/vu1/prog_4cde80.bin'), 'rb').read()[:16]


def f32(w):
    return round(struct.unpack('<f', struct.pack('<I', w))[0], 3)


def census(s):
    g = 0x70000620
    it = chain_words(s, s.u32(g + 0xE4 + 4 * (s.u32(g + 0xF4) ^ 1)))
    vu, resident, over, modes, fade = {}, False, 0, Counter(), Counter()
    for _, w in it:
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        n = num or 256
        if cmd >= 0x60:
            pay = [next(it)[1] for _ in range((UNPACK_BYTES.get(cmd & 0xF, 4) * n + 3) // 4)]
            if cmd & 0xF == 0xC and not imm & 0x8000:  # V4-32 to an absolute address (headers; STCYCL 1:1 assumed)
                for k in range(n):
                    vu[(imm & 0x3FF) + k] = pay[4 * k:4 * k + 4]
            elif cmd & 0xF == 0xE:  # V4-8 colours
                b = struct.pack('<%dI' % len(pay), *pay)
                over += sum(b[4 * k + 3] > 0x80 for k in range(n))
        elif cmd == 0x4A:
            pay = [next(it)[1] for _ in range(n * 2)]
            if imm == 0:
                resident = struct.pack('<4I', *pay[:4]) == P1
        elif cmd in (0x50, 0x51):
            for _ in range((imm or 65536) * 4):
                next(it)
        elif cmd in (0x20, 0x30, 0x31):
            for _ in range(1 if cmd == 0x20 else 4):
                next(it)
        elif cmd in (0x14, 0x15, 0x17):
            if resident and (cmd == 0x17 or imm == 0x196) and 17 in vu:
                mode = vu[17][3] & 0xFF
                mode = mode if mode <= 4 else 'untracked'  # VU 17 not written by an absolute V4-32 unpack we follow
                modes[(mode, over > 0)] += 1
                if mode == 3:
                    fade[tuple(f32(x) for x in vu.get(16, [0] * 4))] += 1
            over = 0
    return modes, fade


def main(argv):
    total = Counter()
    for path in argv:
        modes, fade = census(State(path))
        total.update(modes)
        print(os.path.basename(path), dict(sorted(modes.items(), key=str)), 'mode 3 VU16 (hi, lo, c, d):', dict(fade))
    print('total (mode, alpha > 0x80):', dict(sorted(total.items(), key=str)))


if __name__ == '__main__':
    main(sys.argv[1:])
