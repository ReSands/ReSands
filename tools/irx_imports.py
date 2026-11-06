#!/usr/bin/env python3
"""List the IOP kernel/library imports of IRX modules, with call-site counts.

Usage: irx_imports.py <file> [<file> ...] [--tabs DIR]
A file can be a single .irx or an EE ELF with IRX modules embedded as data (PoP's SLUS_207.43
carries seven); every embedded ELF is split out and listed. Import names come from ps2sdk's
exports.tab files in DIR (default game/analysis/iop/ps2sdk_tabs); unknown ordinals print as #N.
"""
import re
import struct
import sys
from pathlib import Path

IMPORT_MAGIC = 0x41E00000
EXPORT_MAGIC = 0x41C00000
JR_RA = 0x03E00008


def load_tabs(tab_dir):
    names = {}
    for tab in Path(tab_dir).glob('*.tab'):
        lib = None
        for line in tab.read_text(errors='replace').splitlines():
            m = re.search(r'DECLARE_EXPORT_TABLE\((\w+)', line)
            if m:
                lib = m.group(1)
                names[lib] = []
                continue
            m = re.search(r'DECLARE_EXPORT\w*\((\w+)', line)
            if m and lib:
                names[lib].append(m.group(1))
    names.setdefault('thbase', names.get('thbase', []))
    return names


def split_modules(data):
    """Yields (file offset, bytes) for each IRX ELF in data (the file itself if it is one)."""
    offsets = [m.start() for m in re.finditer(rb'\x7fELF\x01\x01\x01', data)]
    for i, off in enumerate(offsets):
        e_type = struct.unpack_from('<H', data, off + 16)[0]
        if e_type != 0xFF80:  # IRX (relocatable IOP module)
            continue
        end = offsets[i + 1] if i + 1 < len(offsets) else len(data)
        yield off, data[off:end]


def text_base(mod):
    """File offset of the module's code (vaddr 0): the first PT_LOAD."""
    phoff, = struct.unpack_from('<I', mod, 28)
    phnum, = struct.unpack_from('<H', mod, 44)
    for i in range(phnum):
        p_type, p_offset = struct.unpack_from('<II', mod, phoff + 32 * i)
        if p_type == 1:
            return p_offset
    raise ValueError('no PT_LOAD')


def tables(mod, magic):
    """(table offset, library name, [(stub offset, ordinal)])."""
    for off in range(0, len(mod) - 28, 4):
        if struct.unpack_from('<I', mod, off)[0] != magic:
            continue
        name = mod[off + 12:off + 20].split(b'\0')[0].decode('latin-1')
        stubs = []
        s = off + 20
        while s + 8 <= len(mod):
            first, second = struct.unpack_from('<II', mod, s)
            if magic == IMPORT_MAGIC and first == JR_RA and second >> 16 == 0x2400:
                stubs.append((s, second & 0xFFFF))
                s += 8
            else:
                break
        if name.isprintable() and name:
            yield off, name, stubs


def module_name(mod, base):
    """The module's own name: from its export table, else the .iopmod name string."""
    for _, name, _ in tables(mod, EXPORT_MAGIC):
        return name
    m = re.search(rb'[A-Za-z][\w ]{3,}\0', mod[base - 0x40:base] if base >= 0x40 else b'')
    return m.group(0)[:-1].decode() if m else '?'


def jal_counts(mod, base, stub_offsets):
    """Calls (jal) into each stub, scanning the code segment."""
    want = {((s - base) >> 2) & 0x3FFFFFF: s for s in stub_offsets}
    counts = dict.fromkeys(stub_offsets, 0)
    for off in range(base, len(mod) - 3, 4):
        w, = struct.unpack_from('<I', mod, off)
        if w >> 26 == 3 and (w & 0x3FFFFFF) in want:
            counts[want[w & 0x3FFFFFF]] += 1
    return counts


def _selftest():
    stub = struct.pack('<I', IMPORT_MAGIC) + b'\0' * 8 + b'thbase\0\0' + b'\0' * 0
    stub = stub + struct.pack('<II', JR_RA, 0x24000004) + struct.pack('<II', 0, 0)
    got = list(tables(stub + b'\0' * 16, IMPORT_MAGIC))
    assert got == [(0, 'thbase', [(20, 4)])], got


def main(argv):
    _selftest()
    tab_dir = 'game/analysis/iop/ps2sdk_tabs'
    if '--tabs' in argv:
        i = argv.index('--tabs')
        tab_dir = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    names = load_tabs(tab_dir)
    for path in argv:
        data = Path(path).read_bytes()
        for off, mod in split_modules(data):
            base = text_base(mod)
            print(f'== {path} +0x{off:x}: {module_name(mod, base)} ({len(mod)} bytes)')
            imports = list(tables(mod, IMPORT_MAGIC))
            counts = jal_counts(mod, base, [s for _, _, st in imports for s, _ in st])
            for _, lib, stubs in imports:
                known = names.get(lib, [])
                items = []
                for s, ordinal in stubs:
                    n = known[ordinal] if ordinal < len(known) else f'#{ordinal}'
                    items.append(f'{n}({ordinal})x{counts[s]}')
                print(f'  {lib}: ' + ', '.join(items))


if __name__ == '__main__':
    main(sys.argv[1:])
