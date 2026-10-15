"""Skinned characters from a captured frame (POP_DMACAP chain): for every skinned draw, the called
packet is rebuilt from the chain records and decoded with jade2gltf.skinned_batches, and posed with
the matrices the game uploaded to VU1 just before the CALL: layout A (multi-bone) gets its bone
palette at VU 0x388 (4 qwords per bone, bone slot k -> matrix k-1), layout B (single-bone /
normal-lit) its object matrix at VU 0. Matrices are as the game sends them (row vectors:
world = [x y z 1] * M). Skinned UVs are at 1/512 (raw 0..512).

Usage: python3 -I tools/chainskin.py <chain.bin> <out_posed.gltf> [<out_bind.gltf>] [--near X,Y,Z,R] [--textures]
  --near keeps only draws whose posed vertices lie within R metres of (X, Y, Z) (one character).
"""
import base64
import json
import math
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import chaintex  # noqa: E402
import dmacmp  # noqa: E402
import gslm  # noqa: E402
import gsvram  # noqa: E402
import jade2gltf  # noqa: E402

PALETTE_VU = 0x388


def gs_replay(tag, data, vram, regs):
    """Applies a record's A+D register writes and IMAGE uploads (PSMCT32 destinations) to the GS model."""
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
            elif flg == 2:
                img = gif[q:q + 16 * nloop]
                q += 16 * nloop
                b, pos, reg = regs.get(0x50, 0), regs.get(0x51, 0), regs.get(0x52, 0)
                dbp, dbw, dpsm = (b >> 32) & 0x3FFF, (b >> 48) & 0x3F, (b >> 56) & 0x3F
                dx, dy, w, h = (pos >> 32) & 0x7FF, (pos >> 48) & 0x7FF, reg & 0xFFF, (reg >> 32) & 0xFFF
                if dpsm == 0 and w:
                    for i in range(min(len(img) // 4, w * h)):
                        y, x = divmod(i, w)
                        o = gsvram.addr(dbp, dbw, dx + x, dy + y)
                        vram[o:o + 4] = img[4 * i:4 * i + 4]
            else:
                break


def texture(vram, tex0):
    """(w, h, RGBA bytes) of a PSMT8 / PSMT4 (CSM1 PSMCT32 CLUT) or PSMCT32 texture, alpha 0x80 -> 255."""
    tbp, tbw, psm = tex0 & 0x3FFF, (tex0 >> 14) & 0x3F, (tex0 >> 20) & 0x3F
    w, h = 1 << ((tex0 >> 26) & 0xF), 1 << ((tex0 >> 30) & 0xF)
    out = bytearray()
    if psm == 0x13:
        cbp = (tex0 >> 37) & 0x3FFF
        clut = []
        for i in range(256):
            j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
            a = gsvram.addr(cbp, 1, j & 15, j >> 4)
            c = vram[a:a + 4]
            clut.append(bytes((c[0], c[1], c[2], min(c[3] * 2, 255))))
        for y in range(h):
            for x in range(w):
                out += clut[vram[gsvram.addr8(tbp, tbw, x, y)]]
    elif psm == 0x14:  # PSMT4, 16-entry PSMCT32 CLUT (CSM1: 8x2 at CBP; CSA assumed 0)
        cbp = (tex0 >> 37) & 0x3FFF
        clut = []
        for i in range(16):
            a = gsvram.addr(cbp, 1, i % 8, i // 8)
            c = vram[a:a + 4]
            clut.append(bytes((c[0], c[1], c[2], min(c[3] * 2, 255))))
        for y in range(h):
            for x in range(w):
                n = gslm.addr4(tbp, tbw, x, y)
                byte = vram[(n >> 1) % (4 << 20)]
                out += clut[(byte >> 4) if n & 1 else (byte & 0xF)]
    elif psm == 0:
        for y in range(h):
            for x in range(w):
                a = gsvram.addr(tbp, tbw, x, y)
                c = vram[a:a + 4]
                out += bytes((c[0], c[1], c[2], min(c[3] * 2, 255)))
    else:
        return None
    return w, h, bytes(out)


def skinned_draws(chain_path, with_textures=False):
    """[(target, packet bytes, {vu addr: [16 floats] matrices uploaded before the CALL}, texture or None,
    alpha test (ATE, AREF), GS ALPHA blend byte)]; textures decoded from GS memory as the frame's uploads left it at the draw."""
    out, pre, cur = [], {}, None
    vram, regs, cache = bytearray(4 << 20), {}, {}
    for pc, tag, da, data in dmacmp.chain_records(chain_path):
        tid = (tag[0] >> 28) & 7
        if with_textures:
            uploads = len(cache)
            gs_replay(tag, data, vram, regs)
        if cur is not None:
            cur['bytes'] += struct.pack('<4I', *tag) + data if tid not in (0, 3, 4) else struct.pack('<4I', *tag)
            cur['ups'] += dmacmp.vif_unpacks(tag, data)
        else:
            for f, n, a, p in dmacmp.vif_unpacks(tag, data):
                if f == 0xC and n % 4 == 0 and len(p) >= 64 * (n // 4):
                    for k in range(n // 4):
                        pre[a + 4 * k] = list(struct.unpack_from('<16f', p, 64 * k))
        if tid == 5:
            cur = {'target': tag[1] & 0x7FFFFFFF, 'bytes': b'', 'ups': [], 'mats': dict(pre),
                   'tex0': regs.get(0x06), 'test': regs.get(0x47, 0), 'alpha': regs.get(0x42, 0)}
        elif tid == 6 and cur is not None:
            addrs = {(f, a) for f, n, a, p in cur['ups']}
            if (0xC, 0x53) in addrs or (8, 0x53) in addrs or ((0xC, 0x3) in addrs and (0xA, 0x83) in addrs):
                tex = None
                if with_textures and cur['tex0'] is not None:
                    tex = texture(vram, cur['tex0'])
                t = cur['test']
                out.append((cur['target'], cur['bytes'], cur['mats'], tex, (t & 1, (t >> 4) & 0xFF), cur['alpha'] & 0xFF))
            cur = None
    return out


TRANSPOSE = False  # experiment only: transposed rotations break the body apart (15 Oct), so False is right


def xform(m, v):
    x, y, z = v
    if TRANSPOSE:
        return (x * m[0] + y * m[1] + z * m[2] + m[12], x * m[4] + y * m[5] + z * m[6] + m[13], x * m[8] + y * m[9] + z * m[10] + m[14])
    return (x * m[0] + y * m[4] + z * m[8] + m[12], x * m[1] + y * m[5] + z * m[9] + m[13], x * m[2] + y * m[6] + z * m[10] + m[14])


def build(chain_path, near=None, with_textures=False):
    posed, bind = [], []
    for target, pkt, mats, tex, test, alpha in skinned_draws(chain_path, with_textures):
        if alpha == 0x46:
            # second pass of characters: ALPHA 0x46 = Cd * (1 - As) with a 4-bit shading texture whose
            # UVs VU1 probably computes (not modelled yet); drawn as an ordinary mesh it hides the colours
            continue
        batches = jade2gltf.skinned_batches(pkt, 0, len(pkt))
        # layout B (no UNPACK to VU 0x53): one rigid part, placed by the object matrix at VU 0
        single = all(not ((w >> 29) & 3 == 3 and (w & 0x3FF) == 0x53) for w in struct.unpack_from('<%dI' % (len(pkt) // 4), pkt))
        for pos, uv, fl, bones, weights, normals, colours in batches:
            n = len(pos)
            tris = [(i - 2, i - 1, i) if i % 2 == 0 else (i - 1, i - 2, i) for i in range(2, n) if not fl[i]]
            if not tris:
                continue
            out = []
            for v, b, w in zip(pos, bones, weights):
                acc = [0.0, 0.0, 0.0]
                tot = 0.0
                for slot, wt in zip(b, w):
                    if wt == 0:
                        continue
                    m = mats.get(0) if single else mats.get(PALETTE_VU + 4 * (slot - 1)) if slot > 0 else None
                    if m is None:
                        continue
                    p = xform(m, v)
                    acc = [acc[k] + wt * p[k] for k in range(3)]
                    tot += wt
                out.append(tuple(a / tot for a in acc) if tot else tuple(v))
            if near:
                cx, cy, cz, r = near
                if not any((p[0] - cx) ** 2 + (p[1] - cy) ** 2 + (p[2] - cz) ** 2 < r * r for p in out):
                    continue
            # colour 1.0 = PS2 MODULATE at 0x80; normal-lit parts are lit by VU1 at run time (not modelled yet)
            col = colours if colours else [(1.0, 1.0, 1.0, 1.0)] * n
            posed.append((out, uv, col, tris, target, tex, test))
            bind.append((list(pos), uv, col, tris, target, tex, test))
    return posed, bind


def write_gltf(meshes, path, offset_grid=False):
    buf = bytearray()
    g = {'asset': {'version': '2.0', 'generator': 'chainskin.py'}, 'buffers': [], 'bufferViews': [], 'accessors': [],
         'meshes': [], 'nodes': [], 'materials': [{'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 1.0}, 'doubleSided': True}]}

    def add(data, fmt, count, typ, ctype, target=None):
        while len(buf) % 4:
            buf.append(0)
        o = len(buf)
        buf.extend(data)
        bv = {'buffer': 0, 'byteOffset': o, 'byteLength': len(data)}
        if target:
            bv['target'] = target
        g['bufferViews'].append(bv)
        acc = {'bufferView': len(g['bufferViews']) - 1, 'componentType': ctype, 'count': count, 'type': typ}
        if fmt == 'pos':
            vals = list(struct.iter_unpack('<3f', data))
            acc['min'] = [min(v[k] for v in vals) for k in range(3)]
            acc['max'] = [max(v[k] for v in vals) for k in range(3)]
        g['accessors'].append(acc)
        return len(g['accessors']) - 1
    images = {}

    def material(tex, test):
        if tex is None:
            return 0
        key = (tex[0], tex[1], hash(tex[2]), test)
        if key not in images:
            g.setdefault('images', []).append({'uri': 'data:image/png;base64,' + base64.b64encode(jade2gltf.png_bytes(*tex)).decode()})
            g.setdefault('textures', []).append({'source': len(g['images']) - 1, 'sampler': 0})
            g.setdefault('samplers', [{'magFilter': 9729, 'minFilter': 9729, 'wrapS': 10497, 'wrapT': 10497}])
            m = {'pbrMetallicRoughness': {'baseColorTexture': {'index': len(g['textures']) - 1}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0},
                 'doubleSided': True}
            if test[0]:  # GS alpha test (ATST GREATER as the game uses), AREF on the 0x80 scale
                m.update({'alphaMode': 'MASK', 'alphaCutoff': round(min(test[1] * 2, 255) / 255.0, 3)})
            g['materials'].append(m)
            images[key] = len(g['materials']) - 1
        return images[key]
    for i, (pos, uv, col, tris, target, tex, test) in enumerate(meshes):
        a_pos = add(b''.join(struct.pack('<3f', *p) for p in pos), 'pos', len(pos), 'VEC3', 5126, 34962)
        a_col = add(b''.join(struct.pack('<4f', *c[:4]) if len(c) >= 4 else struct.pack('<4f', 1, 1, 1, 1) for c in col[:len(pos)]), None, len(pos), 'VEC4', 5126, 34962)
        a_uv = add(b''.join(struct.pack('<2f', u / 512.0, v / 512.0) for u, v in uv[:len(pos)]), None, len(pos), 'VEC2', 5126, 34962)
        a_idx = add(b''.join(struct.pack('<3H', *t) for t in tris), None, 3 * len(tris), 'SCALAR', 5123, 34963)
        g['meshes'].append({'name': '%x/%d' % (target, i), 'primitives': [{'attributes': {'POSITION': a_pos, 'TEXCOORD_0': a_uv, 'COLOR_0': a_col},
                                                                          'indices': a_idx, 'material': material(tex, test)}]})
        g['nodes'].append({'name': 'skin %x' % target, 'mesh': i})
    g['buffers'].append({'byteLength': len(buf), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(buf)).decode()})
    g['scenes'] = [{'nodes': list(range(len(g['nodes'])))}]
    g['scene'] = 0
    json.dump(g, open(path, 'w'))


def main(argv):
    near = None
    if '--near' in argv:
        near = [float(x) for x in argv[argv.index('--near') + 1].split(',')]
    posed, bind = build(argv[0], near, '--textures' in argv)
    write_gltf(posed, argv[1])
    if len(argv) > 2 and not argv[2].startswith('--'):
        write_gltf(bind, argv[2])
    pts = [p for m in posed for p in m[0]]
    print(f'{len(posed)} skinned batches, {len(pts)} vertices; posed bounds '
          f'{[round(min(p[k] for p in pts), 1) for k in range(3)]} .. {[round(max(p[k] for p in pts), 1) for k in range(3)]}')


if __name__ == '__main__':
    main(sys.argv[1:])
