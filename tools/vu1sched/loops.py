# Loops of a build-time scheduled VU1 program (runtime patch 37) and how many edges inside each cross parts.
# Usage: python3 -I tools/vu1sched/loops.py <generated dir> <program hash> [nodyn]   (nodyn: leave out JR edges)
# SCCs of a scheduled program's node graph from its generated part files; part-crossing edges inside each SCC
import re, sys, glob, collections
sys.setrecursionlimit(100000)
d, h = sys.argv[1], sys.argv[2]
NODYN = len(sys.argv) > 3
succ = collections.defaultdict(set); part = {}; pc = {}
for f in glob.glob(f'{d}/vu1_{h}_sched_*.cpp'):
    k = int(re.search(r'_sched_(\d+)\.cpp', f).group(1)); cur = None
    for line in open(f):
        m = re.match(r'^n(\d+): // (\S+)', line)
        if m: cur = int(m.group(1)); part[cur] = k; pc[cur] = m.group(2); continue
        if re.match(r'^[a-z]\w*:', line) and not line.startswith('n'): cur = None   # cold blocks
        if cur is None: continue
        if NODYN and re.match(r'^        case 0x', line): continue   # JR (link label) edges
        for t in re.findall(r'goto n(\d+);', line) + re.findall(r'nx = (\d+)u', line): succ[cur].add(int(t))
idx = {}; low = {}; st = []; on = set(); comps = []; c = [0]
def tarjan(v):
    idx[v] = low[v] = c[0]; c[0] += 1; st.append(v); on.add(v)
    for w in succ[v]:
        if w not in idx: tarjan(w); low[v] = min(low[v], low[w])
        elif w in on: low[v] = min(low[v], idx[w])
    if low[v] == idx[v]:
        comp = []
        while True:
            w = st.pop(); on.discard(w); comp.append(w)
            if w == v: break
        comps.append(comp)
for v in list(part):
    if v not in idx: tarjan(v)
big = sorted((c for c in comps if len(c) > 1), key=len, reverse=True)
print(f'{h}: {len(part)} nodes, {len(big)} loops (SCCs > 1 node)')
for comp in big[:8]:
    s = set(comp); cross = sum(1 for v in comp for w in succ[v] if w in s and part[w] != part[v])
    pcs = sorted({pc[v] for v in comp})
    print(f'  size {len(comp):5d}  parts {sorted({part[v] for v in comp})}  crossing edges {cross:4d}  pcs {pcs[0]}..{pcs[-1]} ({len(pcs)} distinct)')
