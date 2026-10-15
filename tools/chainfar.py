"""Far layers (sky, distant backdrop) from a captured frame: the draws the game makes under the far
fog constants (VU 0x10/0x18 = (255, 0, a, b) with a large a), decoded from the chain with their
object matrix (VU 0) and textures (GS replay), written as a glTF in world coordinates.
Usage: python3 -I tools/chainfar.py <chain.bin> <out.gltf>   (prints the far fog constants)
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import chainskin  # noqa: E402
import dmacmp  # noqa: E402
import jade2gltf  # noqa: E402


def far_meshes(chain_path, min_a=400.0):
    vram, regs = bytearray(4 << 20), {}
    fog = mat = cur = None
    meshes, consts = [], set()
    for pc, tag, da, data in dmacmp.chain_records(chain_path):
        tid = (tag[0] >> 28) & 7
        chainskin.gs_replay(tag, data, vram, regs)
        if cur is None:
            for f, k, a, p in dmacmp.vif_unpacks(tag, data):
                if f == 0xC and k == 1 and a in (0x10, 0x18) and len(p) >= 16:
                    v = struct.unpack_from('<4f', p, 0)
                    if v[0] == 255.0:
                        fog = v
                if f == 0xC and k == 4 and a == 0 and len(p) >= 64:
                    mat = struct.unpack_from('<16f', p, 0)
        else:
            cur['bytes'] += struct.pack('<4I', *tag) + (data if tid not in (0, 3, 4) else b'')
        if tid == 5:
            cur = {'bytes': b'', 'fog': fog, 'm': mat, 'tex0': regs.get(0x06), 'test': regs.get(0x47, 0),
                   'alpha': regs.get(0x42, 0) & 0xFF, 'tex': None}
            if fog and fog[2] >= min_a and cur['tex0'] is not None:
                cur['tex'] = chainskin.texture(vram, cur['tex0'])
        elif tid == 6 and cur is not None:
            if cur['fog'] and cur['fog'][2] >= min_a and cur['m']:
                consts.add((round(cur['fog'][2], 4), round(cur['fog'][3], 4)))
                for b in jade2gltf.decode_packet(cur['bytes'], 0, len(cur['bytes'])):
                    m = jade2gltf.batch_mesh(b, 4096.0)
                    if not m or not m[3]:
                        continue
                    pos, uv, col, tris = m
                    out = [chainskin.xform(cur['m'], v) for v in pos]
                    raw_uv = [(u * 512.0, v * 512.0) for u, v in uv]  # chainskin.write_gltf divides by 512
                    t = cur['test']
                    # the far packets' V4-8 data at 0x83 is (0, 0, 0, 0x7F) for most batches: not a colour
                    # the VU1 sky program uses (it supplies its own); drawn at 0x80 = 1.0 until that is read
                    if all(c[0] == 0 and c[1] == 0 and c[2] == 0 for c in col):
                        col = [(1.0, 1.0, 1.0, c[3]) for c in col]
                    meshes.append((out, raw_uv, col, tris, 0, cur['tex'], (t & 1, (t >> 4) & 0xFF, cur['alpha'] == 0x44)))
            cur = None
    return meshes, consts


def main(argv):
    meshes, consts = far_meshes(argv[0])
    chainskin.write_gltf(meshes, argv[1])
    print(f'{len(meshes)} far batches; far fog constants (a, b): {sorted(consts)}')


if __name__ == '__main__':
    main(sys.argv[1:])
