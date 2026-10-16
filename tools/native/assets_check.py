"""Checks the C++ texture decoder (pop_assets_test) against tools/jadetex.py on unpacked packs: every
texture record must decode to the same RGBA (FNV-1a 64), or fail in both.
Usage: python3 -I tools/native/assets_check.py <pop_assets_test> <pack.bin>..."""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import jadetex  # noqa: E402


def fnv(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def main(argv):
    exe, total, bad = argv[0], 0, 0
    for path in argv[1:]:
        d = open(path, 'rb').read()
        recs = jadetex.records(d)
        cpp = {l.split()[0]: l.split()[2:] for l in subprocess.run([exe, path], capture_output=True, text=True).stdout.splitlines()}
        for key in sorted(recs):
            off, _ = recs[key]
            if not jadetex.is_texture(d, off):
                continue
            total += 1
            try:
                w, h, rgba = jadetex.decode(d, recs, key, 'ps2')
                want = ['%dx%d' % (w, h), '%016x' % fnv(rgba)]
            except Exception:  # noqa: BLE001
                want = ['fail']
            got = cpp.get('%08x' % key)
            if got != want:
                bad += 1
                if bad <= 5:
                    print(os.path.basename(path), '%08x' % key, 'C++', got, 'Python', want)
    print(f'{total} textures, {total - bad} identical, {bad} differ')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
