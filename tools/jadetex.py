#!/usr/bin/env python3
"""Reads textures from Prince of Persia: The Sands of Time bigfiles (Jade BIG v0x25), PS2 and PC.

  jadetex.py unpack <prince.bf> <key> <out.bin>         decompress one bigfile entry (world pack)
  jadetex.py list <pack.bin>                            list the texture records in a world pack
  jadetex.py png <pack.bin> <key> <out.png> [ps2|pc]    decode one texture to PNG
  jadetex.py compare <ps2.bf> <pc.bf> <key> <outdir> [n]
                                                        side-by-side PNGs (PS2 scaled up | PC) for the
                                                        textures of world pack <key> that are larger on PC

Formats (see CLAUDE.md, "Texture formats"):
  bigfile: u32 file count at 0x08, (offset, key) pairs from 0x44; an entry is u32 size + data.
           World packs are 0x20000-byte chunks: u32 size, u32 compressed size, LZO1X data
           (stored raw when the sizes match).
  pack:    records of u32 size, u32 0xEEFFC099, u32 key, size bytes.
  texture: +0x04 0xFFFFFFFF, +0x08 u16 format, +0x0C/+0x0E u16 width/height, +0x18 0xCAD01234.
           PS2 (format 3): 8-bit indices, palette record key at +0x3C. Both the indices and the
           palette record carry a GS upload packet; the data follows the GIF IMAGE tag. Indices
           are stored as the GS lays out PSMT8 data uploaded as PSMCT32; the palette is in CSM1
           order with alpha 0x80 = opaque.
           PC (format 0xB): +0x24 header variant (3: one extra word, data at +0x3C; else +0x38),
           +0x28 pixel format (0 BGRA32, 5/11 DXT1, 7 DXT5), mip levels follow the top level.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzo1x import decompress  # noqa: E402

RECORD_MARKER = 0xEEFFC099
TEXTURE_MAGIC = bytes.fromhex('3412d0ca')


def big_entry(path, key):
    with open(path, 'rb') as f:
        head = f.read(0x44)
        count, = struct.unpack_from('<I', head, 8)
        table = f.read(count * 8)
        for i in range(count):
            off, k = struct.unpack_from('<II', table, i * 8)
            if k == key:
                f.seek(off)
                size, = struct.unpack('<I', f.read(4))
                return f.read(size)
    raise KeyError(f'key {key:08x} not in {path}')


def unpack_chunks(blob):
    out = bytearray()
    p = 0
    while p + 8 <= len(blob):
        usize, csize = struct.unpack_from('<II', blob, p)
        p += 8
        if usize == 0:
            break
        chunk = blob[p:p + csize]
        p += csize
        out += chunk if csize == usize else decompress(bytes(chunk))
    return bytes(out)


def records(pack):
    out = {}
    p = 0
    while p + 12 <= len(pack):
        size, marker, key = struct.unpack_from('<III', pack, p)
        if marker != RECORD_MARKER:
            raise ValueError(f'record marker missing at {p:#x}')
        out[key] = (p + 12, size)
        p += 12 + size
    return out


def is_texture(pack, off):
    return pack[off + 4:off + 8] == b'\xff\xff\xff\xff' and pack[off + 0x18:off + 0x1c] == TEXTURE_MAGIC


def png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def image_data(d, start, end, nbytes):
    for p in range(start, end - 16, 4):
        tag, = struct.unpack_from('<Q', d, p)
        if (tag >> 58) & 3 == 2 and (tag & 0x7FFF) * 16 == nbytes:
            return p + 16
    raise ValueError('no GIF IMAGE tag')


def unswizzle8(buf, w, h):
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            block = (y & ~0xf) * w + (x & ~0xf) * 2
            swap = (((y + 2) >> 2) & 1) * 4
            py = (((y & ~3) >> 1) + (y & 1)) & 7
            col = py * w * 2 + ((x + swap) & 7) * 4
            byte = ((y >> 1) & 1) + ((x >> 2) & 2)
            out[y * w + x] = buf[block + col + byte]
    return out


def ps2_texture(pack, recs, off):
    w, h = struct.unpack_from('<HH', pack, off + 12)
    pal_key, = struct.unpack_from('<I', pack, off + 0x3c)
    po, ps = recs[pal_key]
    pp = image_data(pack, po, po + ps, 1024)
    pal = pack[pp:pp + 1024]
    dp = image_data(pack, off, off + 0x200, w * h)
    idx = unswizzle8(pack[dp:dp + w * h], w, h)
    out = bytearray(w * h * 4)
    for i, c in enumerate(idx):
        c = (c & 0xE7) | ((c & 0x08) << 1) | ((c & 0x10) >> 1)
        r, g, b, a = pal[c * 4:c * 4 + 4]
        out[i * 4:i * 4 + 4] = bytes([r, g, b, min(255, a * 2)])
    return w, h, out


def rgb565(v):
    return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31


def color_block(data, p, opaque_four):
    c0, c1, bits = struct.unpack_from('<HHI', data, p)
    r0, r1 = rgb565(c0), rgb565(c1)
    if c0 > c1 or opaque_four:
        cols = [r0 + (255,), r1 + (255,), tuple((2 * x + y) // 3 for x, y in zip(r0, r1)) + (255,),
                tuple((x + 2 * y) // 3 for x, y in zip(r0, r1)) + (255,)]
    else:
        cols = [r0 + (255,), r1 + (255,), tuple((x + y) // 2 for x, y in zip(r0, r1)) + (255,), (0, 0, 0, 0)]
    return [cols[(bits >> (2 * j)) & 3] for j in range(16)]


def dxt(data, w, h, alpha):
    out = bytearray(w * h * 4)
    p = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            alphas = None
            if alpha:
                a0, a1 = data[p], data[p + 1]
                ab = int.from_bytes(data[p + 2:p + 8], 'little')
                table = [a0, a1] + ([((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)] if a0 > a1 else
                                    [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255])
                alphas = [table[(ab >> (3 * j)) & 7] for j in range(16)]
                p += 8
            cols = color_block(data, p, alpha)
            p += 8
            for j in range(16):
                x, y = bx + j % 4, by + j // 4
                if x < w and y < h:
                    c = cols[j]
                    q = (y * w + x) * 4
                    out[q:q + 4] = bytes([c[0], c[1], c[2], alphas[j] if alphas else c[3]])
    return out


def pc_texture(pack, off):
    w, h = struct.unpack_from('<HH', pack, off + 12)
    variant, fmt = struct.unpack_from('<II', pack, off + 0x24)
    px = pack[off + (0x3c if variant == 3 else 0x38):]
    if fmt == 7:
        return w, h, dxt(px, w, h, True)
    if fmt in (5, 11):
        return w, h, dxt(px, w, h, False)
    if fmt == 0:
        out = bytearray(w * h * 4)
        for i in range(w * h):
            b, g, r, a = px[i * 4:i * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes([r, g, b, 255])
        return w, h, out
    raise ValueError(f'PC pixel format {fmt}')


def ps2_lightmap(pack, off, size):
    """Lightmap page (texture record format 6): a PSMT4 (4-bit) texture the game uploads as a
    PSMCT32 image (GIF A+D BITBLTBUF/TRXREG, then an IMAGE packet, inside the record) and reads
    back with a 16-step greyscale CLUT (index i -> grey 17*i). Rebuilds the GS swizzle in a small
    memory model (layout as ps2xRuntime; checked identical to the game's GS memory). Returns
    (w, h, RGBA bytes) with the brightness in RGB and alpha 255."""
    import gslm  # GS address layout (PSMT4)
    import gsvram  # GS address layout (PSMCT32)
    w, h = struct.unpack_from('<HH', pack, off + 12)
    regs, q, img = {}, off, None
    while q + 16 <= off + min(size, 0x400):
        lo, hi = struct.unpack_from('<QQ', pack, q)
        nloop, flg, nreg = lo & 0x7FFF, (lo >> 58) & 3, ((lo >> 60) & 0xF) or 16
        if flg == 0 and nreg == 1 and (hi & 0xF) == 0xE and 0 < nloop < 16:
            for k in range(nloop):
                val, a = struct.unpack_from('<QQ', pack, q + 16 + 16 * k)
                regs[a & 0xFF] = val
            q += 16 + 16 * nloop
            continue
        if flg == 2 and 0 < nloop * 16 <= size:
            img = pack[q + 16:q + 16 + 16 * nloop]
            break
        q += 16
    if img is None:
        raise ValueError('no IMAGE packet in lightmap record')
    dbw = (regs.get(0x50, 0) >> 48) & 0x3F or max(1, w // 128)
    cw = regs.get(0x52, 0) & 0xFFF or 128
    vram = bytearray(4 << 20)
    for i in range(len(img) // 4):
        y, x = divmod(i, cw)
        a = gsvram.addr(0, dbw, x, y)
        vram[a:a + 4] = img[4 * i:4 * i + 4]
    tbw = max(1, w // 64)
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            n = gslm.addr4(0, tbw, x, y)
            b = vram[n >> 1]
            v = 17 * ((b >> 4) if n & 1 else (b & 0xF))
            out[4 * (y * w + x):4 * (y * w + x) + 4] = bytes((v, v, v, 255))
    return w, h, out


def decode(pack, recs, key, kind):
    off, size = recs[key]
    if kind == 'ps2' and struct.unpack_from('<H', pack, off + 8)[0] == 6:
        return ps2_lightmap(pack, off, size)
    return ps2_texture(pack, recs, off) if kind == 'ps2' else pc_texture(pack, off)


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ''
    if cmd == 'unpack':
        data = unpack_chunks(big_entry(argv[2], int(argv[3], 16)))
        open(argv[4], 'wb').write(data)
        print(f'{len(data)} bytes')
    elif cmd == 'list':
        pack = open(argv[2], 'rb').read()
        for key, (off, size) in records(pack).items():
            if is_texture(pack, off):
                fmt, = struct.unpack_from('<H', pack, off + 8)
                w, h = struct.unpack_from('<HH', pack, off + 12)
                print(f'{key:08x} format {fmt:#x} {w}x{h} {size} bytes')
    elif cmd == 'png':
        pack = open(argv[2], 'rb').read()
        w, h, px = decode(pack, records(pack), int(argv[3], 16), argv[5] if len(argv) > 5 else 'ps2')
        png(argv[4], w, h, px)
    elif cmd == 'compare':
        key = int(argv[4], 16)
        a = unpack_chunks(big_entry(argv[2], key))
        b = unpack_chunks(big_entry(argv[3], key))
        ra, rb = records(a), records(b)
        os.makedirs(argv[5], exist_ok=True)
        limit = int(argv[6]) if len(argv) > 6 else 10
        done = 0
        for k, (o, _) in ra.items():
            if done >= limit:
                break
            if k not in rb or not is_texture(a, o) or not is_texture(b, rb[k][0]):
                continue
            w1, h1 = struct.unpack_from('<HH', a, o + 12)
            w2, h2 = struct.unpack_from('<HH', b, rb[k][0] + 12)
            if w2 * h2 <= w1 * h1:
                continue
            try:
                _, _, p1 = ps2_texture(a, ra, o)
                _, _, p2 = pc_texture(b, rb[k][0])
            except (ValueError, KeyError) as e:
                print(f'skip {k:08x}: {e}')
                continue
            width = w2 * 2 + 8
            out = bytearray(width * h2 * 4)
            for y in range(h2):
                for x in range(w2):
                    s1 = ((y * h1 // h2) * w1 + x * w1 // w2) * 4
                    out[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes(p1[s1:s1 + 3]) + b'\xff'
                    s2 = (y * w2 + x) * 4
                    q = (y * width + x + w2 + 8) * 4
                    out[q:q + 4] = bytes(p2[s2:s2 + 3]) + b'\xff'
            png(os.path.join(argv[5], f'{k:08x}_{w1}x{h1}_vs_{w2}x{h2}.png'), width, h2, out)
            done += 1
        print(f'{done} comparisons written')
    else:
        print(__doc__)


if __name__ == '__main__':
    main(sys.argv)
