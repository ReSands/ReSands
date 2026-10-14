"""Renders a glTF from tools/jade2gltf.py to a PNG (pure Python: z-buffer, flat shading by face
normal times vertex colour), so mesh decoding can be checked without a 3D viewer.

Usage: python3 -I tools/gltfpreview.py <in.gltf> <out.png> [width] [--node NAME] [--yaw DEG] [--pitch DEG]
Each node is drawn at its translation (jade2gltf lays records out in a grid); camera looks down at
the scene from the given yaw/pitch (default 35 / 30 degrees), orthographic, fitted to the scene.
"""
import base64
import json
import math
import struct
import sys
import zlib


def load(path):
    g = json.load(open(path))
    buf = base64.b64decode(g['buffers'][0]['uri'].split(',', 1)[1])

    def acc(i):
        a = g['accessors'][i]
        v = g['bufferViews'][a['bufferView']]
        o = v['byteOffset']
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        fmt = {5126: 'f', 5123: 'H'}[a['componentType']]
        vals = struct.unpack_from('<%d%s' % (a['count'] * n, fmt), buf, o)
        return [vals[k * n:(k + 1) * n] for k in range(a['count'])] if n > 1 else list(vals)
    return g, acc


def read_png(data):
    # Only what jade2gltf writes: 8-bit RGBA, filter 0 per row.
    w, h = struct.unpack('>II', data[16:24])
    p, idat = 8, b''
    while p < len(data):
        n = struct.unpack('>I', data[p:p + 4])[0]
        if data[p + 4:p + 8] == b'IDAT':
            idat += data[p + 8:p + 8 + n]
        p += 12 + n
    raw = zlib.decompress(idat)
    return w, h, b''.join(raw[y * (4 * w + 1) + 1:(y + 1) * (4 * w + 1)] for y in range(h))


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main(argv):
    src, out = argv[0], argv[1]
    width = int(argv[2]) if len(argv) > 2 and argv[2].isdigit() else 1200
    only = argv[argv.index('--node') + 1] if '--node' in argv else None
    yaw = math.radians(float(argv[argv.index('--yaw') + 1])) if '--yaw' in argv else math.radians(35)
    pitch = math.radians(float(argv[argv.index('--pitch') + 1])) if '--pitch' in argv else math.radians(30)
    g, acc = load(src)
    tris = []  # (3 world points, colour, uvs, texture)
    texs = []
    for mat in g.get('materials', []):
        t = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture')
        if t is None:
            texs.append(None)
            continue
        uri = g['images'][g['textures'][t['index']]['source']]['uri']
        texs.append(read_png(base64.b64decode(uri.split(',', 1)[1])))

    def mul(a, b):  # column-major 4x4
        return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]

    def local(node):
        if 'matrix' in node:
            return node['matrix']
        x, y, z, w = node.get('rotation', [0, 0, 0, 1])
        t = node.get('translation', [0, 0, 0])
        return [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
                2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
                2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
                t[0], t[1], t[2], 1]

    def visit(i, parent):
        node = g['nodes'][i]
        m = mul(parent, local(node))
        if 'mesh' in node and (not only or node.get('name') == only):
            for prim in g['meshes'][node['mesh']]['primitives']:
                pos = acc(prim['attributes']['POSITION'])
                col = acc(prim['attributes']['COLOR_0'])
                uvs = acc(prim['attributes']['TEXCOORD_0'])
                tex = texs[prim['material']] if 'material' in prim else None
                idx = acc(prim['indices'])
                P = []
                for p in pos:
                    w = [sum(m[c * 4 + r] * (list(p) + [1])[c] for c in range(4)) for r in range(3)]
                    P.append((w[0], -w[2], w[1]))  # glTF Y-up -> Z-up for display
                for k in range(0, len(idx), 3):
                    a, b, c = idx[k], idx[k + 1], idx[k + 2]
                    cc = tuple((col[a][j] + col[b][j] + col[c][j]) / 3 for j in range(3))
                    tris.append((P[a], P[b], P[c], cc, (uvs[a], uvs[b], uvs[c]), tex))
        for ch in node.get('children', []):
            visit(ch, m)
    for root in g['scenes'][g.get('scene', 0)]['nodes']:
        visit(root, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
    if not tris:
        print('nothing to draw')
        return
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)

    def view(p):
        x = p[0] * cy - p[1] * sy
        yy = p[0] * sy + p[1] * cy
        return (x, p[2] * cp - yy * sp, p[2] * sp + yy * cp)  # screen x, screen y (up), depth (towards viewer)
    V = [(view(a), view(b), view(c), cc, uv, tex) for a, b, c, cc, uv, tex in tris]
    xs = [p[0] for t in V for p in t[:3]]
    ys = [p[1] for t in V for p in t[:3]]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    scale = (width - 20) / max(x1 - x0, 1e-6)
    height = max(64, min(4000, int((y1 - y0) * scale) + 20))
    zbuf = [-1e30] * (width * height)
    rgb = bytearray([24, 24, 32] * (width * height))
    light = (0.4, 0.5, 0.75)
    for a, b, c, cc, uv, tex in V:
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        shade = 0.35 + 0.65 * abs(nx * light[0] + ny * light[1] + nz * light[2]) / nl
        colour = [max(0, min(255, int(255 * shade * ch))) for ch in cc]
        pts = [((p[0] - x0) * scale + 10, (y1 - p[1]) * scale + 10, p[2]) for p in (a, b, c)]
        minx, maxx = max(0, int(min(p[0] for p in pts))), min(width - 1, int(max(p[0] for p in pts)) + 1)
        miny, maxy = max(0, int(min(p[1] for p in pts))), min(height - 1, int(max(p[1] for p in pts)) + 1)
        (ax, ay, az), (bx, by, bz), (qx, qy, qz) = pts
        den = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy)
        if abs(den) < 1e-9:
            continue
        for py in range(miny, maxy + 1):
            for px in range(minx, maxx + 1):
                w0 = ((by - qy) * (px - qx) + (qx - bx) * (py - qy)) / den
                w1 = ((qy - ay) * (px - qx) + (ax - qx) * (py - qy)) / den
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * az + w1 * bz + w2 * qz
                i = py * width + px
                if z > zbuf[i]:
                    if tex:
                        tw, th, tp = tex
                        u = w0 * uv[0][0] + w1 * uv[1][0] + w2 * uv[2][0]
                        v = w0 * uv[0][1] + w1 * uv[1][1] + w2 * uv[2][1]
                        k = 4 * ((int(v * th) % th) * tw + (int(u * tw) % tw))
                        if tp[k + 3] < 128:
                            continue
                        px_col = bytes(max(0, min(255, int(tp[k + j] * shade * cc[j]))) for j in range(3))
                    else:
                        px_col = bytes(colour)
                    zbuf[i] = z
                    rgb[3 * i:3 * i + 3] = px_col
    write_png(out, width, height, rgb)
    print(f'{len(tris)} triangles -> {out} ({width}x{height})')


if __name__ == '__main__':
    main(sys.argv[1:])
