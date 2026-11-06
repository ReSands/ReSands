#!/usr/bin/env python3
"""Fetch PCSX2's SPU2 core at a pinned commit and build it as a static library for the offline reference.

Usage: build.py --iop-include <ps2xIOP>/include [--out DIR]   (default game/native-build/pcsx2_ref)

Builds libpcsx2_spu2.a: PCSX2's core plus our glue (pcsx2_spu2_backend.cpp, a ps2xIOP Spu2Backend).

PCSX2 (GPL-3.0+) is not copied into this repo: its files are downloaded to game/analysis/pcsx2_spu2/<commit>/SPU2
and compiled with our stand-ins for the rest of PCSX2 (shim/). ReverbResample.cpp's SSE/AVX paths need PCSX2's
GSVector library, so a copy keeping only its plain reference filters (the same maths) is generated beside it.
"""
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

COMMIT = '1ccd6a8dc3574771c138849039d71951a3a1c175'  # PCSX2 master, 6 Nov 2026
FILES = ['ADSR.cpp', 'Dma.cpp', 'Dma.h', 'Mixer.cpp', 'ReadInput.cpp', 'RegTable.cpp', 'Reverb.cpp',
         'ReverbResample.cpp', 'defs.h', 'interpolate_table.h', 'regs.h', 'spdif.h', 'spu2.h', 'spu2sys.cpp', 'Debug.h']
SOURCES = ['ADSR.cpp', 'Dma.cpp', 'Mixer.cpp', 'ReadInput.cpp', 'RegTable.cpp', 'Reverb.cpp', 'spu2sys.cpp',
           'ReverbResample_ref.cpp']
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def fetch(dest):
    dest.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        path = dest / name
        if not path.exists():
            url = f'https://raw.githubusercontent.com/PCSX2/pcsx2/{COMMIT}/pcsx2/SPU2/{name}'
            path.write_bytes(urllib.request.urlopen(url, timeout=60).read())


def reference_resample(text):
    """ReverbResample.cpp without its SSE/AVX paths (they need PCSX2's GSVector), the selectors calling the
    reference filters (the same maths). Relies on PCSX2's style: each function ends with a '}' line."""
    text = text.replace('#include "GS/GSVector.h"', '')
    text = re.sub(r'#if _M_SSE >= 0x501\n.*?#endif\n', '', text, flags=re.S)
    pieces = re.split(r'(?<=\n\}\n)', text)  # each piece ends with one function (or is the tail)
    kept = []
    for piece in pieces:
        if re.search(r'\b\w+_(?:sse|avx)\(V_Core& core', piece):
            continue
        if re.search(r'\ns32 ReverbDownsample\(V_Core& core, bool right\)\n', piece):
            piece = piece[:piece.index('s32 ReverbDownsample(')] + \
                's32 ReverbDownsample(V_Core& core, bool right)\n{\n\treturn ReverbDownsample_reference(core, right);\n}\n'
        elif re.search(r'\nStereoOut32 ReverbUpsample\(V_Core& core\)\n', piece):
            piece = piece[:piece.index('StereoOut32 ReverbUpsample(')] + \
                'StereoOut32 ReverbUpsample(V_Core& core)\n{\n\treturn ReverbUpsample_reference(core);\n}\n'
        kept.append(piece)
    text = ''.join(kept)
    for needed in ('ReverbDownsample_reference(V_Core& core, bool right)', 'ReverbUpsample_reference(V_Core& core)',
                   'return ReverbDownsample_reference(core, right);', 'return ReverbUpsample_reference(core);',
                   'MULTI_ISA_UNSHARED_END'):
        assert needed in text, f'ReverbResample.cpp changed shape: {needed!r} missing'
    assert 'GSVector' not in text and '_sse' not in text and '_avx' not in text
    return text


def main(argv):
    out = Path(argv[argv.index('--out') + 1]) if '--out' in argv else ROOT / 'game/native-build/pcsx2_ref'
    src = ROOT / f'game/analysis/pcsx2_spu2/{COMMIT}'
    fetch(src / 'SPU2')
    (src / 'SPU2/ReverbResample_ref.cpp').write_text(reference_resample((src / 'SPU2/ReverbResample.cpp').read_text()))
    out.mkdir(parents=True, exist_ok=True)
    iop_include = argv[argv.index('--iop-include') + 1]
    objects = []
    for path in [src / 'SPU2' / name for name in SOURCES] + [HERE / 'pcsx2_spu2_backend.cpp']:
        obj = out / (path.stem + '.o')
        subprocess.run(['c++', '-std=c++20', '-O2', '-w', '-fPIC', '-include', str(HERE / 'shim/pcsx2_prelude.h'),
                        '-I', str(HERE / 'shim'), '-I', str(src), '-I', iop_include, '-c', str(path), '-o', str(obj)],
                       check=True)
        objects.append(str(obj))
    lib = out / 'libpcsx2_spu2.a'
    lib.unlink(missing_ok=True)
    subprocess.run(['ar', 'rcs', str(lib), *objects], check=True)
    print(f'{lib} (PCSX2 {COMMIT[:12]}); include {src} and {HERE / "shim"}')


if __name__ == '__main__':
    main(sys.argv[1:])
