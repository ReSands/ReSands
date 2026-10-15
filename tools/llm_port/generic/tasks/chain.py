"""DMA chain captures (POP_DMACAP files): every record and the VIF UNPACKs it carries."""
import glob
SOURCES = [('jade2gltf', ['EL']), ('dmacmp', ['vif_unpacks'])]
REPLACE = [('jade2gltf.EL', 'EL')]
NOTES = """- Input: a capture file of records: u32 tag address, 16-byte DMA tag (4 u32), u32 data address,
  u32 size, then size bytes of data (see canon). fnv64 = FNV-1a 64 (basis 0xcbf29ce484222325, prime
  0x100000001b3)."""


def inputs(root):
    return sorted(glob.glob(root + '/game/analysis/drawlist/pc_10*/chain.bin.2.0'))


# --- canon
def fnv64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def canon(data):
    out = []
    p = 0
    while p + 28 <= len(data):
        pc, t0, t1, t2, t3, da, sz = struct.unpack_from('<7I', data, p)
        payload = data[p + 28:p + 28 + sz]
        line = '%x %08x %08x %08x %08x %x %x' % (pc, t0, t1, t2, t3, da, sz)
        for f, num, addr, pl in vif_unpacks((t0, t1, t2, t3), payload):
            line += ' | %x %d %x %016x' % (f, num, addr, fnv64(pl))
        out.append(line)
        p += 28 + sz
    return '\n'.join(out) + '\n'
