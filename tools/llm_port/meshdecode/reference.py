"""Reference output for the mesh decoder port: the same canonical text as driver.cpp prints, from the
Python functions in tools/jade2gltf.py, plus the test inputs.

Usage: python3 -I tools/llm_port/meshdecode/reference.py <prince.bf> <outdir> <pack key hex> ...
  For each key: <outdir>/<key>.bin (unpacked pack), <key>.jobs (element offsets, from the converted
  ReadElement), <key>.expected (canonical text).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import jade2gltf  # noqa: E402
import jadetex  # noqa: E402


def f32(x):
    return ' %08x' % struct.unpack('<I', struct.pack('<f', x))[0]


def f64(x):
    return ' %016x' % struct.unpack('<Q', struct.pack('<d', x))[0]


def batches_text(bs):
    out = ''
    for b in bs:
        out += ' B'
        for fmt in sorted(b):
            addr, raw, n = b[fmt]
            out += ' %x:%x:%u:%s' % (fmt, addr, n, raw.hex())
    return out


def element_text(d, o):
    lines = ['J %u' % o]
    try:
        nb, lods, passes = jade2gltf.element_parts(d, o)
    except Exception:
        return lines + ['parts X']
    lines.append('parts %u' % nb + ''.join(' L%u:%u:%u' % (v, lo, ls) for v, lo, ls in lods)
                 + ''.join(' P%u:%u:%u' % (i, po, ps) for i, po, ps in passes))
    blobs = {x: (lo, ls) for x, lo, ls in lods}
    for v, lo, ls in lods:
        if v != 3:
            continue
        try:
            v3 = jade2gltf.variant3_batches(d, lo, ls)
        except Exception:
            lines.append('v3 %u X' % lo)
            continue
        t = 'v3 %u' % lo
        for pos, uvs, flags in v3:
            t += ' B' + ''.join(f32(c) for p in pos for c in p) + ' |' + ''.join(' %d' % c for uv in uvs for c in uv) \
                + ' |' + ''.join(' %d' % c for c in flags)
        lines.append(t)
    for _i, po, ps in passes:
        for with_blobs in (0, 1):
            try:
                bs = jade2gltf.decode_packet(d, po, ps, blobs if with_blobs else None)
            except Exception:
                lines.append('pkt %u %d X' % (po, with_blobs))
                continue
            lines.append('pkt %u %d' % (po, with_blobs) + batches_text(bs))
            if not with_blobs:
                continue
            for scale in (4096.0, 512.0):
                for b in bs:
                    try:
                        m = jade2gltf.batch_mesh(b, scale)
                    except Exception:
                        m = None
                    if m is None:
                        lines.append('mesh %g X' % scale)
                        continue
                    pos, uv, col, tris = m
                    lines.append('mesh %g' % scale + ''.join(f32(c) for p in pos for c in p)
                                 + ' |' + ''.join(f64(c) for u in uv for c in u)
                                 + ' |' + ''.join(f64(c) for k in col for c in k)
                                 + ' |' + ''.join(' %u' % c for t in tris for c in t))
        try:
            sk = jade2gltf.skinned_batches(d, po, ps)
        except Exception:
            lines.append('skin %u X' % po)
            continue
        t = 'skin %u' % po
        for pos, uv, fl, bones, weights, normals in sk:
            t += ' B' + ''.join(f32(c) for p in pos for c in p) \
                + ' |' + ''.join(' %d' % c for u in uv for c in u) \
                + ' |' + ''.join(' %d' % c for c in fl) \
                + ' |' + ''.join(' (' + ''.join('%d,' % c for c in bt) + ')' for bt in bones) \
                + ' |' + ''.join(f32(c) for w in weights for c in w) \
                + ' |' + ''.join(' %d' % c for nm in normals for c in nm)
        lines.append(t)
    return lines


def main(argv):
    bigfile, outdir, keys = argv[0], argv[1], argv[2:]
    os.makedirs(outdir, exist_ok=True)
    for k in keys:
        key = int(k, 16)
        pack = jadetex.unpack_chunks(jadetex.big_entry(bigfile, key))
        base = os.path.join(outdir, '%08x' % key)
        open(base + '.bin', 'wb').write(pack)
        offsets = [o for _key, o, *_ in jade2gltf.capture_elements(pack)]
        open(base + '.jobs', 'w').write(''.join('%u\n' % o for o in offsets))
        with open(base + '.expected', 'w') as f:
            for o in offsets:
                f.write('\n'.join(element_text(pack, o)) + '\n')
        print('%08x: %d elements' % (key, len(offsets)))


if __name__ == '__main__':
    main(sys.argv[1:])
