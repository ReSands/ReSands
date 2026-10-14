"""Compares a frame's captured DMA chain (override POP_DMACAP) with our conversion (jade2gltf).

Usage: python3 -I tools/dmacmp.py <chain.bin> <unpacked pack(s).bin> [<conversion.gltf>]

Walks the chain in order, keeping the last object matrix uploaded (UNPACK V4-32 x4 at VU address
0 in a tag's VIF codes, data = 4 rows of 4 floats), and for every positions upload (UNPACK V3-32)
looks up which geometry record / mesh element / batch those exact bytes belong to in our decoding.
Reports: draws matched to our geometry, draws we cannot identify, and, if a glTF from
`jade2gltf --place` is given, whether the game's matrix for each matched draw equals one of our
placements of that geometry.
"""
import json
import math
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import jade2gltf  # noqa: E402


def chain_records(path):
    c = open(path, 'rb').read()
    p = 0
    while p + 28 <= len(c):
        pc, t0, t1, t2, t3, da, sz = struct.unpack_from('<7I', c, p)
        yield pc, (t0, t1, t2, t3), da, c[p + 28:p + 28 + sz]
        p += 28 + sz


def vif_unpacks(tag, data):
    """(format, num, vu addr, payload bytes) for UNPACKs in the tag's two VIF codes (payload = the
    tag's data) and inline in the data itself."""
    out = []
    for w in (tag[2], tag[3]):
        cmd = (w >> 24) & 0x7F
        if cmd >= 0x60:
            f, num = cmd & 0xF, ((w >> 16) & 0xFF) or 256
            out.append((f, num, w & 0x3FF, data))
    # inline VIF codes inside cnt/call data
    v = 0
    while v + 4 <= len(data):
        w = struct.unpack_from('<I', data, v)[0]
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        if cmd >= 0x60 and (cmd & 0xF) in jade2gltf.EL:
            f = cmd & 0xF
            n = num or 256
            nb = (jade2gltf.EL[f] * n + 3) // 4 * 4
            out.append((f, n, imm & 0x3FF, data[v + 4:v + 4 + nb]))
            v += 4 + nb
        else:
            v += 4 + ({0x20: 4, 0x30: 16, 0x31: 16}.get(cmd, 0))
            if cmd in (0x50, 0x51):
                break  # GIF data follows: stop
    return out


def main(argv):
    chain_path, pack_path = argv[0], argv[1]
    gltf = json.load(open(argv[2])) if len(argv) > 2 else None
    d = open(pack_path, 'rb').read()
    # index: first 24 bytes of each decoded batch's positions -> (record, entry, batch)
    index = {}
    for key, o, _s, _mat, entry, _chain in jade2gltf.capture_elements(d):
        nb, lods, passes = jade2gltf.element_parts(d, o)
        if not passes:
            continue
        for bi, b in enumerate(jade2gltf.decode_packet(d, passes[0][1], passes[0][2], {x: (lo, ls) for x, lo, ls in lods})):
            if 8 in b:
                index.setdefault(b[8][1][:24], (key, entry, bi))
    placements = defaultdict(list)
    if gltf:
        for n in gltf['nodes']:
            if 'matrix' in n and 'mesh' in n:
                geo = int(gltf['meshes'][n['mesh']]['name'].split('/')[0], 16) if '/' in gltf['meshes'][n['mesh']]['name'] else None
                if geo is not None:
                    placements[geo].append((n['name'], n['matrix']))
    matrix = None
    draws = matched = unknown = 0
    per_geo = Counter()
    unknown_examples = []
    disagree = []
    agree = 0
    for pc, tag, da, data in chain_records(chain_path):
        for f, num, addr, payload in vif_unpacks(tag, data):
            if f == 0xC and num == 4 and addr == 0 and len(payload) >= 64:
                matrix = list(struct.unpack_from('<16f', payload, 0))
            elif f == 8:
                draws += 1
                hit = index.get(payload[:24])
                if hit:
                    matched += 1
                    per_geo[hit[0]] += 1
                    if gltf and matrix and placements.get(hit[0]):
                        best = min(placements[hit[0]], key=lambda nm: max(abs(a - b) for a, b in zip(nm[1][:15], matrix[:15])))
                        diff = max(abs(a - b) for a, b in zip(best[1][:15], matrix[:15]))
                        if diff < 1e-3:
                            agree += 1
                        elif len(disagree) < 12:
                            disagree.append((hit[0], best[0], round(diff, 3), [round(x, 2) for x in matrix[12:15]], [round(x, 2) for x in best[1][12:15]]))
                else:
                    unknown += 1
                    if len(unknown_examples) < 8:
                        unknown_examples.append((hex(da), num, [round(x, 2) for x in struct.unpack_from('<3f', payload, 0)] if len(payload) >= 12 else None))
    print(f'positions uploads in the frame: {draws}; matched to our geometry: {matched}; unidentified: {unknown}')
    print('geometry records drawn (record: batches):', ', '.join(f'{k:08x}:{v}' for k, v in per_geo.most_common(20)))
    if unknown_examples:
        print('unidentified examples (data address, vertices, first position):', unknown_examples)
    if gltf:
        print(f'matched draws whose game matrix equals one of our placements: {agree}; differing: {matched - agree}')
        for g, name, diff, game_t, ours_t in disagree:
            print(f'  geometry {g:08x} ({name}): max element difference {diff}; game translation {game_t}, ours {ours_t}')


if __name__ == '__main__':
    main(sys.argv[1:])
