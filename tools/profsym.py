"""Symbolizes a POP_PROFILE file (tools/overrides/pop_profiler.cpp) and prints, per busy thread,
the functions with the most samples: self (the sampled pc) and inclusive (anywhere on the stack).

Usage: python3 -I tools/profsym.py PROFILE SYMBOLS [--top N] [--threads N]
  SYMBOLS: the executable (macOS: read with nm -n, names demangled with c++filt) or an MSVC
  linker map (*.map, link /MAP; names undecorated loosely). Addresses outside the image are
  shown as [module name] when the profile lists the loaded modules (Windows), else [outside image].
"""
import bisect
import re
import subprocess
import sys
from collections import Counter


def load_nm(exe):
    out = subprocess.run(['nm', '-n', exe], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in 'tT':
            syms.append((int(p[0], 16), p[2]))
    names = [n for _, n in syms]
    dem = subprocess.run(['c++filt', '-_'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    if len(dem) == len(names):
        syms = [(a, d) for (a, _), d in zip(syms, dem)]
    return 0x100000000, syms


def undecorate(name):
    # ?name@scope@@... -> scope::name ; plain C names stay.
    m = re.match(r'\?([^@?]+)@((?:[^@]+@)*)@', name)
    if not m:
        return name
    scopes = [s for s in m.group(2).split('@') if s]
    return '::'.join(reversed(scopes)) + ('::' if scopes else '') + m.group(1)


def load_map(path):
    preferred, syms = 0x140000000, []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
        if m:
            preferred = int(m.group(1), 16)
        m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\b', line)
        if m:
            syms.append((int(m.group(2), 16), undecorate(m.group(1))))
    syms.sort()
    return preferred, syms


def main(argv):
    top = int(argv[argv.index('--top') + 1]) if '--top' in argv else 25
    nthreads = int(argv[argv.index('--threads') + 1]) if '--threads' in argv else 6
    prof, symfile = argv[0], argv[1]
    preferred, syms = load_map(symfile) if symfile.lower().endswith('.map') else load_nm(symfile)
    addrs = [a for a, _ in syms]
    base, threads, stacks, modules = None, {}, [], []
    for line in open(prof, encoding='utf-8', errors='replace'):
        p = line.split()
        if not p or p[0] == '#':
            if p:
                print(line.rstrip())
            continue
        if p[0] == 'image':
            base = int(p[1], 16)
        elif p[0] == 'module':
            modules.append((int(p[1], 16), int(p[2], 16), ' '.join(p[3:])))
        elif p[0] == 'thread':
            threads[int(p[1])] = (int(p[2]), ' '.join(p[3:]))
        elif p[0] == 's':
            stacks.append((int(p[1]), int(p[2]), [int(x, 16) for x in p[3:]]))
    slide = base - preferred
    hi = addrs[-1] + 0x100000 if addrs else 0
    cache = {}

    def name(pc):
        if pc in cache:
            return cache[pc]
        a = pc - slide
        i = bisect.bisect_right(addrs, a) - 1
        if 0 <= i and addrs[0] <= a < hi:
            n = syms[i][1]
        else:
            n = next((f'[{m}]' for b, size, m in modules if b <= pc < b + size and b != base), '[outside image]')
        cache[pc] = n
        return n

    total = sum(s for s, _ in threads.values())
    # Every thread is sampled each round, so rank threads by samples whose pc is in the image
    # (waits sit in system libraries).
    per = {}
    for st, count, pcs in stacks:
        selfc, incl = per.setdefault(st, (Counter(), Counter()))
        selfc[name(pcs[0])] += count
        for n in set(name(pc) for pc in pcs):
            incl[n] += count
    outside = lambda n: n.startswith('[')
    busy_of = {t: threads[t][0] - sum(c for n, c in per.get(t, (Counter(), None))[0].items() if outside(n)) for t in threads}
    for t in sorted(threads, key=lambda t: -busy_of[t])[:nthreads]:
        samples, tname = threads[t]
        selfc, incl = per.get(t, (Counter(), Counter()))
        busy = busy_of[t]
        print(f'\n== thread {tname}: {samples} samples, {busy} in the image ({100 * busy / max(samples, 1):.0f}%)')
        print('   self:')
        for n, c in selfc.most_common(top):
            print(f'   {100 * c / samples:5.1f}%  {n[:150]}')
        print('   inclusive:')
        for n, c in incl.most_common(top):
            print(f'   {100 * c / samples:5.1f}%  {n[:150]}')
    print(f'\n{total} samples in all threads')


if __name__ == '__main__':
    main(sys.argv[1:])
