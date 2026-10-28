# DWARF version 1 reader (CodeWarrior PS2 .debug sections, e.g. the prototype's POP1.ELF).
# dies(elf bytes) -> {offset: Die}; each Die has tag, attrs {attr: value}, children (offsets).
# Forms are the low nibble of the attribute code, so unknown (vendor) attributes still parse.
import struct
from collections import namedtuple

TAGS = {1: 'array_type', 2: 'class_type', 4: 'enumeration_type', 5: 'formal_parameter', 6: 'global_subroutine',
        7: 'global_variable', 0xa: 'label', 0xb: 'lexical_block', 0xc: 'local_variable', 0xd: 'member',
        0xf: 'pointer_type', 0x10: 'reference_type', 0x11: 'compile_unit', 0x13: 'structure_type',
        0x14: 'subroutine', 0x15: 'subroutine_type', 0x16: 'typedef', 0x17: 'union_type',
        0x18: 'unspecified_parameters', 0x1c: 'inheritance', 0x1d: 'inlined_subroutine', 0x1f: 'ptr_to_member_type'}
AT = {0x001: 'sibling', 0x002: 'location', 0x003: 'name', 0x005: 'fund_type', 0x006: 'mod_fund_type',
      0x007: 'user_def_type', 0x008: 'mod_u_d_type', 0x009: 'ordering', 0x00a: 'subscr_data', 0x00b: 'byte_size',
      0x00c: 'bit_offset', 0x00d: 'bit_size', 0x00f: 'element_list', 0x010: 'stmt_list', 0x011: 'low_pc',
      0x012: 'high_pc', 0x013: 'language', 0x014: 'member', 0x01b: 'comp_dir', 0x025: 'producer',
      0x027: 'prototyped'}
Die = namedtuple('Die', 'off tag attrs children')


def section(d, want):
    shoff, = struct.unpack_from('<I', d, 0x20)
    es, n, si = struct.unpack_from('<HHH', d, 0x2e)
    sh = [struct.unpack_from('<10I', d, shoff + i * es) for i in range(n)]
    st = sh[si][4]
    for s in sh:
        if d[st + s[0]:d.index(b'\0', st + s[0])] == want.encode():
            return d[s[4]:s[4] + s[5]]
    raise KeyError(want)


def parse(dbg):
    out, order, p = {}, [], 0
    while p + 4 <= len(dbg):
        ln, = struct.unpack_from('<I', dbg, p)
        if ln < 8:                      # null entry: ends a sibling chain
            out[p] = Die(p, None, {}, [])
            order.append(p)
            p += max(ln, 4)
            continue
        tag, = struct.unpack_from('<H', dbg, p + 4)
        q, end, attrs = p + 6, p + ln, {}
        while q < end:
            a, = struct.unpack_from('<H', dbg, q)
            q += 2
            form, name = a & 0xf, AT.get(a >> 4, a >> 4)
            if form in (1, 2, 6):
                v, = struct.unpack_from('<I', dbg, q); q += 4
            elif form == 5:
                v, = struct.unpack_from('<H', dbg, q); q += 2
            elif form == 7:
                v, = struct.unpack_from('<Q', dbg, q); q += 8
            elif form == 3:
                n, = struct.unpack_from('<H', dbg, q); v = dbg[q + 2:q + 2 + n]; q += 2 + n
            elif form == 4:
                n, = struct.unpack_from('<I', dbg, q); v = dbg[q + 4:q + 4 + n]; q += 4 + n
            elif form == 8:
                z = dbg.index(b'\0', q); v = dbg[q:z].decode('latin-1'); q = z + 1
            else:
                raise ValueError('form %d at %#x' % (form, q))
            attrs[name] = v
        out[p] = Die(p, TAGS.get(tag, tag), attrs, [])
        order.append(p)
        p = end
    # tree: a DIE's children follow it directly when its sibling pointer is further than its own end
    for i, o in enumerate(order):
        die = out[o]
        sib = die.attrs.get('sibling')
        if die.tag is None or sib is None or i + 1 >= len(order) or order[i + 1] >= sib:
            continue
        c = order[i + 1]
        while c is not None and c < sib and out[c].tag is not None:
            die.children.append(c)
            c = out[c].attrs.get('sibling')
    return out


def dies(elf):
    return parse(section(elf, '.debug'))


if __name__ == '__main__':
    import sys, collections
    t = dies(open(sys.argv[1], 'rb').read())
    print(len(t), 'entries', collections.Counter(x.tag for x in t.values()).most_common(30))
    cus = [x for x in t.values() if x.tag == 'compile_unit']
    print(len(cus), 'units; first:', cus[0].attrs)
