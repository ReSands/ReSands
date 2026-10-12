#!/usr/bin/env python3
"""Carry function names from the Aug 2003 PS2 prototype (unstripped ELF) to the retail ELF.

Matches retail functions (from the Ghidra export's functions.csv) to prototype functions by
their instructions, with 16-bit immediates and jump targets masked so code that moved between
builds still matches. Only unique matches are kept. CodeWarrior-mangled names are split into a
namespace path and a short function name, so the Ghidra export (which writes the short name)
stays a plain identifier.

Names PS2Recomp would replace with its own stubs (the recompiler's call list and the exporter's
runtime handler list) get a "pop_" prefix, so the recompiled code stays the same.

Usage: tools/proto_names.py <proto ELF> <retail ELF> <functions.csv> <PS2Recomp dir[:dir...]> <out.tsv>
Output columns: retail address, namespace (:: separated, may be empty), name, mangled name.
"""
import collections
import csv
import re
import struct
import sys
from pathlib import Path

MASK_KEEP_IMM = {0x00, 0x10, 0x11, 0x12, 0x13, 0x1C}  # SPECIAL, COP0-2, COP1X, MMI
PREFIX_LEN = 8  # instructions hashed for the first lookup

OPERATORS = {
    "__as": "operator=", "__eq": "operator==", "__ne": "operator!=", "__lt": "operator<",
    "__gt": "operator>", "__le": "operator<=", "__ge": "operator>=", "__pl": "operator+",
    "__mi": "operator-", "__ml": "operator*", "__dv": "operator/", "__md": "operator%",
    "__apl": "operator+=", "__ami": "operator-=", "__amu": "operator*=", "__adv": "operator/=",
    "__vc": "operator[]", "__cl": "operator()", "__nw": "operator_new", "__dl": "operator_delete",
    "__nwa": "operator_new[]", "__dla": "operator_delete[]", "__rf": "operator->",
    "__nt": "operator!", "__aa": "operator&&", "__oo": "operator||", "__ad": "operator&",
    "__or": "operator|", "__er": "operator^", "__co": "operator~", "__ls": "operator<<",
    "__rs": "operator>>", "__pp": "operator++", "__mm": "operator--",
}


def load_elf(path):
    data = Path(path).read_bytes()
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phnum, = struct.unpack_from("<H", data, 0x2C)
    segs = []
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, *_ = struct.unpack_from("<8I", data, phoff + i * 32)
        if p_type == 1:
            segs.append((vaddr, data[off:off + filesz]))

    def read(addr, size):
        for vaddr, blob in segs:
            if vaddr <= addr and addr + size <= vaddr + len(blob):
                return blob[addr - vaddr:addr - vaddr + size]
        return None

    return data, read


def elf_functions(data):
    shoff, = struct.unpack_from("<I", data, 0x20)
    shnum, = struct.unpack_from("<H", data, 0x30)
    sections = [struct.unpack_from("<10I", data, shoff + i * 40) for i in range(shnum)]
    symtab = next(s for s in sections if s[1] == 2)
    strtab = sections[symtab[6]]
    funcs = []
    for i in range(symtab[5] // 16):
        name_off, value, size, info, _, _ = struct.unpack_from("<IIIBBH", data, symtab[4] + i * 16)
        if info & 0xF == 2 and size >= 8:
            end = data.index(b"\0", strtab[4] + name_off)
            funcs.append((value, size, data[strtab[4] + name_off:end].decode("latin1")))
    return funcs


def mask(code):
    out = []
    for (word,) in struct.iter_unpack("<I", code):
        op = word >> 26
        if op in (2, 3):
            word &= 0xFC000000
        elif op not in MASK_KEEP_IMM:
            word &= 0xFFFF0000
        out.append(word)
    return tuple(out)


def strip_template(name):
    depth, out = 0, []
    for c in name:
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
        elif depth == 0:
            out.append(c)
    return "".join(out)


def split_base(mangled):
    """Splits 'base__<class><signature>' at the '__' that ends the base name."""
    depth = 0
    start = 2 if mangled.startswith("__") else 1
    for i in range(start, len(mangled) - 1):
        c = mangled[i]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
        elif depth == 0 and mangled.startswith("__", i) and i + 2 < len(mangled):
            nxt = mangled[i + 2]
            if nxt.isdigit() or nxt in "QFC":
                return mangled[:i], mangled[i + 2:]
    return None, None


def read_class(rest):
    """Parses '<len><name>' or 'Q<n><len><name>...'. Returns (components, remainder)."""
    def one(s):
        m = re.match(r"(\d+)", s)
        if not m:
            return None, s
        n = int(m.group(1))
        body = s[m.end():m.end() + n]
        return (body, s[m.end() + n:]) if len(body) == n else (None, s)

    if rest.startswith("Q"):
        m = re.match(r"Q(\d)", rest)
        if not m:
            return [], rest
        rest = rest[2:]
        parts = []
        for _ in range(int(m.group(1))):
            part, rest = one(rest)
            if part is None:
                return [], rest
            parts.append(part)
        return parts, rest
    if rest[:1].isdigit():
        part, rest = one(rest)
        return ([part] if part else []), rest
    return [], rest


def demangle(mangled):
    """Returns (namespace components, short name) for a CodeWarrior name."""
    base, rest = split_base(mangled)
    if base is None:
        return [], mangled
    classes, _ = read_class(rest)
    short_classes = [strip_template(c) for c in classes]
    if base == "__ct" and classes:
        name = short_classes[-1]
    elif base == "__dt" and classes:
        name = "~" + short_classes[-1]
    else:
        name = OPERATORS.get(base, strip_template(base))
    return classes, name


def stub_names(recomp_dirs):
    """Names PS2Recomp replaces by name: the recompiler's call list and the exporter's list."""
    names = set()
    for recomp_dir in recomp_dirs.split(":"):
        names |= stub_names_in(recomp_dir)
    return names


def stub_names_in(recomp_dir):
    names = set()
    call_list = Path(recomp_dir, "ps2xRuntime/include/ps2_call_list.h").read_text()
    names.update(re.findall(r"X\((\w+)\)", call_list))
    export = Path(recomp_dir, "ps2xRecomp/tools/ghidra/ExportPS2Functions.java").read_text()
    block = export.split("RUNTIME_HANDLER_NAMES = new HashSet<>(Arrays.asList(", 1)[1].split("));", 1)[0]
    names.update(re.findall(r'"(\w+)"', block))
    return {n for n in names if n}


def collides(name, stubs):
    bare = name.lstrip("_")
    return name in stubs or bare in stubs or ("_" + bare) in stubs or ("__" + bare) in stubs


def main():
    proto_elf, retail_elf, functions_csv, recomp_dir, out_path = sys.argv[1:6]
    proto_data, proto_read = load_elf(proto_elf)
    _, retail_read = load_elf(retail_elf)

    index = collections.defaultdict(list)
    for addr, size, name in elf_functions(proto_data):
        code = proto_read(addr, size)
        if code:
            index[mask(code[:PREFIX_LEN * 4])].append((size, name, mask(code)))

    stubs = stub_names(recomp_dir)
    rows, ambiguous, prefixed = [], 0, []
    for rec in csv.DictReader(open(functions_csv)):
        addr = int(rec["Start"], 16)
        head = retail_read(addr, PREFIX_LEN * 4)
        if not head:
            continue
        names = set()
        for size, name, full in index.get(mask(head), []):
            code = retail_read(addr, size)
            if code and mask(code) == full:
                names.add(name)
        if len(names) > 1:
            ambiguous += 1
        if len(names) != 1:
            continue
        mangled = names.pop()
        namespace, short = demangle(mangled)
        if collides(short, stubs):
            prefixed.append(short)
            short = "pop_" + short
        rows.append((addr, "::".join(namespace), short, mangled))

    with open(out_path, "w") as f:
        for addr, ns, short, mangled in rows:
            f.write(f"0x{addr:08X}\t{ns}\t{short}\t{mangled}\n")
    print(f"{len(rows)} unique matches, {ambiguous} ambiguous, "
          f"{len(prefixed)} prefixed pop_ (stub-name collisions): {' '.join(sorted(set(prefixed)))}")


if __name__ == "__main__":
    main()
