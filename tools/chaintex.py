"""Maps GS texture addresses to bigfile texture keys for one captured frame (POP_DMACAP chain):
every IMAGE upload in the chain is identified by its bytes among the pack's texture records, and
its destination is the last BITBLTBUF (A+D register 0x50, DBP) set before it.

Usage: python3 -I tools/chaintex.py <chain.bin> <unpacked pack(s).bin>  -> <chain.bin>.tbpkeys.json
"""
import json
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import dmacmp  # noqa: E402
import jadetex  # noqa: E402


def gif_packets(tag, data):
    """GIF data carried by DIRECT VIF codes: in the tag's own VIF codes (data = the GIF stream) or
    inside the data, which is then itself a stream of VIF codes."""
    if any(((w >> 24) & 0x7F) in (0x50, 0x51) for w in (tag[2], tag[3])):
        yield data
        return
    v = 0
    while v + 4 <= len(data):
        w = struct.unpack_from('<I', data, v)[0]
        cmd = (w >> 24) & 0x7F
        if cmd in (0x50, 0x51):
            n = (w & 0xFFFF) or 65536
            # DIRECT data is quadword aligned
            start = (v + 4 + 15) // 16 * 16
            yield data[start:start + 16 * n]
            v = start + 16 * n
        elif cmd >= 0x60:
            f = cmd & 0xF
            n = ((w >> 16) & 0xFF) or 256
            el = {0: 4, 1: 2, 2: 1, 4: 8, 5: 4, 6: 2, 8: 12, 9: 6, 0xA: 3, 0xC: 16, 0xD: 8, 0xE: 4, 0xF: 2}.get(f, 4)
            v += 4 + (el * n + 3) // 4 * 4
        else:
            v += 4 + {0x20: 4, 0x30: 16, 0x31: 16}.get(cmd, 0)


def uploads(chain):
    dbp = None
    for pc, tag, da, data in dmacmp.chain_records(chain):
        for gif in gif_packets(tag, data):
            q = 0
            while q + 16 <= len(gif):
                lo, hi = struct.unpack_from('<QQ', gif, q)
                nloop, flg, nreg = lo & 0x7FFF, (lo >> 58) & 3, ((lo >> 60) & 0xF) or 16
                q += 16
                if flg == 0 and nreg == 1 and (hi & 0xF) == 0xE:
                    for _ in range(nloop):
                        if q + 16 > len(gif):
                            break
                        val, addr = struct.unpack_from('<QQ', gif, q)
                        q += 16
                        if addr & 0xFF == 0x50:
                            dbp = (val >> 32) & 0x3FFF
                elif flg == 2:
                    yield dbp, gif[q:q + 16 * nloop]
                    q += 16 * nloop
                else:
                    break


def main(argv):
    d = open(argv[1], 'rb').read()
    recs = jadetex.records(d)
    tex = {k: (o, s) for k, (o, s) in recs.items() if s > 0x40 and jadetex.is_texture(d, o)}
    out, n, found = {}, 0, 0
    for dbp, img in uploads(argv[0]):
        n += 1
        if dbp is None or len(img) < 64:
            continue
        for k, (o, s) in tex.items():
            if d.find(img[:64], o, o + s) >= 0:
                out[dbp] = k
                found += 1
                break
    json.dump({str(a): k for a, k in out.items()}, open(argv[0] + '.tbpkeys.json', 'w'))
    print(f'{n} image uploads, {found} identified; {len(out)} GS addresses mapped -> {argv[0]}.tbpkeys.json')


if __name__ == '__main__':
    main(sys.argv[1:])
