# dare_names.py <callees.tsv> <PS2Recomp source dir> <out prefix>: DARE call-target names (dare_callees.py) as
# PoPApplyProtoNames.java input (address, namespace, name, mangled), with the same pop_ prefix as
# tools/proto_names.py for names PS2Recomp would stub. <prefix>_2votes.tsv / <prefix>_1vote.tsv: checked against
# the prototype names (29 Oct), 14 of 177 one-vote names were wrong, none of 61 with two or more votes.
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from proto_names import demangle, stub_names, collides


def main(callees, recomp_dir, out):
    stubs = stub_names(recomp_dir)
    rows = [l.rstrip('\n').split('\t') for l in open(callees)]
    with open(out + '_2votes.tsv', 'w') as o2, open(out + '_1vote.tsv', 'w') as o1:
        for a, mangled, votes, other in rows:
            if int(votes) <= int(other):
                continue
            o = o1 if int(votes) == 1 else o2
            ns, short = demangle(mangled)
            if collides(short, stubs):
                short = 'pop_' + short
            o.write('0x%08X\t%s\t%s\t%s\n' % (int(a, 16), '::'.join(ns), short, mangled))


if __name__ == '__main__':
    main(*sys.argv[1:4])
