"""Water packets (VU1 program P6) in a save's last VIF1 chain: the GS register writes of each DIRECT (FRAME_1, TEST_1,
ALPHA_1, TEX0_1, XYOFFSET_1, and the primitives it draws) and the P6 runs between them, as one line per event
(docs/native_renderer.md "Water packets": mask, refraction texture, colour). Only refracting waters are listed: from
the mask pass's alpha-only FRAME write to the colour pass's last P6 run (a water without a mask pass would be left
out; none in the 95 Mac saves with P6 runs). --sig prints one pass signature per water instead, to compare the saves.
Usage: python3 -I tools/p6packets.py ELF STATE [--sig]
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402
from p5packets import vif_codes  # noqa: E402
from vu1progs import find_programs  # noqa: E402

REGS = {0x4C: 'FRAME', 0x47: 'TEST', 0x42: 'ALPHA', 0x06: 'TEX0', 0x18: 'XYOFS', 0x4E: 'ZBUF'}
PRIMS = ['point', 'line', 'lstrip', 'tri', 'tstrip', 'tfan', 'sprite', '?']


def gif_writes(words):
    """(register name, value) writes and primitive counts of a DIRECT's GIF packets."""
    data = struct.pack('<%dI' % len(words), *words)
    out, q, n = [], 0, len(data) // 16
    while q < n:
        lo, hi = struct.unpack_from('<QQ', data, q * 16)
        q += 1
        nloop, flg, nreg = lo & 0x7FFF, (lo >> 58) & 3, (lo >> 60) & 0xF or 16
        if (lo >> 46) & 1:
            out.append(('PRIM', PRIMS[(lo >> 47) & 7]))
        if flg == 0:
            for _ in range(nloop):
                for r in range(nreg):
                    reg = (hi >> (4 * r)) & 0xF
                    v, a = struct.unpack_from('<QQ', data, q * 16)
                    if reg == 0xE and (a & 0xFF) in REGS:
                        out.append((REGS[a & 0xFF], v))
                    elif reg in (4, 5):
                        out.append(('VTX', 1))
                    q += 1
        elif flg == 1:
            q += (nloop * nreg + 1) // 2
        else:
            q += nloop
        if (lo >> 15) & 1:
            break
    return out


def events(s, p6):
    code = bytearray(s.vu1code)
    for w, data in vif_codes(s):
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        if cmd == 0x4A:
            b = struct.pack('<%dI' % len(data), *data)
            code[imm * 8:imm * 8 + len(b)] = b
        elif cmd in (0x14, 0x15, 0x17) and code[:len(p6)] == p6:
            yield 'P6', 'cnt' if cmd == 0x17 else '%04x' % (imm * 8)
        elif cmd in (0x50, 0x51):
            yield 'DIRECT', gif_writes(data)


def describe(writes):
    parts, verts, prim = [], 0, None
    for name, v in writes:
        if name == 'VTX':
            verts += 1
            continue
        if verts:
            parts.append('%s x%d' % (prim, verts))
            verts = 0
        if name == 'PRIM':
            prim = v
        elif name == 'FRAME':
            parts.append('FRAME fbp %x msk %08x' % (v & 0x1FF, v >> 32))
        elif name == 'TEX0':
            parts.append('TEX0 tbp %x tw %d th %d' % (v & 0x3FFF, (v >> 26) & 0xF, (v >> 30) & 0xF))
        else:
            parts.append('%s %x' % (name, v))
    if verts:
        parts.append('%s x%d' % (prim, verts))
    return ', '.join(parts)


def main(argv):
    p6 = [c for v, a, c in find_programs(open(argv[0], 'rb').read()) if a == 0 and v == 0x4DAC8C][0]
    s = State(argv[1])
    ev = list(events(s, p6))
    runs = [i for i, (k, _) in enumerate(ev) if k == 'P6']
    if not runs:
        print('no P6 runs')
        return
    # a water: from the mask pass's alpha-only FRAME write to the last P6 run after the colour pass's RGB-only one
    waters, cur, colour = [], None, False
    for k, v in ev:
        text = k + ' ' + (describe(v) if k == 'DIRECT' else v)
        if k == 'DIRECT' and 'msk 00ffffff' in text and (cur is None or colour or not any(t.startswith('P6') for t, _ in cur)):
            if cur and colour:
                waters.append(cur)
            cur, colour = [], False
        if cur is None:
            continue
        if 'msk ff000000' in text:
            colour = True
        if colour and k != 'P6' and cur[-1][0].startswith('P6') and 'msk' not in text and 'TEX0' not in text:
            waters.append(cur)  # the colour pass's runs are over
            cur, colour = None, False
            continue
        if cur and cur[-1][0] == text:
            cur[-1][1] += 1
        else:
            cur.append([text, 1])
    if cur and colour:
        waters.append(cur)
    waters = [w for w in waters if sum(t.startswith('P6') for t, _ in w) >= 2]
    for n, w in enumerate(waters):
        if '--sig' in argv:
            print('water %d: %s' % (n, ' / '.join(re.sub(r' x\d+', '', t) + (' x%d' % c if c > 1 else '') for t, c in w)))
            continue
        print('water %d' % n)
        for t, c in w:
            print('  ' + t + (' (x%d)' % c if c > 1 else ''))


if __name__ == '__main__':
    main(sys.argv[1:])
