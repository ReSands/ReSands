"""Stage borders for a VU1 program port (tools/vu1port/port.py) from vu1replay --cutscan.

  vu1replay <trace> <image dir> /dev/null --only <hash> --cutscan <scan.txt> --hints <scan.hints>   (per trace)
  python3 tools/vu1port/cutscan.py <image.bin> <scan.txt>... [--max N] [--resume <pc>...]

Prints a STAGES dict for port.py and a report. A pc may start a stage only if, on every visit in the traces:
it is not a delay slot, nothing but vf / vi / ACC writes, clip flags, Q (the snapshot lands both), MAC / status
flags or an XGKICK is in flight, and it does not lie between a MAC / clip flag write and the FM* / FC* reader that
reads those flags (the scan's flag-split column). Every stage entry must be such a pc: the run starts (0 and the
MSCNT resume points after each E pair), subroutine entries (BAL targets), return points (after a BAL's delay
slot) and every branch target reached from another stage. Straight runs longer than --max pairs (default 16) are
cut at the safe pc nearest the limit. Written for P4 (the character program, 25 Oct 2026).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
from vudis import disasm  # noqa: E402

# Pending bits (replay.cpp): vf 1, vi 2, acc 4, store 8, xgkick 16, Q 32, P 64, MAC 128, clip 256, status 512, branch 1024
SAFE = 1 | 2 | 4 | 16 | 32 | 128 | 256 | 512
END = 0xffffffff


def load(image, scans):
    img = open(image, 'rb').read()
    pend, delay, split, visits = {}, set(), set(), {}
    for path in scans:
        for line in open(path):
            f = line.split()
            pc = int(f[0], 16)
            pend[pc] = pend.get(pc, 0) | int(f[1], 16)
            if f[2] == '1':
                delay.add(pc)
            if f[3] == '1':
                split.add(pc)
            visits[pc] = visits.get(pc, 0) + int(f[4])
    code = {}
    for line in disasm(img[:max(pend) + 16], 0):
        pc = int(line[:4], 16)
        code[pc] = (line[6:41].strip(), line[41:76].strip(), line[76:].strip())
    return code, pend, delay, split, visits


def control(lower):
    """(kind, target, link register) of a lower branch instruction, else None."""
    m = re.match(r'(B|BAL|IBEQ|IBNE|IBLTZ|IBGTZ|IBLEZ|IBGEZ)\b.*?0x([0-9a-f]+)$', lower)
    if m:
        link = re.match(r'BAL (vi\d+)', lower)
        return m.group(1), int(m.group(2), 16), link.group(1) if link else None
    m = re.match(r'(JR|JALR)\b.*?(vi\d+)$', lower)
    if m:
        return m.group(1), None, m.group(2)
    return None


def main(argv):
    image, rest = argv[0], argv[1:]
    maxlen = 16
    extra = set()
    scans = []
    i = 0
    while i < len(rest):
        if rest[i] == '--max':
            maxlen = int(rest[i + 1])
            i += 2
        elif rest[i] == '--resume':
            i += 1
            while i < len(rest) and not rest[i].startswith('--') and not rest[i].endswith('.txt'):
                extra.add(int(rest[i], 16))
                i += 1
        else:
            scans.append(rest[i])
            i += 1
    code, pend, delay, split, visits = load(image, scans)
    cov = set(pend)

    def safe(pc):
        return pc in cov and pc not in delay and pc not in split and (pend[pc] & ~SAFE) == 0

    # basic blocks: leaders are the run starts (0, the pair after each E pair's delay slot), branch targets and the
    # pair after each branch's delay slot; so a block's only branch is its second-to-last pair
    returns = {}  # link register -> the return points of its BALs
    leaders = {0} | extra
    starts = {0} | extra
    sub_entries = set()
    for pc in sorted(cov):
        up, lo, fl = code[pc]
        c = control(lo)
        if 'E' in fl:
            starts.add(pc + 16)
            leaders.add(pc + 16)
        if c:
            kind, target, link = c
            leaders.add(pc + 16)
            if target is not None:
                leaders.add(target)
            if kind == 'BAL':
                returns.setdefault(link, set()).add(pc + 16)
                sub_entries.add(target)
    required = (starts | sub_entries | {r for rs in returns.values() for r in rs}) & cov
    leaders = {l for l in leaders if l in cov}
    for pc in sorted(cov):  # the first pair of each covered run of code
        if pc - 8 not in cov:
            leaders.add(pc)

    def block_end(l):
        pc = l + 8
        while pc in cov and pc not in leaders:
            pc += 8
        return pc

    # long blocks: extra leaders at safe pcs about maxlen pairs apart
    for l in sorted(leaders):
        e = block_end(l)
        s = l
        while (e - s) // 8 > maxlen:
            cand = [p for p in range(s + 8 * 4, e - 8 * 3, 8) if safe(p)]
            if not cand:
                break
            best = min(cand, key=lambda p: (abs(p - (s + maxlen * 8)), p))
            leaders.add(best)
            s = best
    blocks = {l: block_end(l) for l in leaders}

    def succ(l):
        e = blocks[l]
        out = set()
        bpc = e - 16
        up, lo, fl = code.get(bpc, ('', '', ''))
        c = control(lo) if bpc >= l else None
        if 'E' in fl and bpc >= l:
            return {END}
        if c:
            kind, target, link = c
            if kind in ('JR', 'JALR'):
                return set(returns.get(link, set()))
            out.add(target)
            if kind.startswith('IB'):
                out.add(e)
            return {t for t in out if t in cov}  # never taken in the traces: no exit
        if 'E' in code.get(e - 8, ('', '', ''))[2]:  # E in the last pair (its delay slot is the next block)
            return {e}
        return {e} if e in cov else set()

    preds = {}
    for l in blocks:
        for t in succ(l):
            preds.setdefault(t, set()).add(l)
    # a block that only falls through from its single predecessor (which has no other successor) joins it
    # while the stage stays within maxlen; unsafe non-required blocks join every predecessor's stage
    entry_of = {}
    for l in sorted(blocks):
        p = preds.get(l, set())
        q = next(iter(p)) if len(p) == 1 else None
        joinable = q is not None and blocks[q] == l and l in succ(q)  # l is only reached by falling out of q
        entry_of[l] = None if (l not in required and (not safe(l) or joinable)) else l
    # stages: from each entry block, the closure over successors that are not entries
    def grow(entry):
        members, todo = [], [entry]
        size = 0
        while todo:
            b = todo.pop()
            if b in members:
                continue
            members.append(b)
            size += (blocks[b] - b) // 8
            for t in succ(b):
                if t != END and t in blocks and entry_of.get(t) is None and t not in members:
                    todo.append(t)
        return members, size
    # undo fall-through joins that make a stage longer than maxlen (the joined block becomes an entry if safe)
    for _ in range(100):
        changed = False
        for l in sorted(b for b in blocks if entry_of[b] is not None):
            members, size = grow(l)
            if size > maxlen:
                for b in sorted(members):
                    if b != l and safe(b) and b not in required and entry_of[b] is None:
                        entry_of[b] = b
                        changed = True
                        break
        if not changed:
            break
    stages = []
    unsafe_req = sorted(r for r in required if not safe(r))
    for l in sorted(b for b in blocks if entry_of[b] is not None):
        members, size = grow(l)
        ex = set()
        for b in members:
            for t in succ(b):
                if t == END or t not in members:
                    ex.add(t)
        ranges = []
        for b in sorted(members):
            if ranges and ranges[-1][1] + 8 == b:
                ranges[-1][1] = blocks[b] - 8
            else:
                ranges.append([b, blocks[b] - 8])
        stages.append((l, sorted(ex), ranges, size))
    orphans = sorted(b for b in blocks if entry_of[b] is None and not preds.get(b))
    print('# %d stages, %d pairs in stages; longest %d pairs; required entries %d' % (
        len(stages), sum(st[3] for st in stages), max(st[3] for st in stages), len(required)))
    for r in unsafe_req:
        print('# UNSAFE required entry 0x%x' % r)
    for b in orphans:
        print('# block 0x%x is reached by no other block and is no entry' % b)
    for l, ex, ranges, size in stages:
        bad = [x for x in ex if x != END and x in blocks and entry_of[x] is None]
        exs = ', '.join('P4_END' if x == END else '0x%x' % x for x in ex)
        rs = ', '.join('(0x%x, 0x%x)' % (a, b) for a, b in ranges)
        print("    'p4_%04x': dict(entry=0x%x, exits=[%s], ranges=[%s]),  # %d pairs%s" % (
            l, l, exs, rs, size, ('  EXIT TO NON-ENTRY ' + str([hex(x) for x in bad])) if bad else ''))


if __name__ == '__main__':
    main(sys.argv[1:])
