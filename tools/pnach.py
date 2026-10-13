#!/usr/bin/env python3
"""Apply sections of a PCSX2 .pnach patch file to a copy of the ELF, before the recompile.

PCSX2 patches overwrite words in EE memory while the game runs. Our code is compiled ahead
of time, so code patches must be in the ELF the recompiler reads. Only code patches are
accepted: at boot the runtime loads memory from the original ELF on the disc (the game
override is keyed to its CRC), so a data word patched here would never reach the game. Data
patches (e.g. the 60 FPS flag) belong in the override, written at run time.

A patched `jal`/`j` whose target is not a function start (the SLUS-20743 widescreen menu fix
calls 4 bytes before a function, into padding the patch fills) gets a new function entry in
functions.csv, ending where the function it runs into ends, so the recompiler emits it.

Usage: tools/pnach.py <ELF in> <.pnach> <ELF out> <functions.csv> <section>[;<section>...]
Section names are the [headers] in the .pnach, e.g. "Widescreen 16:9;Remove Blur/Bloom".
The .pnach files come from https://github.com/PCSX2/pcsx2_patches (patches/<serial>_<crc>.pnach).
"""
import bisect
import csv
import re
import struct
import sys


def read_sections(path):
    sections, current = {}, None
    for raw in open(path, encoding='utf-8', errors='replace'):
        line = raw.split('//', 1)[0].strip()
        m = re.fullmatch(r'\[(.+)\]', line)
        if m:
            current = sections.setdefault(m.group(1).strip(), [])
            continue
        m = re.fullmatch(r'patch\s*=\s*(\d+)\s*,\s*EE\s*,\s*([0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*([0-9A-Fa-f]+)', line)
        if m and current is not None:
            if m.group(3).lower() != 'word':
                sys.exit(f'unsupported patch type {m.group(3)!r}: {raw.strip()}')
            current.append((int(m.group(2), 16) & 0x01FFFFFF, int(m.group(4), 16)))
    return sections


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    elf_in, pnach, elf_out, csv_path, wanted = sys.argv[1:]
    sections = read_sections(pnach)
    names = [s.strip() for s in wanted.split(';') if s.strip()]
    missing = [n for n in names if n not in sections]
    if missing:
        sys.exit(f'sections not in {pnach}: {missing}; available: {list(sections)}')

    data = bytearray(open(elf_in, 'rb').read())
    phoff, = struct.unpack_from('<I', data, 0x1C)
    phnum, = struct.unpack_from('<H', data, 0x2C)
    segments = []
    for i in range(phnum):
        ptype, off, vaddr, _, filesz, _, _, _ = struct.unpack_from('<8I', data, phoff + i * 32)
        if ptype == 1:
            segments.append((vaddr, off, filesz))

    def file_offset(addr):
        for vaddr, off, filesz in segments:
            if vaddr <= addr and addr + 4 <= vaddr + filesz:
                return off + addr - vaddr
        sys.exit(f'patch address {addr:#x} is outside the ELF file image')

    with open(csv_path, newline='') as f:
        rows = list(csv.DictReader(f))
    starts = sorted(int(r['Start'], 16) for r in rows)
    ends = {int(r['Start'], 16): int(r['End'], 16) for r in rows}

    new_functions = []
    def in_code(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and addr < ends[starts[i]]

    patches = [(n, a, v) for n in names for a, v in sections[n]]
    jal_targets = {(a & 0xF0000000) | ((v & 0x03FFFFFF) << 2) for _, a, v in patches if v >> 26 in (2, 3)}
    data_patches = [(n, a) for n, a, _ in patches if not in_code(a) and not any(t <= a < t + 8 for t in jal_targets)]
    if data_patches:
        sys.exit('data patches (apply them at run time in the override instead): '
                 + ', '.join(f'{n} {a:#x}' for n, a in data_patches))

    for name in names:
        for addr, value in sections[name]:
            off = file_offset(addr)
            old, = struct.unpack_from('<I', data, off)
            struct.pack_into('<I', data, off, value)
            print(f'  {name}: {addr:#010x} {old:08x} -> {value:08x}')
            if value >> 26 in (2, 3):  # j / jal
                target = (addr & 0xF0000000) | ((value & 0x03FFFFFF) << 2)
                if target not in ends and target not in [t for t, _ in new_functions]:
                    nxt = starts[bisect.bisect_right(starts, target)]
                    new_functions.append((target, ends[nxt]))

    open(elf_out, 'wb').write(data)
    if new_functions:
        with open(csv_path, 'a', newline='') as f:
            w = csv.writer(f, lineterminator='\n')
            for start, end in new_functions:
                w.writerow([f'pop_pnach_{start:08x}', f'0x{start:08X}', f'0x{end:08X}', end - start])
                print(f'  new function entry {start:#010x}-{end:#010x} (patched call target)')
    print(f'{sum(len(sections[n]) for n in names)} words patched ({"; ".join(names)}) -> {elf_out}')


if __name__ == '__main__':
    main()
