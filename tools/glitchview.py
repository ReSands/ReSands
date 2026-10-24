#!/usr/bin/env python3
"""Reads a glitch capture (<save>.glitch + <save>.glitch.txt, written at every kept quick-save with
POP_NATIVE_WORLD=1; tools/overrides/pop_native_world.cpp dumpRecent): the last ~10 s of native world pictures
(half size) with one record per frame. Writes every frame as PNG and a report that flags the frames where
something jumps: picture brightness / change from the frame before, camera (clip matrix), fog zones, missed
injections, purity flips (frames whose late lists switch between native and GS).

Usage: tools/glitchview.py <file.glitch> <out dir> [--every N]   (PNG of every Nth frame plus all flagged ones)
The pictures are the native world only: characters, effects and 2D are drawn by the GS on top of them.
"""
import os
import re
import struct
import sys
import zlib


def write_png(path, w, h, rgb):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    rows = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def parse_line(line):
    r = {'line': line.rstrip('\n')}
    m = re.match(r'frame (\d+) at ([\d.]+) ms: ([A-Za-z ]+), (\d+) draws', line)
    if m:
        r.update(seq=int(m.group(1)), ms=float(m.group(2)), state=m.group(3), draws=int(m.group(4)))
    m = re.search(r'; clip ((?:\S+ ){15}\S+);', line)
    r['clip'] = [float(x) for x in m.group(1).split()] if m else None
    r['fog'] = re.findall(r'\d:\(([^)]*)\)', line)
    r['pure'] = 'pure to the end' in line
    m = re.search(r'late native (\d+) GS (\d+)', line)
    r['late'] = (int(m.group(1)), int(m.group(2))) if m else None
    return r


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    every = int(argv[argv.index('--every') + 1]) if '--every' in argv else 10
    data = open(argv[0], 'rb').read()
    magic, version, n, w, h = struct.unpack_from('<5I', data)
    if magic != 0x4C474E50 or version != 1:
        raise SystemExit('not a glitch capture (PNGL v1)')
    size = w * h * 3
    lines = [parse_line(l) for l in open(argv[0] + '.txt', encoding='utf-8')]
    os.makedirs(argv[1], exist_ok=True)
    report, flagged, prev, prev_pic = [], set(), None, None
    for i in range(n):
        pic = data[20 + i * size:20 + (i + 1) * size]
        rec = lines[i] if i < len(lines) else {'line': '(no record)'}
        sample = pic[::31]
        mean = sum(sample) / len(sample)
        empty = all(b == 0 for b in pic[::997])
        change = sum(abs(a - b) for a, b in zip(sample, prev_pic[::31])) / len(sample) if prev_pic else 0.0
        why = []
        if rec.get('state') and rec['state'] != 'injected':
            why.append(rec['state'])
        elif empty:
            why.append('empty picture')
        if prev_pic and change > 12:
            why.append(f'picture changed {change:.1f}')
        if prev and rec.get('clip') and prev.get('clip'):
            d = max(abs(a - b) for a, b in zip(rec['clip'], prev['clip']))
            if d > 0.5:
                why.append(f'camera jump {d:.2f}')
        if prev and rec.get('fog') and prev.get('fog') and rec['fog'] != prev['fog']:
            changed = [k for k, (a, b) in enumerate(zip(rec['fog'], prev['fog'])) if a != b]
            why.append('fog zone ' + ','.join(map(str, changed)) + ' changed')
        if prev and 'pure' in rec and 'pure' in prev and rec['pure'] != prev['pure']:
            why.append('late lists now ' + ('native' if rec['pure'] else 'GS'))
        if any(not x.startswith(('fog zone', 'late lists')) for x in why):  # fog and purity flips: listed, no PNGs
            flagged.update(range(max(0, i - 2), min(n, i + 3)))
        report.append(f"{i:4d} mean {mean:6.1f} change {change:5.1f}  {'  <<< ' + '; '.join(why) if why else ''}\n      {rec['line']}\n")
        prev, prev_pic = rec, pic
    for i in range(n):
        if i % every == 0 or i in flagged:
            write_png(os.path.join(argv[1], f'f{i:04d}.png'), w, h, data[20 + i * size:20 + (i + 1) * size])
    with open(os.path.join(argv[1], 'report.txt'), 'w', encoding='utf-8') as f:
        f.writelines(report)
    print(f'{n} frames ({w}x{h}); {len(flagged)} frames around flagged ones; report and PNGs in {argv[1]}')


if __name__ == '__main__':
    main(sys.argv[1:])
