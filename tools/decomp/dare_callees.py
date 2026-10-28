# dare_callees.py <POP1.ELF> <SLUS_207.43> <units.tsv> <out callees.tsv> [<out globals.tsv>]
# Names retail call targets from POP1's symbols: for every DARE function in both ELFs, the k-th jal in retail
# calls what the k-th jal in POP1's copy calls (only when both have the same number of jals). Votes across all
# functions pick the name. Columns: retail addr (hex), mangled name, votes, votes for other names.
# globals.tsv: the same for absolute data addresses (lui rX,hi then a load/store/addiu off rX: DARE is built
# with -sdatathreshold 0, so every global is reached this way); name = POP1 data symbol [+offset], its size.
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


LOADSTORE = {0x08, 0x09, 0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2b, 0x31, 0x37, 0x39, 0x3f, 0x1e, 0x1f}


WRITES_RT = {0x09, 0x1a, 0x1b, 0x1e, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37}


def data_refs(word, addr, size):
    # absolute addresses: lui rX,hi then addiu / load / store with base rX (lo sign-extended) -> [(instr addr, target)]
    out, hi = [], {}
    for a in range(addr, addr + size, 4):
        w = word(a)
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 0x0f:  # lui
            hi[rt] = (w & 0xffff) << 16
            continue
        if op in LOADSTORE and rs in hi:
            out.append((a, (hi[rs] + ((w & 0xffff) ^ 0x8000) - 0x8000) & 0xffffffff))
        # any other write to a register forgets its lui value: I-type ALU ops and GPR loads write rt, R-type and MMI
        # ops rd, mfc1 rt (jumps, branches and stores write none)
        if op in WRITES_RT or 0x08 <= op <= 0x0e or (op == 0x11 and rs == 0):
            hi.pop(rt, None)
        elif op in (0x00, 0x1c):
            hi.pop((w >> 11) & 31, None)
    return out


def main(pop1, retail_elf, units, out, out_globals=None):
    syms, pword = symbols(open(pop1, 'rb').read())
    funcs = {n: (v, sz) for n, v, sz, t in syms if t == 2 and sz}
    names = {v: n for n, (v, sz) in funcs.items()}
    r = open(retail_elf, 'rb').read()
    rword = lambda a: struct.unpack_from('<I', r, a - 0x100000 + 0x100)[0]  # retail text: file offset 0x100 at 0x100000
    objs = sorted((v, sz, n) for n, v, sz, t in syms if t == 1 and sz)
    import bisect

    def obj_name(a):
        i = bisect.bisect_right(objs, (a, 1 << 32)) - 1
        if i >= 0 and objs[i][0] <= a < objs[i][0] + objs[i][1]:
            v, sz, n = objs[i]
            return ('%s+0x%x' % (n, a - v) if a != v else n), sz
        return None, 0
    votes, skipped = defaultdict(Counter), 0
    gvotes, gsize = defaultdict(Counter), {}
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
        a, b = [t for _, t in data_refs(rword, ra, rs)], [t for _, t in data_refs(pword, pa, ps)]
        if len(a) == len(b):
            for x, y in zip(a, b):
                n, sz = obj_name(y)
                if n:
                    gvotes[x][n] += 1
                    gsize[n.split('+')[0]] = sz
    # self-check: a pairing checked by hand on 28 Oct (OneShotSAMPLEPath::GetPos calls BranchPath::GetPos)
    assert votes[0x163100].most_common(1)[0][0] == 'GetPos__10BranchPathFv', votes[0x163100]
    with open(out, 'w') as o:
        for x in sorted(votes):
            (n, v), = votes[x].most_common(1)
            o.write('%x\t%s\t%d\t%d\n' % (x, n, v, sum(votes[x].values()) - v))
    if out_globals:
        with open(out_globals, 'w') as o:
            for x in sorted(gvotes):
                (n, v), = gvotes[x].most_common(1)
                o.write('%x\t%s\t%d\t%d\t%d\n' % (x, n, gsize[n.split('+')[0]], v, sum(gvotes[x].values()) - v))
        print(len(gvotes), 'retail data addresses named')
    split = sum(len(c) > 1 for c in votes.values())
    print(len(votes), 'retail call targets named;', split, 'with disagreeing votes;', skipped,
          'functions skipped (different jal counts)')


if __name__ == '__main__':
    main(*sys.argv[1:6])
