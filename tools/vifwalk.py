"""Walks a captured VIF1 DMA chain (POP_DMACAP file) as one VIF code stream, in order: every VIF code
with its parameters, the DMA record it came from, and which VU1 program (CALL target of the last MPG
packet) is resident. Usage:
  python3 -I tools/vifwalk.py <chain.bin> summary     MSCAL/MSCNT entry points per resident program
  python3 -I tools/vifwalk.py <chain.bin> trace N M   codes of DMA records N..M (unpacks summarised)
Record format: see tools/dmacmp.chain_records. The DMA tag's upper 64 bits are two VIF codes (TTE).
"""
import struct
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from dmacmp import chain_records  # noqa: E402

# UNPACK element sizes in bytes per element, by the low 4 bits of the command (vn << 2 | vl).
UNPACK_BYTES = {0x0: 4, 0x1: 2, 0x2: 1, 0x4: 8, 0x5: 4, 0x6: 2, 0x8: 12, 0x9: 6, 0xA: 3, 0xC: 16, 0xD: 8, 0xE: 4, 0xF: 2}
NAMES = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD', 0x06: 'MSKPATH3',
         0x07: 'MARK', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL', 0x15: 'MSCALF', 0x17: 'MSCNT',
         0x20: 'STMASK', 0x30: 'STROW', 0x31: 'STCOL', 0x4A: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}


def stream(path):
    """Yields (record index, record dma address, word) for the whole chain."""
    for i, (pc, tag, da, data) in enumerate(chain_records(path)):
        yield i, da, tag[2]
        yield i, da, tag[3]
        for k in range(len(data) // 4):
            yield i, da, struct.unpack_from('<I', data, 4 * k)[0]


def codes(path):
    """Yields (record index, record address, cmd, imm, num, payload words) per VIF code."""
    it = stream(path)
    for i, da, w in it:
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        if cmd >= 0x60:
            n = num or 256
            size = UNPACK_BYTES.get(cmd & 0xF, 4) * n
            words = (size + 3) // 4
        elif cmd == 0x4A:
            words = (num or 256) * 2
        elif cmd in (0x50, 0x51):
            words = (imm or 65536) * 4
        elif cmd == 0x20:
            words = 1
        elif cmd in (0x30, 0x31):
            words = 4
        else:
            words = 0
        payload = []
        for _ in range(words):
            try:
                payload.append(next(it)[2])
            except StopIteration:
                break
        yield i, da, cmd, imm, num, payload


def main(argv):
    path, mode = argv[0], argv[1] if len(argv) > 1 else 'summary'
    resident = None
    if mode == 'summary':
        calls = Counter()
        for i, da, cmd, imm, num, payload in codes(path):
            if cmd == 0x4A and imm == 0:
                resident = da
            elif cmd in (0x14, 0x15, 0x17):
                calls[(resident, NAMES[cmd], imm if cmd != 0x17 else None)] += 1
        for (prog, name, entry), n in sorted(calls.items(), key=lambda kv: (kv[0][0] or 0, kv[0][1], kv[0][2] or 0)):
            print('program %-10s %-6s entry %-6s x%d' % (hex(prog) if prog else '-', name, hex(entry) if entry is not None else '-', n))
    elif mode == 'trace':
        lo, hi = int(argv[2]), int(argv[3])
        for i, da, cmd, imm, num, payload in codes(path):
            if cmd == 0x4A and imm == 0:
                resident = da
            if not lo <= i <= hi:
                continue
            if cmd >= 0x60:
                print('%5d %08x UNPACK fmt %x num %d addr %#x%s' % (i, da, cmd & 0xF, num or 256, imm & 0x3FF,
                                                                    ' (+top)' if imm & 0x8000 else ''))
            elif cmd != 0:
                extra = ' %s' % ' '.join('%08x' % x for x in payload[:4]) if cmd in (0x20, 0x30, 0x31) else ''
                print('%5d %08x %-8s imm %#x num %d%s' % (i, da, NAMES.get(cmd, '?%02x' % cmd), imm, num, extra))


if __name__ == '__main__':
    main(sys.argv[1:])
