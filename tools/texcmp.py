"""Compares, per mesh element, the texture the game drew with (from a captured DMA chain) with the
texture jade2gltf assigns (material index -> multi-material -> sub-material -> texture key).

Usage: python3 -I tools/texcmp.py <chain.bin> <texture log> <unpacked pack(s).bin> [record key ...]
  chain.bin    one kick of POP_DMACAP (the main 3D one, several MB)
  texture map  the frame capture section (POP_FRAMECAP: K/T lines), or the run's "[pop:tex] tbp=... key=..." lines (POP_TEXLOG=1, live registration: start
               from a save state before the level loads), up to the capture
The game sends GS registers before each draw as A+D GIF packets in DIRECT VIF codes; TEX0 (0x06)
gives the texture's GS block address (TBP0), which the log maps to a texture key.
"""
import json
import re
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import chaintex  # noqa: E402
import dmacmp  # noqa: E402
import jade2gltf  # noqa: E402
import jadetex  # noqa: E402


def gif_ad(tag, data):
    """(register, value) of A+D GIF packets carried by DIRECT VIF codes (in the tag or the data)."""
    out = []
    for gif in chaintex.gif_packets(tag, data):
        q = 0
        while q + 16 <= len(gif):
            lo, hi = struct.unpack_from('<QQ', gif, q)
            nloop, flg, nreg = lo & 0x7FFF, (lo >> 58) & 3, ((lo >> 60) & 0xF) or 16
            q += 16
            if flg == 2:
                q += 16 * nloop
                continue
            if flg != 0 or nreg != 1 or (hi & 0xF) != 0xE:
                break
            for _ in range(nloop):
                if q + 16 > len(gif):
                    break
                val, addr = struct.unpack_from('<QQ', gif, q)
                out.append((addr & 0xFF, val))
                q += 16
    return out


def main(argv):
    chain, texlog, pack = argv[0], argv[1], argv[2]
    only = {int(k, 16) for k in argv[3:]}
    tbp_key = {}
    if texlog.endswith('.json'):  # tools/chaintex.py: GS address -> key, identified by upload content
        tbp_key = {int(a): k for a, k in json.load(open(texlog)).items()}
        lines = []
    else:
        lines = open(texlog).read().splitlines()
    if any(l.startswith('T ') for l in lines):
        # POP_FRAMECAP dump of the texture manager at the captured frame: "K index key" (registered
        # keys) and "T index gsaddr ..." (slots): exact mapping for that frame
        keys = {int(l.split()[1]): int(l.split()[2], 16) for l in lines if l.startswith('K ')}
        for l in lines:
            if l.startswith('T '):
                ix, tbp = int(l.split()[1]), int(l.split()[2], 16)
                if ix in keys and tbp:
                    tbp_key[tbp] = keys[ix]
    else:
        for line in lines:
            if 'frame captured (2)' in line:
                break
            m = re.search(r'tbp=0x([0-9a-f]+) key=([0-9a-f]+)', line)
            if m:
                tbp_key[int(m.group(1), 16)] = int(m.group(2), 16)
    d = open(pack, 'rb').read()
    recs = jadetex.records(d)
    index = {}
    for key, o, _s, mat, entry, _chain in jade2gltf.capture_elements(d):
        nb, lods, passes = jade2gltf.element_parts(d, o)
        for pidx, po, ps in passes:
            for b in jade2gltf.decode_packet(d, po, ps, {x: (lo, ls) for x, lo, ls in lods}):
                if 8 in b:
                    index.setdefault(b[8][1][:24], (key, entry, mat, pidx))
    # our texture per (geometry, material index), via the game objects' multi-materials
    geos = {k for k, (o, s) in recs.items() if s >= 8 and struct.unpack_from('<I', d, o)[0] == 1}
    ours = defaultdict(set)
    for name, m, geo, mm, *_ in jade2gltf.game_objects(d, recs, geos):
        if mm in recs and struct.unpack_from('<I', d, recs[mm][0])[0] == 4:
            n = struct.unpack_from('<I', d, recs[mm][0] + 8)[0]
            for i in range(n):
                sub = struct.unpack_from('<I', d, recs[mm][0] + 0xC + 4 * i)[0]
                if sub in recs and struct.unpack_from('<I', d, recs[sub][0])[0] == 5:
                    ours[(geo, i)].add(struct.unpack_from('<I', d, recs[sub][0] + 0x2C)[0])
    tex0 = None
    game = defaultdict(Counter)
    for pc, tag, da, data in dmacmp.chain_records(chain):
        for reg, val in gif_ad(tag, data):
            if reg in (0x06, 0x07):
                tex0 = val
        for f, num, addr, payload in dmacmp.vif_unpacks(tag, data):
            if f == 8:
                hit = index.get(payload[:24])
                if hit and (not only or hit[0] in only) and tex0 is not None:
                    tbp = tex0 & 0x3FFF
                    game[hit][tbp_key.get(tbp, 'tbp %#x' % tbp)] += 1
    same = differ = 0
    for (geo, entry, mat, pidx), c in sorted(game.items()):
        o = ours.get((geo, mat), set())
        used = [k for k in c if isinstance(k, int)]
        verdict = 'same' if used and set(used) <= o else 'DIFFERENT'
        same += verdict == 'same'
        differ += verdict != 'same'
        if verdict != 'same' or only:
            print(f'{geo:08x} element {entry} pass {pidx} material {mat}: game drew with '
                  + ', '.join(f'{k:08x}' if isinstance(k, int) else k for k in c) + f'; ours {", ".join(f"{k:08x}" for k in sorted(o)) or "none"} -> {verdict}')
    print(f'{same} element passes use the same texture as the game, {differ} differ (texture log: {len(tbp_key)} GS addresses)')


if __name__ == '__main__':
    main(sys.argv[1:])
