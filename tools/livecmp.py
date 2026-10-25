"""Compares native frames with the game's own GS pictures (live window comparison mode: the viewer's
--live-shots writes <name>_live_<n>.png and <name>_live_<n>_gs1.png). Pure Python (no PIL).

  livecmp.py score <native.png> <gs.png>                  mean |difference| per channel, share > 24
  livecmp.py side <native.png> <gs.png> <out.png>         native | GS | |difference| x 4
  livecmp.py crop x0,y0,x1,y1,zoom <out.png> <a.png> ...   the same crop of each image, zoomed, side by side
  livecmp.py mean x0,y0,x1,y1 <a.png> ...                 mean brightness of a region in each image
"""
import struct
import sys
import zlib


def read_png(path):
    """8-bit RGB or RGBA PNG -> (width, height, RGB bytes)."""
    d = open(path, 'rb').read()
    i, idat, w, h, ct = 8, b'', 0, 0, 2
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        if t == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', c[:10])
            if bd != 8 or ct not in (2, 6):
                raise SystemExit('%s: only 8-bit RGB/RGBA PNGs' % path)
        elif t == b'IDAT':
            idat += c
        i += 12 + n
    bpp = 3 if ct == 2 else 4
    raw, st = zlib.decompress(idat), w * bpp
    out, prev, o = bytearray(), bytearray(st), 0
    for _ in range(h):
        f = raw[o]
        o += 1
        line = bytearray(raw[o:o + st])
        o += st
        for x in range(st):
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
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        out += line if bpp == 3 else bytes(v for k, v in enumerate(line) if k % 4 != 3)
    return w, h, out


def write_png(path, w, h, rgb):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    rows = b''.join(b'\x00' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    cmd = argv[0]
    if cmd == 'score':
        w, h, a = read_png(argv[1])
        _, _, b = read_png(argv[2])
        # letterbox bars (2D, black in the game picture) are not the native frame's: score the rows between them
        black = lambda y: max(b[y * w * 3:(y + 1) * w * 3]) <= 8
        y0, y1 = 0, h
        while y0 < h and black(y0):
            y0 += 1
        while y1 > y0 and black(y1 - 1):
            y1 -= 1
        if y1 - y0 < h // 2:  # mostly black (a fade): score it all
            y0, y1 = 0, h
        d = [abs(x - y) for x, y in zip(a[y0 * w * 3:y1 * w * 3], b[y0 * w * 3:y1 * w * 3])]
        bars = ', rows %d-%d (letterbox)' % (y0, y1 - 1) if (y0, y1) != (0, h) else ''
        print('mean difference %.2f, %.1f%% of values off by > 24, max %d%s' % (sum(d) / len(d), 100 * sum(1 for x in d if x > 24) / len(d), max(d), bars))
    elif cmd == 'side':
        w, h, a = read_png(argv[1])
        _, _, b = read_png(argv[2])
        out = bytearray()
        for y in range(h):
            out += a[y * w * 3:(y + 1) * w * 3] + b[y * w * 3:(y + 1) * w * 3]
            for x in range(w):
                i = (y * w + x) * 3
                v = min(255, 4 * max(abs(a[i + k] - b[i + k]) for k in range(3)))
                out += bytes((v, v, v))
        write_png(argv[3], 3 * w, h, out)
    elif cmd == 'crop':
        x0, y0, x1, y1, z = map(int, argv[1].split(','))
        imgs = [read_png(p) for p in argv[3:]]
        W, H = (x1 - x0) * z, (y1 - y0) * z
        out = bytearray()
        for Y in range(H):
            for (w, h, px) in imgs:
                y = y0 + Y // z
                for X in range(W):
                    i = (y * w + x0 + X // z) * 3
                    out += px[i:i + 3]
        write_png(argv[2], W * len(imgs), H, out)
    elif cmd == 'mean':
        x0, y0, x1, y1 = map(int, argv[1].split(','))
        for p in argv[2:]:
            w, h, px = read_png(p)
            s = sum(sum(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) / 3 for y in range(y0, y1) for x in range(x0, x1))
            print('%s %.1f' % (p.split('/')[-1], s / ((x1 - x0) * (y1 - y0))))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
