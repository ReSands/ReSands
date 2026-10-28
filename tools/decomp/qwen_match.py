"""Qwen byte-matching loop for one DARE function: the model writes a .cpp that includes dare.h, match.py judges it.
Results: game/analysis/qwen/dare_<addr>/{match.log, attempts/roundNN.cpp + .report.txt, best.cpp, PASS}. Resumable.
Usage (on the PC, where mwccps2 and llama-server run):
  python tools/decomp/qwen_match.py <mangled name | retail addr hex> [--rounds 8] [--url http://127.0.0.1:8033]
  python tools/decomp/qwen_match.py <...> --prompt      print the first prompt only (no model, no compiler)
  python tools/decomp/qwen_match.py <...> --try <file>  judge a file as a round would (runner test without the model)
Needs game/decomp/dare/ (dare.h, dare_fix.h, units.tsv, protos.tsv, asm/, ghidra_c/): see docs/decomp_dare.md.
"""
import os, re, shutil, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'llm_port', 'meshdecode'))
import match  # noqa: E402
from port import ask, extract  # noqa: E402  (meshdecode/port.py: model API with a fresh seed, code extraction)

ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
DARE = os.path.join(ROOT, 'game', 'decomp', 'dare')
HEADER_BUDGET = 30000  # characters of dare.h per prompt (the whole header is ~265 KB)

TASK = """Write C++ source that compiles with Metrowerks CodeWarrior for PS2 3.0.3 (mwccps2 -O4,p -enum min) to
EXACTLY the retail machine code below (byte-matching decompilation of Ubisoft's DARE sound engine).

Function: {name}   ({size} bytes at 0x{addr:x}, original source file {file})
Declaration: {decl}
Parameter names: {params}
Original local variable names: {locals}

## Retail assembly (branch delay slots are written with a leading _)
```
{asm}
```

## Ghidra's decompilation (a guide only: types and offsets are Ghidra's guesses)
```c
{ghidra}
```

## Excerpt of dare.h (the real header, generated from the developer's debug info; your file must #include "dare.h")
```cpp
{header}
```

## Rules
- Your file starts with #include "dare.h" and defines only this function ({how}). You may add extern
  declarations, but never redefine or change a type from dare.h. If the function cannot be written without a
  header change (a missing member or type), reply with one line "NEEDS HEADER CHANGE: <what>" and no code.
- The judge compares every instruction; call targets and global addresses only need the right symbol.
- What earlier matches taught us about this compiler:
  - Two beq tests up front, then the default path, then the case bodies = a switch statement, not if/else.
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
    # top-level struct definitions (with their DARE_CHECK lines dropped), enums and C declarations, by name
    blocks, cur, name = {}, [], None
    for line in open(os.path.join(DARE, 'dare.h')):
        if name:
            cur.append(line)
            if line.startswith('};'):
                blocks[name], name = ''.join(cur), None
            continue
        m = re.match(r'(struct|union) (\w+)\b[^;]*\{\s*$', line)
        if m:
            name, cur = m.group(2), [line]
            continue
        m = re.match(r'enum (\w+) \{', line) or re.match(r'[\w \*]*?\b(\w+)\(.*\);$', line)
        if m and not line.startswith('DARE_'):
            blocks[m.group(1)] = line
    return blocks


def header_for(mangled, decl, asm, ghidra):
    blocks = header_blocks()
    m = re.search(r'__(?:Q2\d+\w+?)?(\d+)', mangled)  # method: __<len><Class>F...
    cls = mangled[m.end():m.end() + int(m.group(1))] if m and not mangled.startswith('__') else None
    if mangled.startswith('__'):  # __ct__5CToolFv: operator / constructor / destructor
        m2 = re.match(r'__\w\w__(\d+)', mangled)
        cls = mangled[m2.end():m2.end() + int(m2.group(1))] if m2 else None
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


def prompt(mangled, addr, size, file):
    proto = next((l.rstrip('\n').split('\t') for l in open(os.path.join(DARE, 'protos.tsv'))
                  if l.split('\t')[0] == mangled), [mangled, file, '?', '', ''])
    asm = open(os.path.join(DARE, 'asm', '%08x.s' % addr)).read().strip()
    ghidra = open(os.path.join(DARE, 'ghidra_c', '%08x.c' % addr)).read().strip()
    header, cls = header_for(mangled, proto[2], asm, ghidra)
    how = 'as %s::<name>' % cls if cls else 'a C function: dare.h declares it extern "C"'
    return TASK.format(name=mangled, size=size, addr=addr, file=file, decl=proto[2], params=proto[3] or 'none',
                       locals=(proto[4] if len(proto) > 4 else '') or 'none', asm=asm, ghidra=ghidra,
                       header=header, how=how)


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
    rows = text.splitlines()[2:]
    return matched, sum(not r.startswith('>>') for r in rows), text


def main(argv):
    opt = lambda k, v: argv[argv.index(k) + 1] if k in argv else v
    url, rounds = opt('--url', 'http://127.0.0.1:8033'), int(opt('--rounds', '8'))
    mangled, addr, size, file = row(argv[0])
    first = prompt(mangled, addr, size, file)
    if '--prompt' in argv:
        print(first)
        print('-- %d characters' % len(first), file=sys.stderr)
        return 0
    elf = open(match.ELF, 'rb').read()
    if '--try' in argv:
        ok, score, text = judge(opt('--try', ''), mangled, addr, size, elf)
        print(text)
        return 0 if ok else 1
    out = os.path.join(ROOT, 'game', 'analysis', 'qwen', 'dare_%08x' % addr)
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
    best = -2
    last = max((int(f[5:7]) for f in done), default=0)
    for n in range(last + 1, max(rounds, last) + 1):
        messages = [{'role': 'user', 'content': first}]
        if prev:
            messages += [{'role': 'assistant', 'content': '```cpp\n' + prev[0] + '```'},
                         {'role': 'user', 'content': 'Judge report for that file (retail | yours, >> = differs):\n\n' +
                          prev[1] + '\n\nChange the source so the compiler produces the retail instructions, and '
                          'reply with the complete file in one ```cpp block. Keep your reasoning short.'}]
        try:
            content, _r, usage, secs = ask(url, messages)
        except Exception as e:  # server hiccup: try the round again later
            say('round%02d: request failed (%s); waiting 60 s' % (n, str(e)[:100]))
            time.sleep(60)
            continue
        code = extract(content)
        if code is None:
            need = re.search(r'NEEDS HEADER CHANGE:.*', content)
            if need:
                say('round%02d: %s' % (n, need.group(0)[:200]))
                open(os.path.join(out, 'HEADER'), 'w').write(need.group(0) + '\n')
                return 3
            say('round%02d: no code block (%.0f s, %s tokens)' % (n, secs, usage.get('completion_tokens')))
            continue
        path = os.path.join(att, 'round%02d.cpp' % n)
        open(path, 'w').write(code)
        ok, score, text = judge(path, mangled, addr, size, elf)
        open(path[:-4] + '.report.txt', 'w').write(text)
        say('round%02d: %s; %.0f s, %s tokens' % (n, text.splitlines()[0][:100], secs, usage.get('completion_tokens')))
        if score > best:
            best = score
            open(os.path.join(out, 'best.cpp'), 'w').write(code)
        prev = (code, text)
        if ok:
            open(os.path.join(out, 'PASS'), 'w').write('round%02d\n' % n)
            say('PASS in round%02d' % n)
            return 0
    say('stopped after %d rounds' % rounds)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
