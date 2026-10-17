"""Walks the last complete VIF1 DMA chain held in a save state's memory and sums the frame's work.

The graphics object (*(0x675148) = 0x70000620) keeps two packet buffers at +0xE4/+0xE8; +0xF4 is
the one being built, so the last kicked chain starts at the other (CGraphicStream::ClosePacketStream
0x4A1800, kick 0x4A2C80). Tags are walked in source-chain mode with TTE (the tag's upper 64 bits
are two VIF codes); VIF codes are parsed as in tools/vifwalk.py.

Usage: python3 -I tools/statechain.py STATE [STATE ...]   (one summary line per state)
"""
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402
from vifwalk import UNPACK_BYTES  # noqa: E402


def chain_words(s, start, limit=200000):
    """Yields (record index, word) over the chain from start (DMA source chain, TTE on)."""
    addr, stack = start, []
    for rec in range(limit):
        t0, t1, t2, t3 = struct.unpack('<4I', s.mem(addr, 16))
        qwc, tid, ta = t0 & 0xFFFF, (t0 >> 28) & 7, t1 & 0x7FFFFFF0
        yield rec, t2
        yield rec, t3
        if tid in (0, 3, 4):              # refe, ref, refs: data at ta
            data, nxt = ta, addr + 16
        else:                             # cnt, next, call, ret, end: data follows the tag
            data, nxt = addr + 16, addr + 16 + qwc * 16
        payload = s.mem(data, qwc * 16)
        for k in range(qwc * 4):
            yield rec, struct.unpack_from('<I', payload, 4 * k)[0]
        if tid in (0, 7):                 # refe, end
            return
        if tid == 2:                      # next
            addr = ta
        elif tid == 5:                    # call
            stack.append(nxt)
            addr = ta
        elif tid == 6:                    # ret
            if not stack:
                return
            addr = stack.pop()
        else:                             # cnt, ref, refs
            addr = nxt
    raise RuntimeError('chain too long')


def summarise(s):
    g = 0x70000620
    building = s.u32(g + 0xF4)
    start = s.u32(g + 0xE4 + 4 * (building ^ 1))
    c = Counter()
    progs = Counter()
    it = chain_words(s, start)
    last_rec = -1
    for rec, w in it:
        if rec != last_rec:
            c['records'] += 1
            last_rec = rec
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        if cmd >= 0x60:
            n = num or 256
            words = (UNPACK_BYTES.get(cmd & 0xF, 4) * n + 3) // 4
            c['unpack_qw'] += n
            if (cmd & 0xF) in (0x8, 0xC):  # V3-32 / V4-32: positions, matrices
                c['unpack_v32'] += n
        elif cmd == 0x4A:
            words = (num or 256) * 2
            c['mpg'] += 1
        elif cmd in (0x50, 0x51):
            words = (imm or 65536) * 4
            c['direct_qw'] += imm or 65536
        elif cmd == 0x20:
            words = 1
        elif cmd in (0x30, 0x31):
            words = 4
        else:
            words = 0
            if cmd in (0x14, 0x15, 0x17):
                c['vu1_runs'] += 1
                progs[imm if cmd != 0x17 else 'cnt'] += 1
        for _ in range(words):
            next(it, None)
        c['words'] += 1 + words
    return start, c, progs


def main(argv):
    for path in argv:
        s = State(path)
        start, c, progs = summarise(s)
        name = os.path.basename(path).replace('.state', '')
        print(f"{name} chain {start:08x} records {c['records']:5d} MB {c['words'] * 4 / 1e6:5.2f} "
              f"unpack {c['unpack_qw']:7d} v32 {c['unpack_v32']:7d} vu1 {c['vu1_runs']:5d} "
              f"direct {c['direct_qw']:6d} mpg {c['mpg']:3d}  top {progs.most_common(4)}")


if __name__ == '__main__':
    main(sys.argv[1:])
