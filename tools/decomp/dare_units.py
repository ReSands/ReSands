# dare_units.py <POP1.ELF> <retail_names_from_proto.tsv> <out.tsv>
# Every function POP1.ELF's DWARF places in a DARE source file, joined by mangled name with the retail SoT
# functions (addr, size). Columns: file, mangled name, pop1 size, retail addr (hex or -), retail size, same.
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dwarf1

def dare_funcs(elf):
    t = dwarf1.dies(elf)
    for cu in t.values():
        if cu.tag != 'compile_unit' or '\\dare\\' not in cu.attrs['name'].lower():
            continue
        for k in cu.children:
            f = t[k]
            if f.tag in ('global_subroutine', 'subroutine') and 'low_pc' in f.attrs:
                yield (cu.attrs['name'].split('\\engine\\', 1)[1].replace('\\', '/'),
                       f.attrs.get(0x200, f.attrs.get('name')), f.attrs['high_pc'] - f.attrs['low_pc'])

def main(elf, names, out):
    retail = {}
    for line in open(names, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        retail[p[3]] = (int(p[0], 16), int(p[2]))
    rows = sorted(set(dare_funcs(open(elf, 'rb').read())))
    with open(out, 'w') as o:
        for f, n, sz in rows:
            a, rs = retail.get(n, (None, 0))
            o.write('%s\t%s\t%d\t%s\t%d\t%d\n' % (f, n, sz, '%x' % a if a else '-', rs, int(rs == sz)))
    hit = [r for r in rows if r[1] in retail]
    print(len(rows), 'DARE functions in POP1;', len(hit), 'in retail,',
          sum(retail[r[1]][1] == r[2] for r in hit), 'same size; retail bytes', sum(retail[r[1]][1] for r in hit))

if __name__ == '__main__':
    main(*sys.argv[1:4])
