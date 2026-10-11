"""Decode a PSMCT32 framebuffer from a raw GS VRAM dump (4 MB) into a PNG.

Mirrors GSPSMCT32::addrPSMCT32 in ps2xRuntime. FBP is in 2048-word units (32 blocks),
FBW in 64-pixel units (pages per row).

Usage: python3 -I tools/gsvram.py VRAM.bin FBP [FBW=10] [W=640] [H=448] OUT.png
       python3 -I tools/gsvram.py tex8 VRAM.bin TEX0 OUT.png   (PSMT8 texture + CLUT)
"""
import struct
import sys
import zlib

BLOCK = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23],
         [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15],
          [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
          [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47],
          [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]


def addr(block, width, x, y):
    page = (block >> 5) + (y >> 5) * width + (x >> 6)
    block_id = (block & 0x1F) + BLOCK[(y >> 3) & 3][(x >> 3) & 7]
    return ((page << 13) + ((block_id >> 5) << 13) + (block_id & 0x1F) * 256 + COLUMN[y & 7][x & 7] * 4) % (4 << 20)


COLUMN8 = [
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255]]


def addr8(block, width, x, y):
    """Mirrors GSPSMT8::addrPSMT8 (width in 64-pixel units, as TBW)."""
    ppr = max(width >> 1, 1)
    page = (block >> 5) + (y >> 6) * ppr + (x >> 7)
    block_id = (block & 0x1F) + BLOCK[(y >> 4) & 3][(x >> 4) & 7]
    return ((page << 13) + ((block_id >> 5) << 13) + (block_id & 0x1F) * 256 + COLUMN8[y & 15][x & 15]) % (4 << 20)


def tex8(vram, tex0, out):
    """Decode a PSMT8 texture with a CSM1 PSMCT32 CLUT, from a TEX0 value. Writes RGB and alpha PNGs."""
    tbp, tbw = tex0 & 0x3FFF, (tex0 >> 14) & 0x3F
    w, h = 1 << ((tex0 >> 26) & 0xF), 1 << ((tex0 >> 30) & 0xF)
    cbp = (tex0 >> 37) & 0x3FFF
    clut = []
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        a = addr(cbp, 1, j & 15, j >> 4)
        clut.append(vram[a:a + 4])
    rows, arows, alphas = [], [], set()
    for y in range(h):
        row, arow = bytearray(), bytearray()
        for x in range(w):
            c = clut[vram[addr8(tbp, tbw, x, y)]]
            row += c[:3]
            arow += bytes((min(c[3] * 2, 255),) * 3)
            alphas.add(c[3])
        rows.append(bytes(row))
        arows.append(bytes(arow))
    png(out, w, h, rows)
    png(out.replace(".png", "_alpha.png"), w, h, arows)
    print(f"PSMT8 tbp={tbp} tbw={tbw} {w}x{h} cbp={cbp}: alpha values {sorted(alphas)[:12]} -> {out}")


def png(path, w, h, rows):
    raw = b"".join(b"\0" + r for r in rows)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    if sys.argv[1] == "tex8":
        tex8(open(sys.argv[2], "rb").read(), int(sys.argv[3], 0), sys.argv[4])
        return
    vram, fbp = open(sys.argv[1], "rb").read(), int(sys.argv[2], 0)
    fbw = int(sys.argv[3], 0) if len(sys.argv) > 4 else 10
    w = int(sys.argv[4], 0) if len(sys.argv) > 5 else 640
    h = int(sys.argv[5], 0) if len(sys.argv) > 6 else 448
    out = sys.argv[-1]
    rows, nonzero = [], 0
    for y in range(h):
        row = bytearray()
        for x in range(w):
            a = addr(fbp * 32, fbw, x, y)
            r, g, b = vram[a], vram[a + 1], vram[a + 2]
            nonzero += (r | g | b) != 0
            row += bytes((r, g, b))
        rows.append(bytes(row))
    png(out, w, h, rows)
    print(f"FBP {fbp}: {nonzero} of {w * h} pixels non-black -> {out}")


if __name__ == "__main__":
    main()
