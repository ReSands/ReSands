"""Material records: multi-materials (first word 4: list of sub-material keys) and sub-materials
(first word 5, 48 bytes: texture key, blend / alpha-test state as the game draws them)."""
import glob
SOURCES = [('jadetex', ['RECORD_MARKER', 'records'])]
REPLACE = []
NOTES = """- The input is a whole unpacked world pack (dict order: first position of a key, last value)."""


def inputs(root):
    return sorted(glob.glob(root + '/game/analysis/meshdecode/tests/*.bin'))


# --- canon
def canon(data):
    recs = records(data)
    out = []
    for key in sorted(recs):
        o, s = recs[key]
        if s < 12:
            continue
        kind = struct.unpack_from('<I', data, o)[0]
        if kind == 4:
            n = struct.unpack_from('<I', data, o + 8)[0]
            if 0 < n <= 64 and 0xC + 4 * n <= s:
                subs = [struct.unpack_from('<I', data, o + 0xC + 4 * i)[0] for i in range(n)]
                out.append('multi %08x %s' % (key, ' '.join('%08x' % k for k in subs)))
        elif kind == 5 and s == 48:
            f14, f1c, f28, tex = struct.unpack_from('<I', data, o + 0x14)[0], struct.unpack_from('<I', data, o + 0x1C)[0], \
                struct.unpack_from('<I', data, o + 0x28)[0], struct.unpack_from('<I', data, o + 0x2C)[0]
            # +0x14 bit 0x80: alpha blending; else +0x1C bit 0x10: alpha test, AREF = (+0x28 top byte) / 2
            mode = 'BLEND' if f14 & 0x80 else 'MASK' if f1c & 0x10 else 'OPAQUE'
            out.append('sub %08x tex %08x %s aref %d' % (key, tex, mode, (f28 >> 24) // 2 if mode == 'MASK' else 0))
    return '\n'.join(out) + '\n'
