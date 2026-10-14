"""Replays records of the game's IMA ADPCM decoder (POP_CAPTURE_ADPCM, override capturedAdpcm)
through a plain reference decoder and reports where the recompiled decoder differs.

Usage: python3 -I tools/adpcm_replay.py <SLUS_207.43> <capture file>

The reference reads the game's own tables from the ELF (index table 0x4EB020, 16 words; step
table 0x4EB060, 89 words) and tries the usual IMA variants (shift-add or multiply delta, nibble
order), so a mismatch in every variant points at the recompiled code, not at our guess of the
format. State per channel: s16 predictor, s8 index (t0 + 0 / +2 / +4 / +6).
"""
import struct
import sys


def elf_reader(path):
    elf = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', elf, 0x1C)[0]
    phnum = struct.unpack_from('<H', elf, 0x2C)[0]
    segs = [struct.unpack_from('<8I', elf, phoff + 32 * i) for i in range(phnum)]

    def read(va, n):
        for t, off, v, _pa, fs, _ms, _fl, _al in segs:
            if t == 1 and v <= va < v + fs:
                return elf[off + va - v:off + va - v + n]
        raise ValueError(hex(va))
    return read


def records(path):
    data = open(path, 'rb').read()
    p = 0
    while p + 28 <= len(data):
        magic, n, mode = struct.unpack_from('<3I', data, p)
        if magic != 0x43504441:
            raise ValueError(f'bad record at {p:#x}')
        before, after = data[p + 12:p + 20], data[p + 20:p + 28]
        inp = data[p + 28:p + 28 + n]
        out = data[p + 28 + n:p + 28 + 5 * n]
        p += 28 + 5 * n
        yield n, mode, before, after, inp, out


class Ref:
    def __init__(self, idx_tab, step_tab, multiply):
        self.idx, self.step, self.mul = idx_tab, step_tab, multiply

    def nibble(self, st, code):
        pred, index = st
        step = self.step[index]
        if self.mul:
            diff = ((code & 7) * 2 + 1) * step >> 3
        else:
            diff = step >> 3
            if code & 4: diff += step
            if code & 2: diff += step >> 1
            if code & 1: diff += step >> 2
        pred = pred - diff if code & 8 else pred + diff
        pred = max(-32768, min(32767, pred))
        index = max(0, min(88, index + self.idx[code & 15]))
        return (pred, index), pred


def state(b):
    p0, i0, _x, p1, i1, _y = struct.unpack('<hbbhbb', b)
    return [(p0, i0), (p1, i1)]


def decode(ref, n, mode, before, inp, high_first):
    st = state(before)
    out = []
    for byte in inp:
        lo, hi = byte & 15, byte >> 4
        if mode == 1:  # stereo: one nibble per channel
            a, b = (hi, lo) if high_first else (lo, hi)
            st[0], s0 = ref.nibble(st[0], a)
            st[1], s1 = ref.nibble(st[1], b)
            out += [s0, s1]
        else:
            for code in ((hi, lo) if high_first else (lo, hi)):
                st[0], s = ref.nibble(st[0], code)
                out.append(s)
    return out, st


def main(argv):
    read = elf_reader(argv[0])
    idx_tab = struct.unpack('<16i', read(0x4EB020, 64))
    step_tab = struct.unpack('<89i', read(0x4EB060, 89 * 4))
    variants = {(m, h): Ref(idx_tab, step_tab, m) for m in (False, True) for h in (False, True)}
    totals = {k: [0, 0] for k in variants}  # samples equal, samples compared
    first_bad = {}
    count = 0
    for n, mode, before, after, inp, out in records(argv[1]):
        count += 1
        game = list(struct.unpack(f'<{(2 * n)}h', out[:4 * n]))
        for key, ref in variants.items():
            mine, _ = decode(ref, n, mode, before, inp, key[1])
            m = min(len(mine), len(game))
            eq = sum(1 for i in range(m) if mine[i] == game[i])
            totals[key][0] += eq
            totals[key][1] += m
            if eq != m and key not in first_bad:
                i = next(i for i in range(m) if mine[i] != game[i])
                first_bad[key] = (count, mode, n, i, mine[i:i + 6], game[i:i + 6], before.hex())
    print(f'{count} decoder calls')
    for (mul, high), (eq, tot) in totals.items():
        print(f'  delta {"multiply" if mul else "shift-add"}, {"high" if high else "low"} nibble first: '
              f'{eq}/{tot} samples equal ({100.0 * eq / max(tot, 1):.2f}%)')
    best = max(totals, key=lambda k: totals[k][0])
    if best in first_bad:
        c, mode, n, i, mine, game, st = first_bad[best]
        print(f'best variant first differs: call {c} (mode {mode}, {n} bytes, state {st}) at sample {i}:\n'
              f'  reference {mine}\n  game      {game}')
    else:
        print('best variant matches every sample: the recompiled decoder is correct')


if __name__ == '__main__':
    main(sys.argv[1:])
