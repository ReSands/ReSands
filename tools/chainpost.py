"""Lists the frame's 2D / post-processing passes: every GIF primitive sent by PATH2 (DIRECT VIF codes
in a POP_DMACAP chain), grouped into runs of the same GS state. Per run: target buffer (FRAME_1 FBP,
FBW, PSM), source texture (TEX0 TBP0, TBW, PSM, size), ALPHA, TEST, primitive type, count, colour
(RGBAQ), screen extent (XYZ, 1/16 px, minus the 2048 offset) and texture coordinate extent (UV).
Usage: python3 -I tools/chainpost.py <chain.bin> [max runs]
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import chaintex  # noqa: E402
import dmacmp  # noqa: E402

PRIMS = ['point', 'line', 'linestrip', 'tri', 'tristrip', 'trifan', 'sprite', '?']


def runs(chain_path):
    regs, out, cur = {}, [], None

    def state():
        f, t, a, te = regs.get(0x4C, 0), regs.get(0x06, 0), regs.get(0x42, 0), regs.get(0x47, 0)
        prim = regs.get(0x00, 0)
        return (f & 0x1FF, (f >> 16) & 0x3F, (f >> 24) & 0x3F,
                (t & 0x3FFF) if prim & 0x10 else None, (t >> 14) & 0x3F, (t >> 20) & 0x3F,
                1 << ((t >> 26) & 0xF), 1 << ((t >> 30) & 0xF), a & 0xFF, (a >> 32) & 0xFF, te & 0xFFFFF,
                PRIMS[prim & 7], bool(prim & 0x10), bool(prim & 0x40), bool(prim & 0x8))

    def vertex(x, y, u, v, rgba):
        nonlocal cur
        st = state()
        if cur is None or cur['state'] != st:
            cur = {'state': st, 'verts': 0, 'x': [1e9, -1e9], 'y': [1e9, -1e9], 'u': [1e9, -1e9], 'v': [1e9, -1e9], 'rgba': set()}
            out.append(cur)
        cur['verts'] += 1
        for k, val in (('x', x), ('y', y), ('u', u), ('v', v)):
            cur[k][0] = min(cur[k][0], val)
            cur[k][1] = max(cur[k][1], val)
        if len(cur['rgba']) < 4:
            cur['rgba'].add(rgba)

    for pc, tag, da, data in dmacmp.chain_records(chain_path):
        for gif in chaintex.gif_packets(tag, data):
            q = 0
            while q + 16 <= len(gif):
                lo, hi = struct.unpack_from('<QQ', gif, q)
                nloop, pre, prim, flg = lo & 0x7FFF, (lo >> 46) & 1, (lo >> 47) & 0x7FF, (lo >> 58) & 3
                nreg = ((lo >> 60) & 0xF) or 16
                q += 16
                if pre:
                    regs[0x00] = prim
                if flg == 2:  # IMAGE
                    q += 16 * nloop
                    continue
                if flg != 0:  # REGLIST: rare here, skip
                    q += 8 * nloop * nreg
                    q = (q + 15) // 16 * 16
                    continue
                descs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
                u = v = 0
                for _ in range(nloop):
                    for d in descs:
                        if q + 16 > len(gif):
                            break
                        a0, a1 = struct.unpack_from('<QQ', gif, q)
                        q += 16
                        if d == 0xE:
                            regs[a1 & 0xFF] = a0
                        elif d == 0x0:
                            regs[0x00] = a0 & 0x7FF
                        elif d == 0x1:
                            regs[0x01] = a0
                        elif d == 0x3:
                            u, v = (a0 & 0x3FFF) / 16.0, ((a0 >> 32) & 0x3FFF) / 16.0
                        elif d in (0x4, 0x5):
                            x, y = (a0 & 0xFFFF) / 16.0 - 2048, ((a0 >> 32) & 0xFFFF) / 16.0 - 2048
                            c = regs.get(0x01, 0)
                            vertex(x, y, u, v, (c & 0xFF, (c >> 32) & 0xFF, (c >> 64 if False else (a0 & 0)) | ((regs.get(0x01, 0) >> 64) & 0xFF) if False else (c >> 0) & 0, 0))
    return out


def main(argv):
    rs = runs(argv[0])
    lim = int(argv[1]) if len(argv) > 1 else 80
    print('%d runs' % len(rs))
    for r in rs[:lim]:
        (fbp, fbw, fpsm, tbp, tbw, tpsm, tw, th, alpha, fix, test, prim, tme, fst, abe) = r['state']
        print('FRAME %4d/%2d psm%02x | %s | ALPHA %02x FIX %02x ABE %d | %-8s x%3d | x %.0f..%.0f y %.0f..%.0f | uv %.0f..%.0f %.0f..%.0f' % (
            fbp, fbw, fpsm, ('TEX %4d/%d psm%02x %dx%d' % (tbp, tbw, tpsm, tw, th)) if tbp is not None else 'untextured        ',
            alpha, fix, abe, prim, r['verts'], *r['x'], *r['y'], *r['u'], *r['v']))


if __name__ == '__main__':
    main(sys.argv[1:])
