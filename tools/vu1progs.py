"""Write PoP's VU microprograms, taken straight from the game's ELF, as vu1_recomp inputs.

PoP keeps every VU microprogram in the ELF as a DMA packet of VIF MPG commands that loads it at VU address
0 (found 19 Oct 2026: 0x4CD50C-0x4E4DEC in SLUS_207.43). A program overwrites only its own length, so the
16 KB VU1 code image the runtime sees is the current program followed by whatever an earlier, longer
program left behind. Hashing the whole image therefore gave a new "program" for each combination (the
death scene of 19 Oct ran on the interpreter that way); the runtime also matches a compiled program by its
zero-padded prefix (upstream patch 33), so compiling each program once, zero-padded, covers every
combination, including scenes nobody has recorded. This is also how a new user gets the images: from their
own ELF, nothing to ship.

Writes vu1_<hash>.bin (16 KB) for every program loaded at 0, and also vu0_<hash>.bin (4 KB) for those that
fit VU0's 4 KB (the packet does not say which VIF it goes to; an unused registration costs only build time).
Hash = FNV-1a 64 of the padded image, as PS2X_VU1_CAPTURE names them. Uploads at other addresses (patches
into a loaded program) are listed, not written: an image containing one falls back to the interpreter.

Usage: python3 -I tools/vu1progs.py ELF OUTDIR [--check CAPTUREDIR]
  --check: report, for each captured vu1_*.bin, which program it starts with (all must match one).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vuimages import fnv1a64  # noqa: E402

VU1_SIZE = 0x4000
VU0_SIZE = 0x1000


def load_segments(elf):
    phoff = struct.unpack_from('<I', elf, 0x1C)[0]
    count = struct.unpack_from('<H', elf, 0x2C)[0]
    segs = []
    for i in range(count):
        kind, off, vaddr, _, filesz, _, _, _ = struct.unpack_from('<8I', elf, phoff + i * 32)
        if kind == 1 and filesz:
            segs.append((off, vaddr, filesz))
    return segs


def find_programs(elf):
    """MPG chains: [vaddr of first tag, load address, code bytes]. A tag continues the previous chain
    when it loads right after it; data follows each tag 8-byte aligned (tag in the upper word)."""
    progs = []
    for off, vaddr, size in load_segments(elf):
        cur = None
        p = off + ((4 - off) % 8)
        while p + 4 <= off + size:
            w = struct.unpack_from('<I', elf, p)[0]
            if w >> 24 == 0x4A:
                num = (w >> 16) & 0xFF or 256
                addr = (w & 0xFFFF) * 8
                if addr + num * 8 <= VU1_SIZE:
                    data = elf[p + 4:p + 4 + num * 8]
                    if cur is not None and cur[3] == p and addr == cur[1] + len(cur[2]):
                        cur[2] += data
                    else:
                        cur = [p - off + vaddr, addr, bytearray(data), 0]
                        progs.append(cur)
                    # the next tag of the same packet sits in the upper word of the following quadword
                    cur[3] = p + 4 + num * 8 + 4
                    p += 4 + num * 8 + 4
                    continue
            p += 8
    return [(v, a, bytes(d)) for v, a, d, _ in progs]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = None
    if '--check' in sys.argv:
        check = sys.argv[sys.argv.index('--check') + 1]
        args.remove(check)
    if len(args) != 2:
        print(__doc__)
        return 2
    elf_path, out = args
    elf = open(elf_path, 'rb').read()
    progs = find_programs(elf)
    os.makedirs(out, exist_ok=True)
    whole = []
    for vaddr, addr, code in progs:
        if addr != 0:
            # mostly data words that happen to look like MPG; the two in the program table (0x4E4A6C,
            # 0x4E50EC) are real small uploads, not seen in any capture so far
            print(f'0x{vaddr:06X}: MPG-like word, 0x{len(code):X} bytes at VU 0x{addr:04X} (not at 0, not written)')
            continue
        names = []
        for unit, size in (('vu1', VU1_SIZE), ('vu0', VU0_SIZE)):
            if len(code) > size:
                continue
            image = code + bytes(size - len(code))
            name = f'{unit}_{fnv1a64(image):016x}.bin'
            with open(os.path.join(out, name), 'wb') as f:
                f.write(image)
            names.append(name)
        whole.append((vaddr, code))
        print(f'0x{vaddr:06X}: program of 0x{len(code):X} bytes -> {" ".join(names)}')
    if check:
        bad = 0
        for name in sorted(os.listdir(check)):
            if not (name.startswith('vu1_') and name.endswith('.bin')):
                continue
            image = open(os.path.join(check, name), 'rb').read()
            hits = [(len(c), v) for v, c in whole if image[:len(c)] == c]
            if hits:
                length, vaddr = max(hits)
                print(f'{name}: program 0x{vaddr:06X} (0x{length:X} bytes) + earlier tail')
            else:
                bad += 1
                print(f'{name}: NO PROGRAM MATCHES')
        print(f'{bad} captured images without a program')
        return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
