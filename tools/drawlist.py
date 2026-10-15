"""Checks a frame's engine-level draw list (POP_FRAMECAP E/R lines, chains walked) against the same
frame's DMA chain (POP_DMACAP): every mesh element the game draws is one DMA CALL to the element's
pass packet, so the multiset of packet addresses must equal the chain's CALL targets.

Usage: python3 -I tools/drawlist.py <framecap file> <chain.bin> [capture number, default: last]
"""
import struct
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import dmacmp  # noqa: E402


def captures(path):
    cur, out = [], []
    for line in open(path, errors='replace'):
        if line.startswith('== end of capture'):
            out.append(cur)
            cur = []
        else:
            cur.append(line.split())
    return out


def main(argv):
    caps = captures(argv[0])
    cap = caps[int(argv[2])] if len(argv) > 2 else caps[-1]
    draws = [f for f in cap if f and f[0] in ('E', 'R')]
    ours = Counter(int(f[5], 16) for f in draws if int(f[5], 16))  # E and R (Ref packets since 15 Oct)
    calls, kinds = Counter(), Counter()
    for pc, tag, da, data in dmacmp.chain_records(argv[1]):
        tid = (tag[0] >> 28) & 7
        kinds[tid] += 1
        if tid == 5:
            calls[tag[1] & 0x7FFFFFFF] += 1
    first = Counter(f[0] + ('0' if f[4] == '0' else '+') for f in draws)
    print(f'{len(caps)} captures; draw list: {len(draws)} elements ({first["E0"]} + {first["E+"]} chained CJMeshElement, '
          f'{first["R0"]} + {first["R+"]} chained CJMeshElementRef), {len(set(f[3] for f in draws))} matrices')
    print('chain tags by id:', dict(sorted(kinds.items())), f'; CALL targets: {sum(calls.values())} ({len(calls)} distinct)')
    both = sum((ours & calls).values())
    print(f'element packets that are CALL targets: {both} of {sum(ours.values())}; CALLs not from the list: {sum((calls - ours).values())}; '
          f'list packets with no CALL: {sum((ours - calls).values())}')
    extra = (calls - ours).most_common(6)
    if extra:
        print('  examples of unexplained CALL targets:', ', '.join(f'{a:#x} x{n}' for a, n in extra))


if __name__ == '__main__':
    main(sys.argv[1:])
