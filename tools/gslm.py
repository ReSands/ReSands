"""Rebuilds GS memory from a captured frame's DMA chain (POP_DMACAP) and decodes the textures the
frame draws with, e.g. the 4-bit lightmap pages (PSMT4) that the game uploads as PSMCT32 images.

Usage: python3 -I tools/gslm.py <chain.bin> <TBP0 hex> <out.png>
  Replays every IMAGE transfer in the chain (BITBLTBUF/TRXPOS/TRXREG, PSMCT32 destinations) into a
  4 MB GS memory model, finds the TEX0 the frame draws with for that TBP0, and decodes it with its
  CLUT. GS address layout mirrors ps2xRuntime (gsvram.addr for PSMCT32; addrPSMT4 below).
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import chaintex  # noqa: E402
import dmacmp  # noqa: E402
import gsvram  # noqa: E402

BLOCK4 = [[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15],
          [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]]


COLUMN4 = [
    [0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110],
    [16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126],
    [65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47],
    [81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63],
    [192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174],
    [208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190],
    [129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239],
    [145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255],
    [256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366],
    [272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382],
    [321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303],
    [337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319],
    [448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430],
    [464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446],
    [385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495],
    [401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511],
]


def addr4(block, width, x, y):
    """Nibble address of a PSMT4 texel (mirrors GSPSMT4::addrPSMT4)."""
    ppr = (width >> 1) or 1
    page = (block >> 5) + (y >> 7) * ppr + (x >> 7)
    block_id = (block & 0x1F) + BLOCK4[(y >> 4) & 7][(x >> 5) & 3]
    return ((page << 14) + ((block_id >> 5) << 14) + (block_id & 0x1F) * 512 + COLUMN4[y & 15][x & 31]) % (8 << 20)


def build_vram(chain):
    """4 MB GS memory after all IMAGE transfers of the chain (PSMCT32 destinations), plus every
    TEX0 value the frame sets."""
    vram = bytearray(4 << 20)
    regs, tex0s, skipped = {}, [], 0
    for pc, tag, da, data in dmacmp.chain_records(chain):
        for gif in chaintex.gif_packets(tag, data):
            q = 0
            while q + 16 <= len(gif):
                lo, hi = struct.unpack_from('<QQ', gif, q)
                nloop, flg, nreg = lo & 0x7FFF, (lo >> 58) & 3, ((lo >> 60) & 0xF) or 16
                q += 16
                if flg == 0 and nreg == 1 and (hi & 0xF) == 0xE:
                    for _ in range(nloop):
                        if q + 16 > len(gif):
                            break
                        val, a = struct.unpack_from('<QQ', gif, q)
                        q += 16
                        regs[a & 0xFF] = val
                        if a & 0xFF in (0x06, 0x07):
                            tex0s.append(val)
                elif flg == 2:
                    img = gif[q:q + 16 * nloop]
                    q += 16 * nloop
                    b, pos, reg = regs.get(0x50, 0), regs.get(0x51, 0), regs.get(0x52, 0)
                    dbp, dbw, dpsm = (b >> 32) & 0x3FFF, (b >> 48) & 0x3F, (b >> 56) & 0x3F
                    dx, dy = (pos >> 32) & 0x7FF, (pos >> 48) & 0x7FF
                    w, h = reg & 0xFFF, (reg >> 32) & 0xFFF
                    if dpsm != 0 or not w:
                        skipped += 1
                        continue
                    for i in range(min(len(img) // 4, w * h)):
                        y, x = divmod(i, w)
                        o = gsvram.addr(dbp, dbw, dx + x, dy + y)
                        vram[o:o + 4] = img[4 * i:4 * i + 4]
                else:
                    break
    return vram, tex0s, skipped


def decode_psmt4(vram, tex0):
    tbp, tbw, psm = tex0 & 0x3FFF, (tex0 >> 14) & 0x3F, (tex0 >> 20) & 0x3F
    tw, th = 1 << ((tex0 >> 26) & 0xF), 1 << ((tex0 >> 30) & 0xF)
    cbp, cpsm, csm, csa = (tex0 >> 37) & 0x3FFF, (tex0 >> 51) & 0xF, (tex0 >> 55) & 1, (tex0 >> 56) & 0x1F
    if psm != 0x14:
        raise ValueError(f'PSM {psm:#x} is not PSMT4')
    # 16-entry PSMCT32 CLUT, CSM1: an 8x2 rectangle at CBP (CSA selects a 16-entry slot)
    clut = []
    for i in range(16):
        x, y = i % 8, i // 8
        o = gsvram.addr(cbp, 1, x + (csa & 1) * 8 * 0, y + (csa >> 1) * 2 * 0)
        clut.append(vram[o:o + 4])
    rows = []
    for y in range(th):
        row = bytearray()
        for x in range(tw):
            n = addr4(tbp, tbw, x, y)
            byte = vram[(n >> 1) % (4 << 20)]
            idx = (byte >> 4) if n & 1 else (byte & 0xF)
            r, g, b, a = clut[idx]
            row += bytes((r, g, b))
        rows.append(bytes(row))
    return tw, th, rows, (tbp, tbw, cbp, cpsm, csm, csa)


def main(argv):
    chain, tbp_want, out = argv[0], int(argv[1], 16), argv[2]
    vram, tex0s, skipped = build_vram(chain)
    cands = [t for t in tex0s if t & 0x3FFF == tbp_want]
    if not cands:
        print(f'no TEX0 with TBP0 {tbp_want:#x} in the frame ({len(tex0s)} TEX0 writes)')
        return 1
    tw, th, rows, info = decode_psmt4(vram, cands[0])
    gsvram.png(out, tw, th, rows)
    print(f'TEX0 {cands[0]:#018x}: {tw}x{th} PSMT4, TBW {info[1]}, CLUT at {info[2]:#x} (CPSM {info[3]}, CSM {info[4]}, CSA {info[5]}); '
          f'{skipped} non-PSMCT32 uploads skipped -> {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
