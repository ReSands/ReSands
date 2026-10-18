"""Write the VU0 and VU1 code images held in pop-sotc save states as vu1_recomp inputs.

Each save holds the VU0 (4 KB) and VU1 (16 KB) micro memory as it was when the save was made. This
writes them as vu0_<hash>.bin / vu1_<hash>.bin (FNV-1a 64 of the whole image, the key the runtime
looks compiled programs up by, as PS2X_VU1_CAPTURE names them) into OUTDIR, skipping images already
there, and prints how many saves had each image. An image only seen during play between saves is not
found this way; PS2X_VU1_CAPTURE runs catch those.

Usage: python3 -I tools/vuimages.py OUTDIR STATE... [--vu0-only]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    vu0_only = '--vu0-only' in sys.argv
    if len(args) < 2:
        print(__doc__)
        return 2
    out, states = args[0], args[1:]
    os.makedirs(out, exist_ok=True)
    seen = {}
    for path in states:
        s = State(path)
        units = [('vu0', s.vu0code)] if vu0_only else [('vu0', s.vu0code), ('vu1', s.vu1code)]
        for unit, code in units:
            if not any(code):
                continue
            name = f'{unit}_{fnv1a64(code):016x}.bin'
            seen.setdefault(name, []).append(os.path.basename(path))
            target = os.path.join(out, name)
            if not os.path.exists(target):
                with open(target, 'wb') as f:
                    f.write(code)
    for name, where in sorted(seen.items()):
        print(f'{name}: {len(where)} saves (first {where[0]})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
