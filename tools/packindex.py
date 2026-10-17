"""Index of the PS2 bigfile's world packs: which pack (key 0xFF......) holds each texture record.

After a save-state load the game makes no bigfile lookups, so the live native renderer cannot tell
which world pack holds a texture key; this index tells it (pop_native_live.cpp reads the file named by POP_PACK_INDEX).
Takes about a minute with 6 workers.

Usage: python3 -I tools/packindex.py [OUT=game/analysis/packindex.txt] [--workers N]
Output: text, one line per texture record: "<texture key> <pack key>" (hex). A key held by several
packs is listed once per pack.
"""
import os
import struct
import sys
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import jadetex  # noqa: E402

BIGFILE = os.path.join(ROOT, 'game', 'ps2', 'disc', 'DATA', 'PRINCE.BF')


def pack_keys():
    with open(BIGFILE, 'rb') as f:
        head = f.read(0x44)
        n = struct.unpack_from('<I', head, 8)[0]
        table = f.read(8 * n)
    return sorted(k for _, k in (struct.unpack_from('<II', table, 8 * i) for i in range(n)) if k >> 24 == 0xFF)


def textures(pack_key):
    try:
        pack = jadetex.unpack_chunks(jadetex.big_entry(BIGFILE, pack_key))
        recs = jadetex.records(pack)
    except Exception as e:  # noqa: BLE001
        return pack_key, None, f'{type(e).__name__}: {e}'
    return pack_key, [k for k, (o, _) in recs.items() if jadetex.is_texture(pack, o)], None


def main(argv):
    workers = 6
    if '--workers' in argv:
        i = argv.index('--workers')
        workers = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]
    out = argv[0] if argv else os.path.join(ROOT, 'game', 'analysis', 'packindex.txt')
    keys = pack_keys()
    lines, failed = [], 0
    with Pool(workers) as pool:
        for i, (pk, texs, err) in enumerate(pool.imap_unordered(textures, keys), 1):
            if err:
                failed += 1
                print(f'{pk:08x}: {err}', file=sys.stderr)
                continue
            lines += [f'{t:08x} {pk:08x}' for t in texs]
            if i % 50 == 0:
                print(f'{i}/{len(keys)} packs, {len(lines)} texture records', file=sys.stderr)
    lines.sort()
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{len(keys)} packs ({failed} failed), {len(lines)} texture records -> {out}')


if __name__ == '__main__':
    main(sys.argv[1:])
