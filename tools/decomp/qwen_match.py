"""Qwen byte-matching loop for one DARE function: the model writes a .cpp that includes dare.h, match.py judges it.
Results: game/analysis/qwen/dare_<addr>/{match.log, attempts/roundNN.cpp + .report.txt, best.cpp, PASS}. Resumable.
Usage (on the PC, where mwccps2 and llama-server run):
  python tools/decomp/qwen_match.py <mangled name | retail addr hex> [--rounds 8] [--url http://127.0.0.1:8033]
  python tools/decomp/qwen_match.py <...> --prompt      print the first prompt only (no model, no compiler)
  python tools/decomp/qwen_match.py <...> --try <file>  judge a file as a round would (runner test without the model)
  python tools/decomp/qwen_match.py --queue | --summary  the night queue (addrs left, in order) / the results so far
--rounds N = N more rounds; --until <unix time>: no new round after it.
Needs game/decomp/dare/ (dare.h, dare_fix.h, units.tsv, protos.tsv, callees.tsv, globals.tsv, asm/, ghidra_c/): see docs/decomp_dare.md.
"""
import os, re, shutil, struct, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'llm_port', 'meshdecode'))
import match  # noqa: E402
from dare_header import mangled_class  # noqa: E402
from dare_callees import data_refs  # noqa: E402
from port import ask, extract  # noqa: E402  (meshdecode/port.py: model API with a fresh seed, code extraction)

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DARE = os.path.join(ROOT, 'game', 'decomp', 'dare')
QWEN = os.path.join(ROOT, 'game', 'analysis', 'qwen')
HEADER_BUDGET = 30000  # characters of dare.h per prompt (the whole header is ~265 KB)

TASK = """Write C++ source that compiles with Metrowerks CodeWarrior for PS2 3.0.3 (mwccps2 -O4,p -enum min -sdatathreshold 0) to
EXACTLY the retail machine code below (byte-matching decompilation of Ubisoft's DARE sound engine).

Function: {name}   ({size} bytes at 0x{addr:x}, original source file {file})
Declaration: {decl}
Parameter names: {params}
Original local variable names: {locals}

## Retail assembly (branch delay slots are written with a leading _; call targets are named with mangled names;
## "; = name" names the global an instruction reaches, "; = 1.5f" the float constant an mtc1 moves)
```
{asm}
```

## Ghidra's decompilation (a guide only: types and offsets are Ghidra's guesses)
```c
{ghidra}
```

## Globals this function uses (names and sizes from the developer's symbols)
{globals}

## Excerpt of dare.h (the real header, generated from the developer's debug info; your file must #include "dare.h")
```cpp
{header}
```

## Rules
- Your file starts with #include "dare.h" and defines only this function ({how}). dare.h already defines every
  class, struct, union, enum and template it uses: never write a type definition yourself. You may add extern
  declarations (functions, and globals that dare.h does not declare: extern <type of that size> <name>;). If the
  function cannot be written without a header change (a missing member, type or declaration, or a method that is
  called with no `this` but is not declared static), reply with one line "NEEDS HEADER CHANGE: <what>" and no code.
- Use members by name: the header excerpt gives each member's offset (// +0x..); never cast raw offsets.
- The judge compares every instruction; call targets and global addresses only need the right symbol.
- What earlier matches taught us about this compiler:
  - Two or more tests of the same value against different constants up front, then the default path, then the
    case bodies = a switch statement. A single test is an if.
  - Stores come out in source order: write member assignments in the order the assembly stores them.
  - Plain int shifts give sll/sra; casts to short add dsll32/dsra32 pairs.
  - The constructor's vptr store is generated; write only the member assignments.
- Write the complete file in ONE ```cpp code block.
"""


def row(key):
    # units.tsv: file, mangled, POP1 size, retail addr, retail size, same size
    for line in open(os.path.join(DARE, 'units.tsv')):
        f = line.rstrip('\n').split('\t')
        if f[3] != '-' and key in (f[1], f[3], f[3].lstrip('0')):
            return f[1], int(f[3], 16), int(f[4]), f[0]
    sys.exit('not in units.tsv with a retail address: ' + key)


def header_blocks():
    # top-level struct definitions (members commented with their offsets from the DARE_CHECK lines, which are
    # dropped), enums, C declarations and globals, by name
    lines = open(os.path.join(DARE, 'dare.h')).read().splitlines(True)
    ofs = {(m.group(1), m.group(2)): int(m.group(3)) for m in
           (re.search(r'DARE_OFS\((\w+), (\w+)\) == (\d+)', l) for l in lines) if m}
    size = {m.group(1): int(m.group(2)) for m in (re.search(r'sizeof\((\w+)\) == (\d+)', l) for l in lines) if m}
    blocks, cur, name = {}, [], None
    for line in lines:
        if name:
            m = re.search(r'\b(\w+)(\[[^\]]*\])?;\s*$', line)
            if m and '(' not in line and (name, m.group(1)) in ofs:
                line = '%s  // +0x%x\n' % (line.rstrip(), ofs[(name, m.group(1))])
            cur.append(line)
            if line.startswith('};'):
                blocks[name], name = ''.join(cur), None
            continue
        m = re.match(r'(struct|union) (\w+)\b[^;]*\{\s*$', line)
        if m:
            name = m.group(2)
            cur = [line.rstrip() + ('  // size %d\n' % size[name] if name in size else '\n')]
            continue
        m = (re.match(r'enum (\w+) \{', line) or re.match(r'[\w \*]*?\b(\w+)\(.*\);$', line)
             or re.match(r'extern [^(]*?\b(\w+)(\[[^\]]*\])?;$', line))
        if m and not line.startswith('DARE_'):
            blocks[m.group(1)] = line
    return blocks


def header_for(mangled, decl, asm, ghidra):
    blocks = header_blocks()
    cls = mangled_class(mangled)[0]
    seeds = ([cls] if cls else []) + re.findall(r'\w+', decl + ' ' + asm + ' ' + ghidra)
    out, seen, size, queue = [], set(), 0, [s for s in seeds if s in blocks]
    while queue:
        n = queue.pop(0)
        if n in seen:
            continue
        seen.add(n)
        text = blocks[n]
        if size + len(text) > HEADER_BUDGET:
            continue
        out.append(text)
        size += len(text)
        # bases and member types of the class itself one level deeper; other structs only their bases
        names = re.findall(r'\w+', text if n == cls or len(seen) < 4 else text.split('{')[0])
        queue += [x for x in names if x in blocks and x not in seen]
    return ''.join(out), cls


def callee_names():
    # callees.tsv (dare_callees.py): retail call target -> POP1 name, kept only when it outvotes all other names
    p = os.path.join(DARE, 'callees.tsv')
    rows = [l.rstrip('\n').split('\t') for l in open(p)] if os.path.exists(p) else []
    return {int(a, 16): n for a, n, v, other in rows if int(v) > int(other)}


def name_callees(asm, ghidra):
    # retail jal comments and Ghidra's FUN_<addr> -> POP1 names; returns the texts and the names' classes
    names, used = callee_names(), set()

    def sub(m):
        a = int(m.group(1), 16)
        if a not in names:
            return m.group(0)
        used.add(names[a])
        return m.group(0).split(';')[0] + '; ' + names[a] if ';' in m.group(0) else names[a]
    asm = re.sub(r'jal 0x([0-9a-f]+)\s+; \S+', sub, asm)
    ghidra = re.sub(r'\bFUN_([0-9a-f]{8})\b', sub, ghidra)
    return asm, ghidra, ' '.join(used | {mangled_class(n)[0] or '' for n in used})


def annotate(asm, ghidra, addr, size):
    # globals.tsv names on the instructions that reach them (and Ghidra's DAT_ names); float constants on mtc1
    p = os.path.join(DARE, 'globals.tsv')
    rows = [l.rstrip('\n').split('\t') for l in open(p)] if os.path.exists(p) else []
    gl = {int(a, 16): (n, int(sz)) for a, n, sz, v, other in rows if int(v) > int(other)}
    elf = open(match.ELF, 'rb').read()
    word = lambda a: struct.unpack_from('<I', elf, a - 0x100000 + 0x100)[0]
    notes, used = {}, {}
    for ia, t in data_refs(word, addr, size):
        if t in gl:
            notes[ia] = gl[t][0]
            used[gl[t][0].split('+')[0]] = gl[t][1]
    const = {}
    for a in range(addr, addr + size, 4):
        w = word(a)
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        if op == 0x0f:
            const[rt] = (w & 0xffff) << 16
        elif op == 0x0d and rs in const:  # ori
            const[rt] = const[rs] | (w & 0xffff)
        elif op == 0x11 and rs == 4:  # mtc1 rt -> f(rd)
            v = 0 if rt == 0 else const.get(rt)
            if v is not None:
                notes[a] = '%gf' % struct.unpack('<f', struct.pack('<I', v))[0]
    asm = '\n'.join(l + '   ; = ' + notes[int(l[:8], 16)] if re.match(r'[0-9a-f]{8} ', l) and int(l[:8], 16) in notes
                    else l for l in asm.splitlines())
    names = {t: n for t, (n, sz) in gl.items()}
    # Ghidra writes DAT_00680c80, _DAT_..., PTR_..., or a type prefix + Ram00680c80 (iRam, puRam, ...)
    ghidra = re.sub(r'\b(?:_?DAT_|PTR_|[a-z]*Ram)([0-9a-f]{8})\b', lambda m: names.get(int(m.group(1), 16), m.group(0)),
                    ghidra)
    return asm, ghidra, used


def prompt(mangled, addr, size, file):
    proto = next((l.rstrip('\n').split('\t') for l in open(os.path.join(DARE, 'protos.tsv'))
                  if l.split('\t')[0] == mangled), [mangled, file, '?', '', ''])
    asm = open(os.path.join(DARE, 'asm', '%08x.s' % addr)).read().strip()
    ghidra = open(os.path.join(DARE, 'ghidra_c', '%08x.c' % addr)).read().strip()
    asm, ghidra, callees = name_callees(asm, ghidra)
    asm, ghidra, used = annotate(asm, ghidra, addr, size)
    header, cls = header_for(mangled, proto[2], asm + ' ' + callees + ' ' + ' '.join(used), ghidra)
    globs = '\n'.join('- %s (%d bytes; %s)' % (n, sz, 'declared in dare.h' if re.search(r'\b%s\b' % n, header)
                       else 'not in dare.h: declare it yourself') for n, sz in sorted(used.items())) or 'none'
    how = 'as %s::<name>' % cls if cls else 'a C function: dare.h declares it extern "C"'
    return TASK.format(name=mangled, size=size, addr=addr, file=file, decl=proto[2], params=proto[3] or 'none',
                       locals=(proto[4] if len(proto) > 4 else '') or 'none', asm=asm, ghidra=ghidra,
                       header=header, how=how, globals=globs)


def judge(path, mangled, addr, size, elf):
    # -> (matched, matching instruction count, report); compiled beside dare.h, which it includes
    src = os.path.join(DARE, '_q_%08x.cpp' % addr)
    obj = src[:-4] + '.o'
    shutil.copy(path, src)
    if os.path.exists(obj):
        os.remove(obj)
    ok, msg = match.compile_src(src, obj)
    if not ok:
        return False, -1, 'COMPILE ERROR\n' + msg
    matched, text = match.objdiff(obj, mangled, addr, size, elf)
    return matched, score(text), text


def score(report):
    # matching instructions in a judge report (-1 = did not compile)
    return -1 if report.startswith('COMPILE') else sum(not r.startswith('>>') for r in report.splitlines()[2:])


def queue():
    # functions not yet PASS / HEADER: untried first, then the same-size ones (most likely to match POP1's layouts), smallest first
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(DARE, 'units.tsv'))]
    at = lambda r, f: os.path.exists(os.path.join(QWEN, 'dare_%08x' % int(r[3], 16), f))
    rows = sorted((r for r in rows if r[3] != '-'), key=lambda r: (at(r, 'attempts'), r[5] != '1', int(r[4]), r[3]))
    done = lambda r: at(r, 'PASS') or at(r, 'HEADER')
    return [r[3] for r in rows if not done(r)]


def summary():
    # one line per state, then the near misses (best attempt, matching instructions of the retail size)
    units = {int(f[3], 16): f for f in (l.rstrip('\n').split('\t') for l in open(os.path.join(DARE, 'units.tsv')))
             if f[3] != '-'}
    state, near = {'PASS': 0, 'HEADER': 0, 'tried': 0}, []
    for d in sorted(os.listdir(QWEN)) if os.path.isdir(QWEN) else []:
        if not d.startswith('dare_') or int(d[5:], 16) not in units:
            continue
        u, path = units[int(d[5:], 16)], os.path.join(QWEN, d)
        flag = next((f for f in ('PASS', 'HEADER') if os.path.exists(os.path.join(path, f))), 'tried')
        state[flag] += 1
        if flag == 'tried':
            att = os.path.join(path, 'attempts')
            best = max((score(open(os.path.join(att, f)).read()) for f in os.listdir(att) if f.endswith('.report.txt')),
                       default=-1)
            near.append((best / (int(u[4]) // 4), best, int(u[4]) // 4, u[1]))
    print('DARE: %d of %d passed, %d need a header change, %d tried without a match, %d not tried' % (
        state['PASS'], len(units), state['HEADER'], state['tried'], len(units) - sum(state.values())))
    for frac, best, n, name in sorted(near, reverse=True)[:15]:
        print('  near miss: %d of %d instructions  %s' % (best, n, name))


def main(argv):
    opt = lambda k, v: argv[argv.index(k) + 1] if k in argv else v
    url, rounds, until = opt('--url', 'http://127.0.0.1:8033'), int(opt('--rounds', '8')), int(opt('--until', '0'))
    if argv[0] == '--queue':
        print('\n'.join(queue()))
        return 0
    if argv[0] == '--summary':
        summary()
        return 0
    mangled, addr, size, file = row(argv[0])
    first = prompt(mangled, addr, size, file)
    if '--prompt' in argv:
        print(first)
        print('-- %d characters' % len(first), file=sys.stderr)
        return 0
    elf = open(match.ELF, 'rb').read()
    if '--try' in argv:
        ok, _n, text = judge(opt('--try', ''), mangled, addr, size, elf)
        print(text)
        return 0 if ok else 1
    out = os.path.join(QWEN, 'dare_%08x' % addr)
    att = os.path.join(out, 'attempts')
    os.makedirs(att, exist_ok=True)
    if os.path.exists(os.path.join(out, 'PASS')):
        print(mangled, 'already passed')
        return 0
    log = open(os.path.join(out, 'match.log'), 'a')
    say = lambda s: (print(s, flush=True), log.write(time.strftime('%H:%M:%S ') + s + '\n'), log.flush())
    done = sorted(f for f in os.listdir(att) if f.endswith('.cpp'))
    prev = (open(os.path.join(att, done[-1])).read(), open(os.path.join(att, done[-1][:-4] + '.report.txt')).read()) \
        if done else None
    if prev and prev[1].startswith('MATCH'):  # an earlier run died between judging and writing PASS (28 Oct, over ssh)
        open(os.path.join(out, 'PASS'), 'w').write(done[-1][:-4] + '\n')
        say('PASS in %s (found on resume)' % done[-1][:-4])
        return 0
    best = max((score(open(os.path.join(att, f[:-4] + '.report.txt')).read()) for f in done), default=-2)
    # --rounds = this many more rounds; rounds that wrote no code leave a .nocode.txt so a resume counts them
    last = max((int(f[5:7]) for f in os.listdir(att) if f.startswith('round')), default=0)
    for n in range(last + 1, last + rounds + 1):
        if until and time.time() > until:
            say('round%02d: past --until, stopping' % n)
            return 1
        messages = [{'role': 'user', 'content': first}]
        if prev:
            messages += [{'role': 'assistant', 'content': '```cpp\n' + prev[0] + '```'},
                         {'role': 'user', 'content': 'Judge report for that file (retail | yours, >> = differs):\n\n' +
                          prev[1] + '\n\nChange the source so the compiler produces the retail instructions, and '
                          'reply with the complete file in one ```cpp block. Keep your reasoning short.'}]
        try:
            content, reasoning, usage, secs = ask(url, messages)
        except Exception as e:  # server hiccup: try the round again later
            say('round%02d: request failed (%s); waiting 60 s' % (n, str(e)[:100]))
            time.sleep(60)
            continue
        # a draft inside the reasoning still gets judged when the answer has none (lesson from 23 Oct)
        code = extract(content) or extract(reasoning)
        if code is None:
            need = re.search(r'NEEDS HEADER CHANGE:.*', content)
            if need:
                say('round%02d: %s' % (n, need.group(0)[:200]))
                open(os.path.join(out, 'HEADER'), 'w').write(need.group(0) + '\n')
                return 3
            open(os.path.join(att, 'round%02d.nocode.txt' % n), 'w').write((content or reasoning)[-4000:])
            say('round%02d: no code block (%.0f s, %s tokens)' % (n, secs, usage.get('completion_tokens')))
            continue
        path = os.path.join(att, 'round%02d.cpp' % n)
        open(path, 'w').write(code)
        ok, got, text = judge(path, mangled, addr, size, elf)
        open(path[:-4] + '.report.txt', 'w').write(text)
        say('round%02d: %s; %.0f s, %s tokens' % (n, text.splitlines()[0][:100], secs, usage.get('completion_tokens')))
        if got > best:
            best = got
            open(os.path.join(out, 'best.cpp'), 'w').write(code)
        prev = (code, text)
        if ok:
            open(os.path.join(out, 'PASS'), 'w').write('round%02d\n' % n)
            say('PASS in round%02d' % n)
            return 0
    say('no match after %d more rounds' % rounds)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
