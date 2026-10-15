"""Game objects (.gao records): name, placement matrix and the geometry / multi-material they use."""
import glob
SOURCES = [('jadetex', ['RECORD_MARKER', 'records']), ('jade2gltf', ['game_objects'])]
REPLACE = []
NOTES = """- The input is a whole unpacked world pack. records(): dict order = first position of a key, last
  value if a key repeats; game_objects iterates recs.items() in that order (the canon sorts anyway).
- math.isfinite on float32 values read from the data; floats are printed as their IEEE bits.
- name: bytes up to the first NUL, printed as hex."""


def inputs(root):
    return sorted(glob.glob(root + '/game/analysis/meshdecode/tests/*.bin'))


# --- canon
def canon(data):
    recs = records(data)
    geos = {k for k, (o, s) in recs.items() if s >= 8 and struct.unpack_from('<II', data, o) == (1, 7)}
    rows = []
    for name, m, geo, mm, o, s, after in game_objects(data, recs, geos):
        bits = ' '.join('%08x' % struct.unpack('<I', struct.pack('<f', x))[0] for x in m)
        rows.append((o, '%x %s geo %08x mm %08x size %x after %x m %s' % (o, name.encode('latin1').hex(), geo, mm, s, after, bits)))
    return '\n'.join(r for _, r in sorted(rows)) + '\n'
