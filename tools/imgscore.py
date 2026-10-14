"""Scores rendered frames against a game frame: brightness correlation and mean ratio over a mask.

Usage: python3 -I tools/imgscore.py <game frame png> <render.png> [<render.png> ...]
The game frame from boot.ps1 -Shots holds the 640x480 game image in the bottom-left corner of the
capture; renders are 640x448 (GS frame), mapped by scaling y. Masked out: the runtime's overlay labels
(top), the HUD tutorial text and the Prince (bottom centre).
"""
import math
import struct
import sys
import zlib


def read_png(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('>II', d[16:24])
    ct = d[25]
    p, idat = 8, b''
    while p < len(d):
        n = struct.unpack('>I', d[p:p + 4])[0]
        if d[p + 4:p + 8] == b'IDAT':
            idat += d[p + 8:p + 8 + n]
        p += 12 + n
    raw = zlib.decompress(idat)
    bpp = {2: 3, 6: 4}[ct]
    stride = w * bpp
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bpp, bytes(out)


def lum(px, i, bpp):
    return 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]


def main(argv):
    gw, gh, gb, g = read_png(argv[0])
    y0 = gh - 480
    for path in argv[1:]:
        w, h, b, r = read_png(path)
        xs, ys = [], []
        for y in range(0, 448, 2):
            gy = int(y * 480 / 448)
            if gy < 100 or (380 <= gy):
                continue
            for x in range(0, 640, 2):
                if 260 <= x <= 380 and gy >= 280:
                    continue
                rv = lum(r, (y * w + x) * b, b)
                if rv < 2:  # background (no geometry) in the render
                    continue
                xs.append(rv)
                ys.append(lum(g, ((y0 + gy) * gw + x) * gb, gb))
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        cov = sum((a - mx) * (c - my) for a, c in zip(xs, ys))
        corr = cov / math.sqrt(sum((a - mx) ** 2 for a in xs) * sum((c - my) ** 2 for c in ys))
        print(f'{path.rsplit("/", 1)[-1]:22} correlation {corr:+.3f}  mean brightness ours {mx:6.1f} / game {my:6.1f} ({n} pixels)')


if __name__ == '__main__':
    main(sys.argv[1:])
