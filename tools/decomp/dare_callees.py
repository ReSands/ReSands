# dare_callees.py <POP1.ELF> <SLUS_207.43> <units.tsv> <out callees.tsv>
# Names retail call targets from POP1's symbols: for every DARE function in both ELFs, the k-th jal in retail
# calls what the k-th jal in POP1's copy calls (only when both have the same number of jals). Votes across all
# functions pick the name. Columns: retail addr (hex), mangled name, votes, votes for other names.
import sys, os, struct
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dare_header import symbols


def jals(word, addr, size):
    out = []
    for a in range(addr, addr + size, 4):
        w = word(a)
        if w >> 26 == 3:  # jal
            out.append(((a + 4) & 0xf0000000) | (w & 0x3ffffff) << 2)
    return out


def main(pop1, retail_elf, units, out):
    syms, pword = symbols(open(pop1, 'rb').read())
    funcs = {n: (v, sz) for n, v, sz, t in syms if t == 2 and sz}
    names = {v: n for n, (v, sz) in funcs.items()}
    r = open(retail_elf, 'rb').read()
    rword = lambda a: struct.unpack_from('<I', r, a - 0x100000 + 0x100)[0]  # retail text: file offset 0x100 at 0x100000
    votes, skipped = defaultdict(Counter), 0
    for line in open(units):
        f = line.rstrip('\n').split('\t')
        if f[3] == '-' or f[1] not in funcs:
            continue
        ra, rs = int(f[3], 16), int(f[4])
        pa, ps = funcs[f[1]]
        a, b = jals(rword, ra, rs), jals(pword, pa, ps)
        if len(a) != len(b):
            skipped += 1
            continue
        for x, y in zip(a, b):
            if y in names:
                votes[x][names[y]] += 1
    # self-check: a pairing checked by hand on 28 Oct (OneShotSAMPLEPath::GetPos calls BranchPath::GetPos)
    assert votes[0x163100].most_common(1)[0][0] == 'GetPos__10BranchPathFv', votes[0x163100]
    with open(out, 'w') as o:
        for x in sorted(votes):
            (n, v), = votes[x].most_common(1)
            o.write('%x\t%s\t%d\t%d\n' % (x, n, v, sum(votes[x].values()) - v))
    split = sum(len(c) > 1 for c in votes.values())
    print(len(votes), 'retail call targets named;', split, 'with disagreeing votes;', skipped,
          'functions skipped (different jal counts)')


if __name__ == '__main__':
    main(*sys.argv[1:5])
