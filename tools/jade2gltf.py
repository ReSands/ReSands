"""Converts the meshes of an unpacked PS2 world pack to glTF, for checking our reading of the format.

Usage: python3 -I tools/jade2gltf.py <unpacked pack.bin> <out.gltf> [--uvscale N]
  (pack from: jadetex.py unpack PRINCE.BF <key> <out>)

How: the converted engine loader (tools/c2py_rt.py, game/analysis/meshport/py) parses every geometry
record; a wrapper on CJMeshElement::ReadElement records where each element's data lies. Each
element (grammar of ReadElement: bbox min/max, word, bone palette, LOD blobs, pass packets) is
drawn from its first pass packet: a DMA chain of VIF UNPACKs per batch (V3-16 UV + strip flag,
V4-8 RGBA with 0x80 = 1.0, V3-32 position, V4-32 x3 batch bbox, MSCNT). Triangle strips: a vertex
whose flag is set starts a strip (no triangle ends there). Elements without pass packets (positions-
only LOD blobs) are skipped and counted. Each geometry record becomes one node, laid out in a grid,
since world placement lives elsewhere (game objects).
"""
import base64
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c2py_rt  # noqa: E402
import jadetex  # noqa: E402

READER = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'analysis', 'meshport', 'py')


def png_bytes(w, h, rgba):
    import zlib
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda t, data: struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def capture_elements(pack):
    """(record key, element offset, size, material index) for every CJMeshElement the converted
    loader reads. The material index is field +4 of the geometry's 0x1C-byte element entry (table
    at object +0x28, count +0x24), whose +0x18 heads the chain of mesh elements (+0x4C = next)."""
    rt0 = c2py_rt.Runtime()
    c2py_rt.load_funcs(rt0, READER)
    orig = rt0.funcs['ReadElement__13CJMeshElementFPPc']
    create = rt0.funcs['GEO_p_CreateFromBuffer__FP16GRO_tdst_Struct_PPcP15WOR_tdst_World_Ui']
    out, cur = [], {}

    def wrap(rt, this, pp):
        a = rt.r32(pp)
        r = orig(rt, this, pp)
        cur['found'].append((this, cur['off'] + (a - 0x01000000), rt.r32(pp) - a))
        return r

    rt0.funcs['ReadElement__13CJMeshElementFPPc'] = wrap
    for key, (off, size) in sorted(jadetex.records(pack).items()):
        if size < 8 or struct.unpack_from('<I', pack, off)[0] != 1 or not 0 < struct.unpack_from('<I', pack, off + 4)[0] < 16:
            continue
        cur['off'], cur['found'] = off, []
        rt = c2py_rt.Runtime()
        rt.funcs = rt0.funcs
        rt.write(0x01000000, pack[off:off + size])
        cell = rt.alloca(8)
        rt.w32(cell, 0x01000000 + 8)
        obj = create(rt, 0, cell, 0, struct.unpack_from('<I', pack, off + 4)[0])
        material = {}
        if obj:
            table, count = rt.r32(obj + 0x28), rt.r32(obj + 0x24)
            for e in range(min(count, 4096)):
                mat = rt.r32(table + 0x1C * e + 4)
                m = rt.r32(table + 0x1C * e + 0x18)
                chain = 0
                while m and m not in material:
                    material[m] = (mat, e, chain)
                    m = rt.r32(m + 0x4C)
                    chain += 1
        for this, eo, es in cur['found']:
            mat, entry, chain = material.get(this, (0, -1, 0))
            out.append((key, eo, es, mat, entry, chain))
    return out


def parse_visual(d, p, end):
    """Grammar of GEO_LoadPS2GaoVisu / CJMeshElementRef (speed mode). Returns (end offset, refs) or None.
    refs: per element, list of chain entries; entry = (lods [(bits, off, size)], passes [(index, off, size)])."""
    U = lambda q: struct.unpack_from('<I', d, q)[0]
    if p + 4 > end: return None
    n = U(p); p += 4
    if not 0 < n <= 64: return None
    refs = []
    for _ in range(n):
        if p + 8 > end: return None
        p += 4; m = U(p); p += 4
        if not 0 < m <= 16: return None
        chain = []
        for _ in range(m):
            if p + 4 > end: return None
            a = U(p); p += 4
            if a > 8: return None
            lods = []
            for _ in range(a):
                if p + 8 > end: return None
                bits, size = U(p), U(p + 4); p += 8
                if size > end - p: return None
                lods.append((bits, p, size)); p += size
            if p + 4 > end: return None
            b = U(p); p += 4
            if b > 8: return None
            passes = []
            for _ in range(b):
                if p + 8 > end: return None
                idx, size = U(p), U(p + 4); p += 8
                if idx > 15 or size > end - p: return None
                passes.append((idx, p, size)); p += size
            chain.append((lods, passes))
        if p + 4 > end: return None
        p += 4
        refs.append(chain)
    return p, refs


def variant3_batches(d, lo, ls):
    """Shared geometry block of instanced meshes (LOD variant 3): per batch, raw V3-16 UVs (s, t,
    strip flag; their UNPACK code is in each object's pass packet) directly before the V3-32
    positions UNPACK, then the V4-32 x3 bbox and MSCNT. Returns [(positions, uvs, flags)]."""
    out, p, end = [], lo, lo + ls
    while p + 4 <= end:
        w = struct.unpack_from('<I', d, p)[0]
        cmd, num = (w >> 24) & 0x7F, (w >> 16) & 0xFF
        if cmd in (0x68, 0x78) and num:
            n = num
            uv0 = p - ((6 * n + 3) // 4 * 4)
            if uv0 < lo:
                return out
            pos = [struct.unpack_from('<3f', d, p + 4 + 12 * i) for i in range(n)]
            raw = [struct.unpack_from('<3h', d, uv0 + 6 * i) for i in range(n)]
            out.append((pos, [(a, b) for a, b, _ in raw], [c for _, _, c in raw]))
            p += 4 + 12 * n
            continue
        p += 4
    return out


def element_parts(d, o):
    U = lambda p: struct.unpack_from('<I', d, p)[0]
    q = o + 36
    nb = U(q)
    q += 4 + nb
    lods = []
    n = U(q)
    q += 4
    for _ in range(n):
        v, s = U(q), U(q + 4)
        lods.append((v, q + 8, s))
        q += 8 + s
    passes = []
    n2 = U(q)
    q += 4
    for _ in range(n2):
        idx, s = U(q), U(q + 4)
        passes.append((idx, q + 8, s))
        q += 8 + s
    return nb, lods, passes


EL = {0: 4, 1: 2, 2: 1, 4: 8, 5: 4, 6: 2, 8: 12, 9: 6, 0xA: 3, 0xC: 16, 0xD: 8, 0xE: 4, 0xF: 2}


def decode_packet(d, o, size, blobs=None):
    """Batches of {format: (addr, raw bytes, count)} from a DMA chain of VIF codes. DMA tags: cnt (1)
    carries its data inline; ref/refe (3/0) point at data elsewhere, and in Jade's packets the tag's
    address is the index of one of the element's blocks (LOD blobs by their 'bits' value, or a game
    object's per-instance blocks), patched to a pointer at load: `blobs` maps index -> (offset, size).
    The tag's upper 8 bytes hold two VIF codes; an UNPACK there takes its data from the reference."""
    U = lambda p: struct.unpack_from('<I', d, p)[0]
    batches, cur = [], {}

    def vif(v, stop, ext=None):
        # walk VIF codes in [v, stop); ext = (offset, size) of referenced data for a trailing UNPACK
        nonlocal cur
        while v < stop:
            w = U(v)
            cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
            if cmd >= 0x60:
                f = cmd & 0xF
                n = num or 256
                nbytes = (EL[f] * n + 3) // 4 * 4
                if ext is not None and v + 4 >= stop:
                    src = ext[0]
                    if f in cur:  # same format again without MSCNT: the next batch starts
                        batches.append(cur)
                        cur = {}
                    cur[f] = (imm & 0x3FF, d[src:src + EL[f] * n], n)
                    # the referenced block may continue with more VIF codes after this UNPACK's
                    # data (e.g. UVs, then UNPACK V4-8 + baked vertex colours)
                    rest = src + nbytes
                    if rest + 4 <= ext[0] + ext[1]:
                        vif(rest, ext[0] + ext[1])
                    v = stop
                    continue
                cur.setdefault(f, (imm & 0x3FF, d[v + 4:v + 4 + EL[f] * n], n))
                v += 4 + nbytes
            else:
                if cmd == 0x17:  # MSCNT: batch complete
                    batches.append(cur)
                    cur = {}
                v += 4 + {0x20: 4, 0x30: 16, 0x31: 16}.get(cmd, 0) + (16 * (imm or 65536) if cmd in (0x50, 0x51) else 0)
        return v

    p, end = o, o + size
    while p + 16 <= end:
        lo, addr = U(p), U(p + 4)
        qwc, tid = lo & 0xFFFF, (lo >> 28) & 7
        if tid in (0, 3) and blobs is not None:  # refe / ref: data from block `addr`
            # address = block index (top byte) + byte offset within the block (low 24 bits)
            ext = blobs.get(addr >> 24)
            if ext is not None and (addr & 0xFFFFFF) + qwc * 16 <= ext[1]:
                vif(p + 8, p + 16, (ext[0] + (addr & 0xFFFFFF), qwc * 16))
            p += 16
            if tid == 0:
                break
            continue
        stop = p + 16 + 16 * qwc
        vif(p + 8, min(stop, end))
        p = stop
        if tid in (6, 7):  # ret / end
            break
    if cur:  # reference-style packets end without MSCNT (the program start comes from the caller)
        batches.append(cur)
    return batches


def batch_mesh(b, uvscale):
    if 8 not in b:
        return None
    _, raw, n = b[8]
    pos = [struct.unpack_from('<3f', raw, 12 * i) for i in range(n)]
    uv, flags = [(0.0, 0.0)] * n, [0] * n
    if 9 in b:
        _, r, m = b[9]
        for i in range(min(n, m)):
            s, t, f = struct.unpack_from('<3h', r, 6 * i)
            uv[i], flags[i] = (s / uvscale, t / uvscale), f
    col = [(1.0, 1.0, 1.0, 1.0)] * n
    if 0xE in b:
        _, r, m = b[0xE]
        col = [tuple(min(r[4 * i + k] / 128.0, 1.0) for k in range(4)) for i in range(min(n, m))] + col[m:]
    tris = []
    for i in range(2, n):
        if flags[i]:
            continue
        tris.append((i - 2, i - 1, i) if i % 2 == 0 else (i - 1, i - 2, i))
    return pos, uv, col, tris


def game_objects(d, recs, geometry_keys):
    """(name, 16 floats, geometry key) for each .gao record that references a converted geometry.
    Layout: magic '.gao', name length at +0x10, name at +0x14, 10 bytes, 4x4 float matrix (3x3
    rotation + translation in metres, row vectors), then keys; the first key naming a converted
    geometry is the visual."""
    out = []
    for key, (o, s) in recs.items():
        if s < 0x80 or d[o:o + 4] != b'.gao':
            continue
        nl = struct.unpack_from('<I', d, o + 0x10)[0]
        if not 0 < nl < 128:
            continue
        name = d[o + 0x14:o + 0x14 + nl].split(b'\0')[0].decode('latin1')
        m0 = o + 0x14 + nl + 10
        m = list(struct.unpack_from('<16f', d, m0))
        if not all(math.isfinite(x) for x in m[:15]):
            continue
        geo = None
        for i in range(m0 + 64, min(o + s - 3, m0 + 64 + 0x80), 2):
            k = struct.unpack_from('<I', d, i)[0]
            if k in geometry_keys:
                geo = k
                mm = struct.unpack_from('<I', d, i + 4)[0]
                break
        if geo is not None:
            out.append((name, m, geo, mm, o, s, m0 + 64))
    return out


def main(argv):
    pack_path, out_path = argv[0], argv[1]
    uvscale = float(argv[argv.index('--uvscale') + 1]) if '--uvscale' in argv else 4096.0
    d = open(pack_path, 'rb').read()
    elements = capture_elements(d)
    by_record = {}
    stats = {'elements': len(elements), 'drawn': 0, 'no_pass': 0, 'batches': 0, 'verts': 0, 'tris': 0, 'uvmax': 0}
    v3_of, n_entries = {}, {}
    for key, o, _size, matid, entry, chain in elements:
        n_entries[key] = max(n_entries.get(key, 0), entry + 1)
        nb, lods, passes = element_parts(d, o)
        if not passes:
            v3 = [(lo, ls) for v, lo, ls in lods if v == 3]
            if v3:
                # instanced mesh: shared UVs/positions here, colours per game object (see main's placement)
                v3_of.setdefault(key, []).append((entry, chain, matid, variant3_batches(d, *v3[0])))
                stats['instanced'] = stats.get('instanced', 0) + 1
            else:
                stats['no_pass'] += 1
            continue
        _idx, po, ps = passes[0]
        blocks = {bits: (lo, ls) for bits, lo, ls in lods}
        # lightmap pass (pass index 3): same positions, second UV set (block 8)
        lm_uv = []
        for pidx, lpo, lps in passes:
            if pidx == 3:
                for lb in decode_packet(d, lpo, lps, blocks):
                    if 8 in lb:
                        uv2 = None
                        if 9 in lb:
                            _, r9, n9 = lb[9]
                            # lightmap UVs are 8x finer than texture UVs (1/32768: 0..1 across the page)
                            uv2 = [(a / (8 * uvscale), c / (8 * uvscale)) for a, c, _f in (struct.unpack_from('<3h', r9, 6 * i) for i in range(n9))]
                        lm_uv.append(uv2)
        batch_no = -1
        for b in decode_packet(d, po, ps, blocks):
            m = batch_mesh(b, uvscale)
            if 8 in b:
                batch_no += 1
            if not m or not m[3]:
                continue
            uv2 = lm_uv[batch_no] if 0 <= batch_no < len(lm_uv) else None
            m = m + ((uv2 if uv2 and len(uv2) == len(m[0]) else None),)
            if m[4]:
                stats['lightmapped_batches'] = stats.get('lightmapped_batches', 0) + 1
            stats['batches'] += 1
            stats['verts'] += len(m[0])
            stats['tris'] += len(m[3])
            if 9 in b:
                _, r, k = b[9]
                stats['uvmax'] = max(stats['uvmax'], max(abs(x) for i in range(k) for x in struct.unpack_from('<2h', r, 6 * i)))
            by_record.setdefault(key, []).append((m, matid))
        stats['drawn'] += 1

    # glTF: one mesh per record (one primitive per batch), nodes in a grid.
    buf = bytearray()
    accessors, views, meshes, nodes = [], [], [], []

    def add(data, comp, typ, count, target=None, minmax=None):
        while len(buf) % 4:
            buf.append(0)
        views.append({'buffer': 0, 'byteOffset': len(buf), 'byteLength': len(data), **({'target': target} if target else {})})
        buf.extend(data)
        acc = {'bufferView': len(views) - 1, 'componentType': comp, 'count': count, 'type': typ}
        if minmax:
            acc['min'], acc['max'] = minmax
        accessors.append(acc)
        return len(accessors) - 1

    keys = sorted(by_record)
    prims_of = {}
    place = '--place' in argv
    cols = max(1, int(math.ceil(math.sqrt(len(keys)))))
    spacing = 0.0
    for parts in by_record.values():
        for (pos, *_), _mat in parts:
            for axis in range(3):
                spacing = max(spacing, max(p[axis] for p in pos) - min(p[axis] for p in pos))
    spacing = min(spacing * 1.2 + 0.1, 50.0)
    for n_key, key in enumerate(keys):
        prims = []
        for (pos, uv, col, tris, uv2), matid in by_record[key]:
            lo = [min(p[a] for p in pos) for a in range(3)]
            hi = [max(p[a] for p in pos) for a in range(3)]
            a_pos = add(b''.join(struct.pack('<3f', *p) for p in pos), 5126, 'VEC3', len(pos), 34962, (lo, hi))
            a_uv = add(b''.join(struct.pack('<2f', *t) for t in uv), 5126, 'VEC2', len(uv), 34962)
            a_col = add(b''.join(struct.pack('<4f', *c) for c in col), 5126, 'VEC4', len(col), 34962)
            a_idx = add(b''.join(struct.pack('<3H', *t) for t in tris), 5123, 'SCALAR', 3 * len(tris), 34963)
            attrs = {'POSITION': a_pos, 'TEXCOORD_0': a_uv, 'COLOR_0': a_col}
            if uv2:
                attrs['TEXCOORD_1'] = add(b''.join(struct.pack('<2f', *t) for t in uv2), 5126, 'VEC2', len(uv2), 34962)
            prims.append(({'attributes': attrs, 'indices': a_idx}, matid))
        prims_of[key] = prims
        meshes.append({'name': f'{key:08x}', 'primitives': [p for p, _ in prims]})
        if not place:
            # PS2 data is Z-up (metres); glTF is Y-up: rotate -90 degrees about X.
            nodes.append({'name': f'{key:08x}', 'mesh': len(meshes) - 1,
                          'translation': [(n_key % cols) * spacing, 0.0, (n_key // cols) * spacing],
                          'rotation': [-0.7071068, 0.0, 0.0, 0.7071068]})
    roots = list(range(len(nodes)))
    if place:
        recs = jadetex.records(d)
        images, textures, materials = [], [], []
        material_cache, mesh_cache = {}, {}

        lightmap_cache = {}

        def lightmap_texture(lm):
            # lightmap page (texture format 6) -> glTF texture index
            if lm not in lightmap_cache:
                lightmap_cache[lm] = None
                try:
                    w, h, rgba = jadetex.decode(d, recs, lm, 'ps2')
                    images.append({'uri': 'data:image/png;base64,' + base64.b64encode(png_bytes(w, h, rgba)).decode(), 'name': f'lightmap {lm:08x}'})
                    textures.append({'source': len(images) - 1, 'sampler': 0})
                    lightmap_cache[lm] = len(textures) - 1
                    stats['lightmaps'] = stats.get('lightmaps', 0) + 1
                except Exception:  # noqa: BLE001
                    pass
            return lightmap_cache[lm]

        def material(mm, idx, lm=None):
            base = material_base(mm, idx)
            if lm is None or base is None:
                return base
            if (mm, idx, lm) not in material_cache:
                t = lightmap_texture(lm)
                if t is None:
                    material_cache[(mm, idx, lm)] = base
                else:
                    # lightmap as glTF occlusion on the second UV set (the game multiplies it in a
                    # second pass)
                    materials.append({**materials[base], 'name': materials[base]['name'] + f'+lm {lm:08x}',
                                      'occlusionTexture': {'index': t, 'texCoord': 1}})
                    material_cache[(mm, idx, lm)] = len(materials) - 1
            return material_cache[(mm, idx, lm)]

        def material_base(mm, idx):
            # multi-material (first word 4, count at +8, sub-material keys from +0xC) -> sub-material
            # (first word 5, texture key at +0x2C) -> PS2 texture record
            if (mm, idx) in material_cache:
                return material_cache[(mm, idx)]
            result = None
            if mm in recs:
                mo, _ = recs[mm]
                if struct.unpack_from('<I', d, mo)[0] == 4 and idx < struct.unpack_from('<I', d, mo + 8)[0]:
                    sub = struct.unpack_from('<I', d, mo + 0xC + 4 * idx)[0]
                    if sub in recs and struct.unpack_from('<I', d, recs[sub][0])[0] == 5:
                        tex = struct.unpack_from('<I', d, recs[sub][0] + 0x2C)[0]
                        if tex in recs and jadetex.is_texture(d, recs[tex][0]):
                            try:
                                w, h, rgba = jadetex.decode(d, recs, tex, 'ps2')
                            except Exception:
                                w = 0
                            if w:
                                images.append({'uri': 'data:image/png;base64,' + base64.b64encode(png_bytes(w, h, rgba)).decode(), 'name': f'{tex:08x}'})
                                textures.append({'source': len(images) - 1, 'sampler': 0})
                                materials.append({'name': f'{tex:08x}', 'pbrMetallicRoughness': {'baseColorTexture': {'index': len(textures) - 1}, 'metallicFactor': 0.0, 'roughnessFactor': 1.0},
                                                  'alphaMode': 'MASK', 'alphaCutoff': 0.5, 'doubleSided': True})
                                result = len(materials) - 1
                                stats['textured'] = stats.get('textured', 0) + 1
            material_cache[(mm, idx)] = result
            return result

        def lightmap_of(go, gs):
            # the game object's lightmap page: first referenced key that is a format-6 texture (keys
            # follow the variable-length name, so they can sit at odd offsets)
            for i in range(go, go + gs - 3):
                k = struct.unpack_from('<I', d, i)[0]
                if k in recs and jadetex.is_texture(d, recs[k][0]) and struct.unpack_from('<H', d, recs[k][0] + 8)[0] == 6:
                    return k
            return None

        def mesh_for(geo, mm, lm=None):
            if (geo, mm, lm) not in mesh_cache:
                prims = []
                for prim, matid in prims_of[geo]:
                    m = material(mm, matid, lm if 'TEXCOORD_1' in prim['attributes'] else None)
                    prims.append({**prim, **({'material': m} if m is not None else {})})
                meshes.append({'name': f'{geo:08x}/{mm:08x}', 'primitives': prims})
                mesh_cache[(geo, mm, lm)] = len(meshes) - 1
            return mesh_cache[(geo, mm, lm)]

        def instance_mesh(name, geo, mm, go, gs, start):
            # Per-object visual block in the game-object record (GEO_LoadPS2GaoVisu grammar): first
            # offset whose parse gives one reference per geometry element entry.
            refs = None
            for st in range(start, go + gs - 4):
                res = parse_visual(d, st, go + gs)
                if res and len(res[1]) == n_entries.get(geo, -1):
                    refs = res[1]
                    break
            prims = [{**prim, **({'material': material(mm, matid)} if material(mm, matid) is not None else {})}
                     for prim, matid in prims_of.get(geo, [])]
            for entry, chain, matid, batches in v3_of[geo]:
                colours = None
                if refs and entry < len(refs) and chain < len(refs[entry]):
                    blobs = [(off, size) for bits, off, size in refs[entry][chain][0] if bits == 4]
                    if blobs:
                        colours = d[blobs[0][0]:blobs[0][0] + blobs[0][1]]
                vbase = 0
                for pos, uv, flags in batches:
                    n = len(pos)
                    col = [tuple(min(colours[4 * (vbase + i) + k] / 128.0, 1.0) for k in range(4)) if colours and 4 * (vbase + i) + 4 <= len(colours) else (1.0, 1.0, 1.0, 1.0) for i in range(n)]
                    vbase += n
                    tris = [(i - 2, i - 1, i) if i % 2 == 0 else (i - 1, i - 2, i) for i in range(2, n) if not flags[i]]
                    if not tris:
                        continue
                    lo = [min(q[a] for q in pos) for a in range(3)]
                    hi = [max(q[a] for q in pos) for a in range(3)]
                    prim = {'attributes': {'POSITION': add(b''.join(struct.pack('<3f', *q) for q in pos), 5126, 'VEC3', n, 34962, (lo, hi)),
                                           'TEXCOORD_0': add(b''.join(struct.pack('<2f', a / uvscale, b / uvscale) for a, b in uv), 5126, 'VEC2', n, 34962),
                                           'COLOR_0': add(b''.join(struct.pack('<4f', *c) for c in col), 5126, 'VEC4', n, 34962)},
                            'indices': add(b''.join(struct.pack('<3H', *t) for t in tris), 5123, 'SCALAR', 3 * len(tris), 34963)}
                    m = material(mm, matid)
                    if m is not None:
                        prim['material'] = m
                    prims.append(prim)
                    stats['instanced_tris'] = stats.get('instanced_tris', 0) + len(tris)
            stats['instances'] = stats.get('instances', 0) + 1
            stats['instances_coloured'] = stats.get('instances_coloured', 0) + (1 if refs else 0)
            meshes.append({'name': name, 'primitives': prims})
            return len(meshes) - 1

        # Static world geometry is stored in world coordinates and drawn with an identity matrix (the
        # game object's matrix is only its pivot); local geometry uses the object's matrix. Rule,
        # checked against a captured frame's DMA matrices (53/53 objects): world space when the
        # vertices' centre is nearer the object's pivot than the origin.
        centre = {}
        for geo, parts in by_record.items():
            pts = [q for (pos, *_), _mat in parts for q in pos[::4]]
            if pts:
                centre[geo] = [sum(q[k] for q in pts) / len(pts) for k in range(3)]
        for geo, items in v3_of.items():
            pts = [q for _e, _c, _m, batches in items for pos, _uv, _fl in batches for q in pos[::4]]
            if pts and geo not in centre:
                centre[geo] = [sum(q[k] for q in pts) / len(pts) for k in range(3)]
        placed = [g for g in game_objects(d, recs, set(keys) | set(v3_of)) if g[1][14] > -1000]
        children = []
        for name, m, geo, mm, go, gs, start in placed:
            c = centre.get(geo)
            if c and math.dist(c, m[12:15]) < math.dist(c, (0.0, 0.0, 0.0)):
                m = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
                stats['world_space'] = stats.get('world_space', 0) + 1
            # Row-vector matrix (rows = axes, row 3 = translation) is glTF's column-major array as stored.
            mat = m[:3] + [0.0] + m[4:7] + [0.0] + m[8:11] + [0.0] + m[12:15] + [1.0]
            nodes.append({'name': name, 'mesh': instance_mesh(name, geo, mm, go, gs, start) if geo in v3_of else mesh_for(geo, mm, lightmap_of(go, gs)), 'matrix': mat})
            children.append(len(nodes) - 1)
        nodes.append({'name': 'world (Z-up)', 'children': children, 'rotation': [-0.7071068, 0.0, 0.0, 0.7071068]})
        roots = [len(nodes) - 1]
        extra = {'images': images, 'textures': textures, 'materials': materials,
                 'samplers': [{'magFilter': 9729, 'minFilter': 9729, 'wrapS': 10497, 'wrapT': 10497}]} if images else {}
        stats['placed'] = len(placed)
    if not place:
        extra = {}
    gltf = {**extra, 'asset': {'version': '2.0', 'generator': 'PoP jade2gltf'},
            'scene': 0, 'scenes': [{'nodes': roots}], 'nodes': nodes, 'meshes': meshes,
            'accessors': accessors, 'bufferViews': views,
            'buffers': [{'byteLength': len(buf), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(bytes(buf)).decode()}]}
    json.dump(gltf, open(out_path, 'w'))
    if place:
        print(f"{stats['placed']} game objects placed (objects parked below z -1000 left out), {stats.get('textured', 0)} textured materials; "
              f"{stats.get('instances', 0)} instanced objects ({stats.get('instances_coloured', 0)} with their colour block, {stats.get('instanced_tris', 0)} triangles); "
              f"{stats.get('world_space', 0)} with world-space geometry (identity matrix); "
              f"{stats.get('lightmaps', 0)} lightmap pages, {stats.get('lightmapped_batches', 0)} lightmapped batches")
    print(f"{stats['elements']} elements: {stats['drawn']} drawn ({stats['batches']} batches, {stats['verts']} vertices, "
          f"{stats['tris']} triangles), {stats['no_pass']} without a pass packet; {len(keys)} records -> {out_path}; "
          f"largest raw UV {stats['uvmax']} (scale 1/{uvscale:g})")


if __name__ == '__main__':
    main(sys.argv[1:])
