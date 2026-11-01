"""Step 4c point 1: do the matrices the save's last chain uploads for its draws still sit in the save's
matrix pools? Walks the last kicked chain (as statechain.py), collects per CALL the V4-32 matrices
uploaded since the previous CALL (VU 0 = object matrix, VU 0x388+ = bone palette), and looks each up
in the bone pool (*(0x700001D0)) and the object pool (*(0x70000260)) of the same save: exact bits,
else the nearest pool matrix by translation (distance in metres).

Usage: python3 -I tools/chainpose.py STATE [STATE ...] [-v]
"""
import math
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402
import dmacmp  # noqa: E402

POOL_CAP = 4096  # matrices scanned per pool (shortcut: fixed cap, raise it if 'exact' misses at the end of a pool)


def chain_records(s, start, limit=200000):
    """Yields (tag as 4 u32, data bytes) over the chain from start (DMA source chain, TTE on)."""
    addr, stack = start, []
    for _ in range(limit):
        tag = struct.unpack('<4I', s.mem(addr, 16))
        qwc, tid, ta = tag[0] & 0xFFFF, (tag[0] >> 28) & 7, tag[1] & 0x7FFFFFF0
        data, nxt = (ta, addr + 16) if tid in (0, 3, 4) else (addr + 16, addr + 16 + qwc * 16)
        yield tag, s.mem(data, qwc * 16)
        if tid in (0, 7):
            return
        if tid == 2:
            addr = ta
        elif tid == 5:
            stack.append(nxt)
            addr = ta
        elif tid == 6:
            if not stack:
                return
            addr = stack.pop()
        else:
            addr = nxt
    raise RuntimeError('chain too long')


def pool(s, base):
    """{raw 64 bytes: index}, [(index, translation)] for POOL_CAP matrices from base."""
    exact, trans = {}, []
    for i in range(POOL_CAP):
        raw = s.mem(base + 0x40 * i, 64)
        if len(raw) < 64:
            break
        m = struct.unpack('<16f', raw)
        if not all(math.isfinite(x) for x in m) or m[15] != 1.0:
            continue  # not an affine row-vector matrix
        exact.setdefault(raw, i)
        trans.append((i, m[12:15]))
    return exact, trans


def measure(path, verbose):
    s = State(path)
    g = 0x70000620
    start = s.u32(g + 0xE4 + 4 * (s.u32(g + 0xF4) ^ 1))
    bones, btr = pool(s, s.u32(0x700001D0))
    objs, otr = pool(s, s.u32(0x70000260))
    c, dists = Counter(), []
    fresh, call = [], 0
    for tag, data in chain_records(s, start):
        tid = (tag[0] >> 28) & 7
        if tid == 5:  # CALL: the matrices uploaded for this draw
            call += 1
            for vu, raw in fresh:
                kind = 'obj' if vu == 0 else 'bone'
                ex, tr = (objs, otr) if kind == 'obj' else (bones, btr)
                if raw in ex or raw in (bones if kind == 'obj' else objs):
                    c[kind + ' exact'] += 1
                    continue
                t = struct.unpack('<16f', raw)[12:15]
                d = min((math.dist(t, p) for _, p in btr + otr), default=float('inf'))
                c[kind + ' miss'] += 1
                dists.append(d)
                if verbose:
                    print(f'  call {call:4d} vu {vu:#05x} {kind} miss, nearest pool translation {d:.4f} m')
            fresh = []
        for f, n, a, p in dmacmp.vif_unpacks(tag, data):
            if f == 0xC and n % 4 == 0 and (a == 0 or a >= 0x388):
                for k in range(min(n // 4, len(p) // 64)):
                    m = struct.unpack_from('<16f', p, 64 * k)
                    if all(math.isfinite(x) for x in m) and m[15] == 1.0:  # VU constants are not matrices
                        fresh.append((a + 4 * k, p[64 * k:64 * k + 64]))
    dists.sort()
    med = dists[len(dists) // 2] if dists else 0
    name = os.path.basename(path).replace('.gs.state', '')
    print(f'{name} calls {call} {dict(sorted(c.items()))} miss nearest median {med:.4f} m max {max(dists, default=0):.4f} m')


def main(argv):
    verbose = '-v' in argv
    for path in (a for a in argv if a != '-v'):
        measure(path, verbose)


if __name__ == '__main__':
    main(sys.argv[1:])
