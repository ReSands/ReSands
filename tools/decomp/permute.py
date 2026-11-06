"""Random source permuter for DARE near misses (decomp-permuter's idea, for C++: its randomizer parses C only).
Starts from a function's best attempt, applies 1-3 random source edits per candidate (move a statement, flip
signedness, swap operands, invert an if/else, split or join an initialised declaration, add a temporary, ...),
judges each with match.py's compiler + diff on N worker processes and climbs on the number of differing
instructions. Writes game/analysis/qwen/dare_<addr>/attempts/permNN.cpp + .report.txt on every improvement (so a
later Qwen round resumes from it), and PASS + best.cpp on a match.
Usage (PC):
  python tools/decomp/permute.py <addr hex> [--minutes 20] [--jobs 6] [--from <file>]
  python tools/decomp/permute.py --queue [--max-off 10]   near misses not yet permuted from their current best
  python tools/decomp/permute.py --lane --until <unix> [--minutes 20] [--jobs 6]   the night lane: the queue, again and again
  python tools/decomp/permute.py --selftest
"""
import hashlib, multiprocessing, os, random, re, shutil, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import match  # noqa: E402
from qwen_match import match_diffs, row  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DARE = os.path.join(ROOT, 'game', 'decomp', 'dare')
QWEN = os.path.join(ROOT, 'game', 'analysis', 'qwen')
BAD = 10 ** 6

# ---- edits: each takes the source text and a Random, returns new text or None ----
STMT = re.compile(r'^(\s+)(?!return\b|case\b|default\b|break\b|continue\b|goto\b)[^\s{}#/][^{}]*;\s*$')


def lines_of(t):
    return t.split('\n')


def body_range(L):
    # line indices inside the function body: after the first line ending in '{' at column 0 (or a lone '{')
    start = next((i for i, l in enumerate(L) if l.rstrip().endswith('{') and not l.startswith((' ', '\t'))), None)
    return range(start + 1, len(L)) if start is not None else range(0)


def move_stmt(t, r):
    L = lines_of(t)
    idx = [i for i in body_range(L) if STMT.match(L[i])]
    if not idx:
        return None
    i = r.choice(idx)
    ind = STMT.match(L[i]).group(1)
    d = r.choice((-3, -2, -1, 1, 2, 3))
    j = i
    for _ in range(abs(d)):  # only across simple statements at the same indent
        k = j + (1 if d > 0 else -1)
        if k not in body_range(L) or not STMT.match(L[k]) or STMT.match(L[k]).group(1) != ind:
            break
        j = k
    if j == i:
        return None
    s = L.pop(i)
    L.insert(j, s)
    return '\n'.join(L)


TYPES = re.compile(r'\b(unsigned (?:int|short|char|long)|signed char|int|short|char|long|unsigned)\b')
FLIP = {'int': 'unsigned int', 'unsigned int': 'int', 'unsigned': 'int', 'short': 'unsigned short',
        'unsigned short': 'short', 'char': 'unsigned char', 'unsigned char': 'char', 'signed char': 'unsigned char',
        'long': 'unsigned long', 'unsigned long': 'long'}


def flip_sign(t, r):
    L = lines_of(t)
    cand = [(i, m) for i in body_range(L) for m in TYPES.finditer(L[i])]
    if not cand:
        return None
    i, m = r.choice(cand)
    L[i] = L[i][:m.start()] + FLIP[m.group(1)] + L[i][m.end():]
    return '\n'.join(L)


OPND = r'[A-Za-z_][\w]*(?:(?:->|\.)[A-Za-z_]\w*|\[[^\]\[]*\])*|\d+[uUlLfF]*'
COMMUTE = re.compile(r'(?<![\w\]\)>.])(%s) (==|!=|\+|\*|&|\||\^) (%s)(?![\w\[(.])' % (OPND, OPND))
CMP = re.compile(r'(?<![\w\]\)>.])(%s) (<=|>=|<|>) (%s)(?![\w\[(.])' % (OPND, OPND))
SWAPCMP = {'<': '>', '>': '<', '<=': '>=', '>=': '<='}


def commute(t, r):
    ms = list(COMMUTE.finditer(t)) + list(CMP.finditer(t))
    if not ms:
        return None
    m = r.choice(ms)
    op = SWAPCMP.get(m.group(2), m.group(2))
    return t[:m.start()] + '%s %s %s' % (m.group(3), op, m.group(1)) + t[m.end():]


def zero_test(t, r):
    # x != 0 <-> x inside if/while conditions, x == 0 <-> !x
    ms = [(m, 'a') for m in re.finditer(r'\((%s) != 0\)' % OPND, t)] + \
         [(m, 'b') for m in re.finditer(r'\((%s) == 0\)' % OPND, t)] + \
         [(m, 'c') for m in re.finditer(r'\b(?:if|while) \((%s)\)' % OPND, t)] + \
         [(m, 'd') for m in re.finditer(r'\(!(%s)\)' % OPND, t)]
    if not ms:
        return None
    m, k = r.choice(ms)
    v = m.group(1)
    new = {'a': '(%s)' % v, 'b': '(!%s)' % v, 'c': m.group(0).replace('(%s)' % v, '(%s != 0)' % v),
           'd': '(%s == 0)' % v}[k]
    return t[:m.start()] + new + t[m.end():]


def block_end(t, i):
    # t[i] == '{': index just after its matching '}'
    depth = 0
    for k in range(i, len(t)):
        depth += {'{': 1, '}': -1}.get(t[k], 0)
        if depth == 0:
            return k + 1
    return None


def cond_end(t, i):
    # t[i] == '(': index just after its matching ')'
    depth = 0
    for k in range(i, len(t)):
        depth += {'(': 1, ')': -1}.get(t[k], 0)
        if depth == 0:
            return k + 1
    return None


def invert_if(t, r):
    # if (c) {A} else {B} -> if (!(c)) {B} else {A}
    cands = []
    for m in re.finditer(r'\bif \(', t):
        if t[max(0, m.start() - 5):m.start()] == 'else ':
            continue
        ce = cond_end(t, m.end() - 1)
        if ce is None:
            continue
        a0 = t.find('{', ce)
        if a0 < 0 or t[ce:a0].strip():
            continue
        a1 = block_end(t, a0)
        e = re.match(r'\s*else\s*\{', t[a1:]) if a1 else None
        if not e:
            continue
        b0 = a1 + e.end() - 1
        b1 = block_end(t, b0)
        if b1:
            cands.append((m.start(), m.end() - 1, ce, a0, a1, b0, b1))
    if not cands:
        return None
    s, c0, c1, a0, a1, b0, b1 = r.choice(cands)
    cond = t[c0 + 1:c1 - 1]
    neg = cond[1:] if cond.startswith('!') and re.fullmatch(r'!(%s)' % OPND, cond) else '!(%s)' % cond
    return t[:s] + 'if (%s) ' % neg + t[b0:b1] + ' else ' + t[a0:a1] + t[b1:]


DECL_INIT = re.compile(r'^(\s+)((?:const )?[A-Za-z_][\w:<>, ]*?[\s\*&]+)([A-Za-z_]\w*) = ([^;]+);\s*$')


def split_join(t, r):
    L = lines_of(t)
    rng = body_range(L)
    split = [i for i in rng if DECL_INIT.match(L[i]) and not L[i].strip().startswith('return')]
    join = [i for i in rng if i + 1 in rng and re.match(r'^\s+[A-Za-z_][\w:<>, ]*?[\s\*&]+([A-Za-z_]\w*);\s*$', L[i])
            and re.match(r'^\s+%s = [^;]+;\s*$' % re.escape(re.match(r'.*?([A-Za-z_]\w*);\s*$', L[i]).group(1)),
                         L[i + 1])]
    if not split and not join:
        return None
    if split and (not join or r.random() < 0.5):
        i = r.choice(split)
        ind, ty, v, e = DECL_INIT.match(L[i]).groups()
        L[i:i + 1] = ['%s%s%s;' % (ind, ty, v), '%s%s = %s;' % (ind, v, e)]
    else:
        i = r.choice(join)
        L[i:i + 2] = [L[i].rstrip()[:-1] + ' =' + L[i + 1].split('=', 1)[1]]
    return '\n'.join(L)


ASSIGN = re.compile(r'^(\s+)([^=;{}]+?) = ([^;]+);\s*$')


def add_temp(t, r):
    # x = e; -> { __typeof__(e) tN = e; x = tN; }   (a temporary changes register allocation)
    L = lines_of(t)
    idx = [i for i in body_range(L) if ASSIGN.match(L[i]) and not DECL_INIT.match(L[i])
           and not L[i].strip().startswith(('return', 'for', 'if', 'while'))]
    if not idx:
        return None
    i = r.choice(idx)
    ind, lhs, e = ASSIGN.match(L[i]).groups()
    n = 'perm_t%d' % r.randrange(1000)
    L[i] = '%s{ __typeof__(%s) %s = %s; %s = %s; }' % (ind, e, n, e, lhs, n)
    return '\n'.join(L)


def inc_form(t, r):
    ms = list(re.finditer(r'\b(\w+)(\+\+|--)(?=[;)])|(\+\+|--)(\w+)\b(?=[;)])|\b(\w+) ([+-])= 1(?=[;)])', t))
    if not ms:
        return None
    m = r.choice(ms)
    v = m.group(1) or m.group(4) or m.group(5)
    op = (m.group(2) or m.group(3) or m.group(6) * 2)[0]
    forms = ['%s%s%s' % (v, op, op), '%s%s%s' % (op, op, v), '%s %s= 1' % (v, op)]
    forms.remove(next(f for f in forms if f == m.group(0)) if m.group(0) in forms else forms[0])
    return t[:m.start()] + r.choice(forms) + t[m.end():]


IFRET = re.compile(r'^(\s+)if \((.+)\) (return[^;]*;)\s*$')


def merge_ifs(t, r):
    # if (a) return x; if (b) return x;  <->  if (a || b) return x;   (0x183080 matched only merged, 5 Nov)
    L = lines_of(t)
    rng = body_range(L)
    merge = [i for i in rng if i + 1 in rng and IFRET.match(L[i]) and IFRET.match(L[i + 1])
             and IFRET.match(L[i]).group(3) == IFRET.match(L[i + 1]).group(3)]
    split = [i for i in rng if IFRET.match(L[i]) and ' || ' in IFRET.match(L[i]).group(2)
             and IFRET.match(L[i]).group(2).count('(') == IFRET.match(L[i]).group(2).count(')')]
    if not merge and not split:
        return None
    if merge and (not split or r.random() < 0.5):
        i = r.choice(merge)
        ind, a, ret = IFRET.match(L[i]).groups()
        L[i:i + 2] = ['%sif (%s || %s) %s' % (ind, a, IFRET.match(L[i + 1]).group(2), ret)]
    else:
        i = r.choice(split)
        ind, c, ret = IFRET.match(L[i]).groups()
        a, b = c.split(' || ', 1)
        L[i:i + 1] = ['%sif (%s) %s' % (ind, a, ret), '%sif (%s) %s' % (ind, b, ret)]
    return '\n'.join(L)


EDITS = [(move_stmt, 5), (flip_sign, 2), (commute, 3), (zero_test, 2), (invert_if, 2), (split_join, 2),
         (add_temp, 2), (inc_form, 1), (merge_ifs, 2)]


def mutate(t, r):
    for _ in range(r.choice((1, 1, 1, 2, 2, 3))):
        for _try in range(10):
            f = r.choices([e for e, _w in EDITS], [w for _e, w in EDITS])[0]
            n = f(t, r)
            if n and n != t:
                t = n
                break
    return t


# ---- judging in worker processes (each compiles in its own folder beside copies of the headers) ----
W = {}


def winit(addr, mangled, size):
    d = os.path.join(DARE, '_perm', '%d' % os.getpid())
    os.makedirs(d, exist_ok=True)
    for h in ('dare.h', 'dare_fix.h'):
        shutil.copy(os.path.join(match.HDR, h), d)
    W.update(dir=d, addr=addr, mangled=mangled, size=size, elf=open(match.ELF, 'rb').read())


def diffs(text):
    # differing instructions in a report (0 = MATCH, BAD = did not compile)
    d = match_diffs(text)
    return BAD if d is None else d


def wjudge(src):
    p = os.path.join(W['dir'], 'p.cpp')
    obj = p[:-4] + '.o'
    open(p, 'w').write(src)
    if os.path.exists(obj):
        os.remove(obj)
    ok, msg = match.compile_src(p, obj)
    if not ok:
        return src, BAD, 'COMPILE ERROR\n' + msg
    _m, text = match.objdiff(obj, W['mangled'], W['addr'], W['size'], W['elf'])
    return src, diffs(text), text


# ---- one function ----
def best_attempt(out):
    att = os.path.join(out, 'attempts')
    best = None
    for f in sorted(os.listdir(att)) if os.path.isdir(att) else []:
        if f.endswith('.cpp') and os.path.exists(os.path.join(att, f[:-4] + '.report.txt')):
            d = diffs(open(os.path.join(att, f[:-4] + '.report.txt')).read())
            if best is None or d < best[0]:
                best = (d, os.path.join(att, f))
    return best


def run(addr_hex, minutes, jobs, start=None, seed=None):
    mangled, addr, size, _file = row(addr_hex)
    out = os.path.join(QWEN, 'dare_%08x' % addr)
    att = os.path.join(out, 'attempts')
    os.makedirs(att, exist_ok=True)
    if os.path.exists(os.path.join(out, 'PASS')):
        print('already passed')
        return 0
    if not start:
        b = best_attempt(out)
        if not b:
            print('no attempt to start from')
            return 2
        start = b[1]
    src = open(start).read()
    tag = hashlib.sha1(src.encode()).hexdigest()[:12]
    log = open(os.path.join(out, 'perm.log'), 'a')
    say = lambda s: (print(s, flush=True), log.write(time.strftime('%Y-%m-%d %H:%M:%S ') + s + '\n'), log.flush())
    r = random.Random(seed)
    pool = multiprocessing.Pool(jobs, winit, (addr, mangled, size))
    try:
        _s, best, rep = pool.apply(wjudge, (src,))
        say('start %s (%s): %d differ, %d minutes, %d jobs' % (os.path.basename(start), tag, best, minutes, jobs))
        if best >= BAD:
            return 2
        bests, seen, n, t0 = [src], {hash(src)}, 0, time.time()
        deadline = t0 + 60 * minutes

        def cands():
            while time.time() < deadline:
                base = bests[-1] if r.random() < 0.6 else r.choice(bests)
                c = mutate(base, r)
                if hash(c) not in seen:
                    seen.add(hash(c))
                    yield c
        for c, d, text in pool.imap_unordered(wjudge, cands(), chunksize=4):
            n += 1
            if d < best:
                best, bests = d, [c]
                k = 1 + max((int(f[4:6]) for f in os.listdir(att) if re.match(r'perm\d\d\.cpp$', f)), default=0)
                name = os.path.join(att, 'perm%02d' % k)
                open(name + '.cpp', 'w').write(c)
                open(name + '.report.txt', 'w').write(text)
                say('perm%02d: %d differ after %d candidates (%.0f s)' % (k, d, n, time.time() - t0))
                if d == 0:
                    open(os.path.join(out, 'best.cpp'), 'w').write(c)
                    open(os.path.join(out, 'PASS'), 'w').write('perm%02d\n' % k)
                    say('PASS by the permuter')
                    return 0
            elif d == best and len(bests) < 64:
                bests.append(c)  # equal-score variants: the climb wanders across plateaus
        say('no match: best %d differ after %d candidates' % (best, n))
        open(os.path.join(out, 'PERMUTED'), 'w').write('%s %d\n' % (tag, minutes))
        return 1
    finally:
        pool.terminate()
        shutil.rmtree(os.path.join(DARE, '_perm'), ignore_errors=True)


def queue(max_off=10):
    # tried, not PASS / HEADER, best attempt within max_off differing instructions, and not yet permuted from it
    # (PERMUTED = start hash + minutes spent)
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(match.HDR, 'units.tsv'))]
    out = []
    for r in rows:
        if r[3] == '-':
            continue
        d = os.path.join(QWEN, 'dare_%08x' % int(r[3], 16))
        if any(os.path.exists(os.path.join(d, f)) for f in ('PASS', 'HEADER')):
            continue
        b = best_attempt(d)
        if not b or b[0] > max_off:
            continue
        tag = hashlib.sha1(open(b[1]).read().encode()).hexdigest()[:12]
        p = os.path.join(d, 'PERMUTED')
        if os.path.exists(p) and open(p).read().split()[:1] == [tag]:  # 'hash minutes' or 'hash unusable'
            continue
        out.append((b[0], int(r[4]), r[3]))
    return [a for _d, _n, a in sorted(out)]


def lane(until, minutes, jobs):
    # the night lane: near misses smallest gap first; when none are left, wait for Qwen to make new ones
    while time.time() < until:
        q = queue()  # one pass per start attempt (PERMUTED holds its hash), however long that pass was
        if not q:
            time.sleep(max(0, min(300, until - time.time())))
            continue
        a = q[0]
        m = min(minutes, max(1, int((until - time.time()) / 60)))
        print('%s %s permuting %s (%d queued)' % (time.strftime('%Y-%m-%d %H:%M'), 'lane:', a, len(q)), flush=True)
        try:
            ok = run(a, m, jobs) in (0, 1)
        except Exception as e:  # one bad candidate (a compiler timeout) must not end the night's lane
            print('lane: %s failed: %r' % (a, e), flush=True)
            ok = False
        if not ok:  # could not start or crashed: not again from this start attempt
            out = os.path.join(QWEN, 'dare_%08x' % int(a, 16))
            b = best_attempt(out)
            tag = hashlib.sha1(open(b[1]).read().encode()).hexdigest()[:12] if b else 'none'
            open(os.path.join(out, 'PERMUTED'), 'w').write('%s unusable\n' % tag)


def selftest():
    src = '''#include "dare.h"
int f(_X *p, int n)
{
    int i = 0;
    unsigned int k;
    k = p->a;
    p->b = n;
    if (p->c != 0) {
        k++;
    } else {
        k = n + p->d;
    }
    while (i < n) i++;
    return k;
}
'''
    r = random.Random(1)
    assert 'unsigned int i' in flip_sign(src, random.Random(0)) or 'int k' in flip_sign(src, random.Random(0)) \
        or flip_sign(src, random.Random(0)) != src
    inv = invert_if(src, r)
    assert 'if (!(p->c != 0)) {\n        k = n + p->d;\n    } else {\n        k++;\n    }' in inv, inv
    sj = split_join(src, random.Random(3))
    assert sj != src and ('    int i;\n    i = 0;' in sj or 'unsigned int k = p->a;' in sj), sj
    assert '(p->c)' in zero_test(src, random.Random(0)) or zero_test(src, random.Random(0)) != src
    c = commute('    x = a->b + c[2];', r)
    assert c == '    x = c[2] + a->b;', c
    assert commute('    if (i < n) y;', r) == '    if (n > i) y;'
    t = add_temp(src, random.Random(2))
    assert '__typeof__(' in t, t
    m = next(x for x in (move_stmt(src, random.Random(k)) for k in range(50)) if x)
    assert m != src and sorted(m.split('\n')) == sorted(src.split('\n')), m
    assert inc_form('    i++;', r) in ('    ++i;', '    i += 1;')
    two = 'void f(int a)\n{\n    if (a == 0) return;\n    if (a == -2) return;\n    g(a);\n}\n'
    m2 = merge_ifs(two, r)
    assert '    if (a == 0 || a == -2) return;\n    g(a);' in m2, m2
    assert merge_ifs(m2, r) == two
    for s in range(200):  # every edit keeps braces and parentheses balanced
        t = mutate(src, random.Random(s))
        assert t.count('{') == t.count('}') and t.count('(') == t.count(')'), t
    # lane(): a run that raises marks that start attempt and the lane goes on (review fix, 5 Nov)
    import tempfile
    global QWEN, run, queue
    keep = QWEN, run, queue
    QWEN = tempfile.mkdtemp()
    os.makedirs(os.path.join(QWEN, 'dare_00123450', 'attempts'))
    open(os.path.join(QWEN, 'dare_00123450', 'attempts', 'round01.cpp'), 'w').write('x')
    open(os.path.join(QWEN, 'dare_00123450', 'attempts', 'round01.report.txt'), 'w').write('1 of 2 instructions differ')
    calls = []
    queue = lambda: [] if calls else ['123450']
    run = lambda *a: calls.append(a) or 1 / 0
    try:
        lane(time.time() + 0.5, 1, 1)
    except ZeroDivisionError:
        raise AssertionError('lane() let a run() exception through')
    finally:
        mark = open(os.path.join(QWEN, 'dare_00123450', 'PERMUTED')).read()
        shutil.rmtree(QWEN)
        QWEN, run, queue = keep
    assert len(calls) == 1 and mark == hashlib.sha1(b'x').hexdigest()[:12] + ' unusable\n', (calls, mark)
    assert diffs('MATCH\n') == 0 and diffs('2 of 66 instructions differ (x)') == 2 and diffs('COMPILE ERROR') == BAD
    print('selftest ok')


def main(a):
    opt = lambda k, v: a[a.index(k) + 1] if k in a else v
    minutes, jobs = int(opt('--minutes', '20')), int(opt('--jobs', '6'))
    if a[0] == '--selftest':
        return selftest()
    if a[0] == '--queue':
        print('\n'.join(queue(int(opt('--max-off', '10')))))
        return 0
    if a[0] == '--lane':
        return lane(int(opt('--until', '0')) or time.time() + 365 * 86400, minutes, jobs)
    return run(a[0], minutes, jobs, opt('--from', None), int(opt('--seed', '0')) or None)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
