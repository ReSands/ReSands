"""PS2 8-bit texture decoder (format 3: GS-swizzled indices + CSM1 palette) -> RGBA, per texture."""
import glob
SOURCES = [('jadetex', ['RECORD_MARKER', 'TEXTURE_MAGIC', 'records', 'is_texture', 'image_data', 'unswizzle8', 'ps2_texture'])]
REPLACE = []
NOTES = """- The input is a whole unpacked world pack. Dict order: records() keeps the first position of a
  key and the last value if a key repeats (Python dict semantics).
- fnv64 is FNV-1a 64-bit (offset basis 0xcbf29ce484222325, prime 0x100000001b3) over the bytes.
- Any exception while decoding one texture prints '<key> X' for it and continues."""


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
        if size < 0x40 or not is_texture(data, off):
            continue
        if struct.unpack_from('<H', data, off + 8)[0] == 6:  # lightmap pages: another task
            continue
        try:
            w, h, rgba = ps2_texture(data, recs, off)
            lines.append('%08x %d %d %016x' % (key, w, h, fnv64(rgba)))
        except Exception:
            lines.append('%08x X' % key)
    return '\n'.join(lines) + '\n'
