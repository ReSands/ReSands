"""PS2 lightmap pages (texture format 6: PSMT4 uploaded as PSMCT32 inside the record) -> grey RGBA."""
import glob
SOURCES = [('jadetex', ['RECORD_MARKER', 'TEXTURE_MAGIC', 'records', 'is_texture', 'ps2_lightmap']),
           ('gsvram', ['BLOCK', 'COLUMN', 'addr']), ('gslm', ['BLOCK4', 'COLUMN4', 'addr4'])]
REPLACE = [('    import gslm  # GS address layout (PSMT4)\n', ''), ('    import gsvram  # GS address layout (PSMCT32)\n', ''),
           ('gsvram.addr(', 'addr('), ('gslm.addr4(', 'addr4(')]
NOTES = """- The input is a whole unpacked world pack (records() as in the reference; dict order = first
  position of a key, last value).
- addr/addr4 are the GS memory layouts (PSMCT32 / PSMT4); the 4 MB memory model must behave exactly
  as the bytearray in the reference (addresses wrap modulo 4 MB as written).
- fnv64 = FNV-1a 64-bit (basis 0xcbf29ce484222325, prime 0x100000001b3)."""


def inputs(root):
    return sorted(glob.glob(root + '/game/analysis/meshdecode/tests/*.bin'))


# --- canon
def fnv64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def canon(data):
    recs = records(data)
    lines = []
    for key in sorted(recs):
        off, size = recs[key]
        if size < 0x40 or not is_texture(data, off) or struct.unpack_from('<H', data, off + 8)[0] != 6:
            continue
        try:
            w, h, rgba = ps2_lightmap(data, off, size)
            lines.append('%08x %d %d %016x' % (key, w, h, fnv64(rgba)))
        except Exception:
            lines.append('%08x X' % key)
    return '\n'.join(lines) + '\n'
