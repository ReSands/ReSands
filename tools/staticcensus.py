"""Census of the static draws in save states: every CJMeshElement (no bone palette) and CJMeshElementRef
entry of the draw manager's lists, grouped by the upload layout of its pass packet (the VIF UNPACKs per
batch: format@VU address), with draw counts, lists, passes and an example. Audits what the native
static world (renderer step 1) meets: e.g. lit static meshes (V4-8 normals at 0x83 + V4-32 positions at
0x03, the Prince's sword) next to the plain static layout (V4-8 colours at 0x83 + V3-32 positions).

  python3 -I tools/staticcensus.py <state or dir> [...] [--elements]

--elements also lists the elements of each non-plain layout (save, element, list, pass, matrix position).
Lists and entry layout as tools/native/src/state_frame.cpp replays them; save layout tools/popstate.py.
"""
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402

DRAW_MANAGER = 0x700000C0
LISTS = (200, 0x2C, 0x3C, 0x68, 0x78, 0x4C, 0x88, 0x98, 0xA8)  # +0x5C (lightmaps) has its own layout
FORMATS = {0x0: 'S-32', 0x1: 'S-16', 0x2: 'S-8', 0x4: 'V2-32', 0x5: 'V2-16', 0x6: 'V2-8', 0x8: 'V3-32', 0x9: 'V3-16',
           0xA: 'V3-8', 0xC: 'V4-32', 0xD: 'V4-16', 0xE: 'V4-8', 0xF: 'V4-5'}
ELEMENT_BYTES = {0x0: 4, 0x1: 2, 0x2: 1, 0x4: 8, 0x5: 4, 0x6: 2, 0x8: 12, 0x9: 6, 0xA: 3, 0xC: 16, 0xD: 8, 0xE: 4, 0xF: 2}


def vif_words(s, packet):
    """The VIF stream of a DMA chain in guest memory (cnt/next/ref/refs, ending at ret/end/refe/call)."""
    words, pc = [], packet
    for _ in range(4096):
        t0, t1 = s.u32(pc), s.u32(pc + 4) & 0x7FFFFFF0
        qwc, tag = t0 & 0xFFFF, (t0 >> 28) & 7
        words += [s.u32(pc + 8), s.u32(pc + 12)]
        data, nxt, stop = pc + 16, pc + 16 + qwc * 16, False
        if tag == 0:
            data, stop = t1, True
        elif tag == 2:
            nxt = t1
        elif tag in (3, 4):
            data, nxt = t1, pc + 16
        elif tag != 1:
            stop = True
        if qwc:
            words += struct.unpack(f'<{qwc * 4}I', s.mem(data, qwc * 16))
        if stop:
            break
        pc = nxt
    return words


def layout(words, w4=None):
    """The packet's batch layouts: each batch = the UNPACKs up to the next MSCAL/MSCNT, as 'fmt@addr'.
    w4: a set that collects the fourth bytes of V4-8 uploads to VU 0x83 (colour alpha ~0x7F/0x80, or a
    normal's spare byte)."""
    batches, cur, i = [], [], 0
    while i < len(words):
        w = words[i]
        cmd = (w >> 24) & 0x7F
        if cmd >= 0x60:
            fmt = (cmd >> 0) & 0xF
            num = ((w >> 16) & 0xFF) or 256
            n = (ELEMENT_BYTES.get(fmt, 4) * num + 3) // 4
            cur.append(f'{FORMATS.get(fmt, hex(fmt))}@{w & 0x3FF:x}')
            if w4 is not None and fmt == 0xE and (w & 0x3FF) == 0x83:
                w4.update((x >> 24) & 0xFF for x in words[i + 1:i + 1 + n])
            i += 1 + n
            continue
        if cmd in (0x14, 0x15, 0x17):  # MSCAL, MSCALF, MSCNT: the batch is sent
            if cur:
                batches.append(' '.join(sorted(cur)))
            cur = []
        i += 1 + {0x20: 1, 0x30: 4, 0x31: 4}.get(cmd, 0)
        if cmd == 0x50 or cmd == 0x51:  # DIRECT/DIRECTHL: skip the GIF data
            i += (w & 0xFFFF) * 4
    if cur:
        batches.append(' '.join(sorted(cur)) + ' (no kick)')
    return batches


def scan(path, table, elements):
    s = State(path)
    pool = s.u32(0x70000260)
    name = os.path.basename(path)
    for off in LISTS:
        lst = DRAW_MANAGER + off
        entries, count = s.u32(lst), s.u32(lst + 4)
        for i in range(min(count, 4096)):
            e = entries + 16 * i
            word = struct.unpack('<Q', s.mem(e, 8))[0]
            kind = (s.mem(e + 6, 1)[0] & 0x3F) >> 2
            if kind not in (9, 10):
                continue
            matrix = (word & 0x3FFFFFFFFF) >> 28
            pas = s.mem(e + 0xD, 1)[0] & 0xF
            node = ((word & 0xFFFFFFFF) * 16) & 0x1FFFFFFF
            at = s.floats(pool + matrix * 0x40 + 0x30, 3)
            for _ in range(64):  # the chain: CJMeshElement +0x4C, CJMeshElementRef +0x1C
                if not node:
                    break
                if kind == 10:
                    element = s.u32(node + 0x20)
                    packet = s.u32(node + 4 * (0 if pas == 4 else pas)) & 0x01FFFFF0
                    nxt = s.u32(node + 0x1C)
                else:
                    element = node
                    packet = s.u32(node + 0x20 + 4 * (1 if pas == 4 else pas)) & 0x01FFFFF0 if pas < 8 else 0
                    nxt = s.u32(node + 0x4C)
                palette = s.u32(element + 0x48)
                if palette:
                    break  # a skinned chain head: the character path, not the static world
                if not packet:
                    break  # Display draws nothing and does not follow the chain
                first = (s.u32(packet) >> 28) & 7
                style = 'inline' if first in (1, 6, 7) else 'ref'
                w4 = set()
                lay = layout(vif_words(s, packet), w4)
                # what the V4-8 at 0x83 holds: colours (fourth byte = alpha, 0x7F/0x80 almost always) or
                # normals (a spare fourth byte, 0/1 in the sword's)
                at83 = '' if not w4 else ' 0x83=colours' if w4 <= {0x7F, 0x80} else ' 0x83=other(' + ','.join(f'{b:x}' for b in sorted(w4)[:6]) + ')'
                key = ('ref ' if kind == 10 else 'mesh', style, f'pass {pas}: ' + (' | '.join(sorted(set(lay))) or '(none)') + at83)
                t = table[key]
                t['draws'] += 1
                t['elements'].add((name, element))
                t['lists'].add(f'+0x{off:x}')
                t['passes'].add(pas)
                t['saves'].add(name)
                if elements is not None:
                    elements[key].append((name, element, off, pas, matrix, at))
                node = nxt


def main(argv):
    show = '--elements' in argv
    paths = []
    for a in (a for a in argv[1:] if not a.startswith('--')):
        paths += sorted(os.path.join(a, f) for f in os.listdir(a) if f.endswith('.state')) if os.path.isdir(a) else [a]
    table = defaultdict(lambda: {'draws': 0, 'elements': set(), 'lists': set(), 'passes': set(), 'saves': set()})
    elements = defaultdict(list) if show else None
    for p in paths:
        try:
            scan(p, table, elements)
        except Exception as ex:  # a damaged or foreign save: report, go on
            print(f'{p}: {ex}', file=sys.stderr)
    print(f'{len(paths)} saves; static draws per layout (draws, distinct elements, saves, lists, passes)')
    for key, t in sorted(table.items(), key=lambda kv: -kv[1]['draws']):
        print(f"{t['draws']:7d} {len({e for _, e in t['elements']}):5d} {len(t['saves']):4d}  {key[0]} {key[1]:6s} "
              f"lists {','.join(sorted(t['lists']))} passes {sorted(t['passes'])}\n          {key[2]}")
        if show and not ('V3-32@3' in key[2] and 'V4-32@3' not in key[2]):
            seen = set()
            for name, element, off, pas, matrix, at in elements[key]:
                if element in seen:
                    continue
                seen.add(element)
                print(f'            {name} el {element:08x} list +0x{off:x} pass {pas} matrix {matrix} at ({at[0]:.1f} {at[1]:.1f} {at[2]:.1f})')
                if len(seen) >= 12:
                    print('            ...')
                    break


if __name__ == '__main__':
    main(sys.argv)
