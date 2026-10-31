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
from dare_header import mangled_class, demangle_params  # noqa: E402
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

## Retail assembly (branch delay slots are written with a leading _; call targets show the mangled name = the C++ name;
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
  called with no `this` but is not declared static), reply with one line "NEEDS HEADER CHANGE: <what>" and no code
  block at all (never put that line inside code).
- In code, use C++ names only: never a mangled name (Name__10Class...), a name with @ or $, or a symbol as a
  variable. @<number> constants are literals the compiler placed: write the literal itself.
- Use members by name: the header excerpt gives each member's offset (// +0x..); never cast raw offsets.
- The judge compares every instruction; call targets and global addresses only need the right symbol.
- What earlier matches taught us about this compiler:
  - Two or more tests of the same value against different constants up front, then the default path, then the
    case bodies = a switch statement. A single test is an if.
  - Stores come out in source order: write member assignments in the order the assembly stores them.
  - Plain int shifts give sll/sra; casts to short add dsll32/dsra32 pairs.
  - The constructor's vptr store is generated; write only the member assignments.
  - A destructor gets a hidden short argument (a1; delete when > 0) and the `this != 0` test from the compiler:
    write a plain destructor with only the member assignments.
  - An offset the excerpt does not list is usually inside a struct or union member: member offset + field offset
    (e.g. +0x60 = uRes at +0x10, then stMultiTrack.hFirstElementOpt at +0x50).
  - Locals on the stack are laid out in declaration order, the first at the lowest address.
  - Locking with a call to (file string, line number) between = EnterCriticalSectionTool_(); MouchardThreadTool(
    __FILE__, <line>); ... QuitCriticalSectionTool(); (the SND_fn_v...ThreadSnd functions the same way).
  - Assigning a number to an enum member or variable needs a cast to the enum type: m_eState = (_SND_tdeState)0.
  - slt / slti / sltu / sltiu: the first two are signed compares, the last two unsigned: make the compared
    variables int or unsigned to match (a mismatch here is a type, not a logic, difference).
- Write the complete file in ONE ```cpp code block.
"""


def row(key):
    # units.tsv: file, mangled, POP1 size, retail addr, retail size, same size
    for line in open(os.path.join(DARE, 'units.tsv')):
        f = line.rstrip('\n').split('\t')
        if f[3] != '-' and key in (f[1], f[3], f[3].lstrip('0')) or f[3] == key.lstrip('0'):
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
        m = re.match(r'(?:template <[^>]*> )?(struct|union) (\w+)\b[^;]*\{\s*$', line)  # sndCList is a template
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
    out, seen, size, queue = [], set(), 0, [(s, 0) for s in seeds if s in blocks]
    while queue:
        n, depth = queue.pop(0)
        if n in seen:
            continue
        seen.add(n)
        text = blocks[n]
        if size + len(text) > HEADER_BUDGET:
            continue
        out.append(text)
        size += len(text)
        # member types two levels down (pResMem->uRes.stMultiTrack.ulDataSize needs the union and the struct in it);
        # deeper only bases; breadth first, so the budget cuts the farthest types
        names = re.findall(r'\w+', text if depth < 2 else text.split('{')[0])
        queue += [(x, depth + 1) for x in names if x in blocks and x not in seen]
    return ''.join(out), cls


def cpp_name(m):
    # mangled -> the C++ name to write: Name__<n>Class... -> Class::Name, Name__F... -> Name
    cls = mangled_class(m)[0]
    i = m.find('__', 1 if not m.startswith('__') else 2)
    base = m[:i] if i > 0 else m
    if cls and '<' in cls:  # template arguments are mangled too: sndCList<18stCallBackRequest_,...>
        t, args = cls.split('<', 1)
        try:
            cls = '%s<%s>' % (t, ', '.join(a('') for a in demangle_params(args.rstrip('>').replace(',', ''))))
        except (IndexError, ValueError):
            pass
    return '%s::%s' % (cls, base) if cls else base


def elf_bytes(elf, addr, n):
    # n bytes at a virtual address of the retail ELF (its program headers), or None
    phoff, = struct.unpack_from('<I', elf, 0x1c)
    es, cnt = struct.unpack_from('<HH', elf, 0x2a)
    for i in range(cnt):
        _t, off, va, _pa, fsz = struct.unpack_from('<5I', elf, phoff + i * es)
        if va <= addr and addr + n <= va + fsz:
            return elf[off + addr - va:off + addr - va + n]
    return None


def global_note(name, addr, size, elf):
    # (note on the instruction, line for the globals list or None) for symbols that are not C++ names
    base, _, plus = name.partition('+')
    if base.startswith('@'):
        b = elf_bytes(elf, addr - (int(plus, 0) if plus else 0), size) or b''
        text = b.split(b'\0')[0]
        # sizes are POP1's: a retail string can be shorter (NUL-padded)
        if text and len(text) < len(b) and all(32 <= c < 127 for c in text):
            lit = '"%s"' % text.decode().replace('\\', '\\\\').replace('"', '\\"')
            if re.search(r'\.(cpp|c)$', text.decode()):
                return lit + ' (__FILE__)', 'the source file name %s: write __FILE__' % lit
            return lit, 'the string literal %s: write it in the code' % lit
        if size == 12:
            return 'a constant pointer to member function', ('a pointer-to-member-function constant the compiler made '
                                                             '(&Class::Method in the code)')
        return 'a constant the compiler made', 'an unnamed constant the compiler made from a literal in the source'
    if '$localstatic' in base:
        var, _, func = base.split('$', 2)
        owner = cpp_name(func)
        return 'static local %s of %s()%s' % (var, owner, '+' + plus if plus else ''), (
            'the function-local static variable `%s` inside %s(); if this function is not %s, that function '
            'was inlined here' % (var, owner, owner))
    if mangled_class(base)[0]:
        return cpp_name(base) + ('+' + plus if plus else ''), 'the static data member %s' % cpp_name(base)
    return name, None


def diff_hints(report):
    # short hints for diffs that earlier rounds kept missing
    rows = [r.split('|') for r in report.splitlines() if r.startswith('>>') and '|' in r]
    op = lambda t: (t.split() or [''])[0].lstrip('_')
    hints = set()
    for a, b in rows:
        a, b = a.split(None, 2)[-1] if len(a.split()) > 2 else a, b.strip()
        oa, ob = op(a), op(b)
        if {oa, ob} in ({'slt', 'sltu'}, {'slti', 'sltiu'}):
            hints.add('slt vs sltu: a signed/unsigned difference: change the type of a compared value (int vs '
                      'unsigned), not the logic.')
        elif {oa, ob} in ({'beq', 'bne'}, {'beql', 'bnel'}, {'bc1t', 'bc1f'}, {'blez', 'bgtz'}, {'bltz', 'bgez'}):
            hints.add('%s vs %s: the branch sense is inverted: swap the if and else bodies, or negate the '
                      'condition.' % (oa, ob))
        elif oa == ob and oa.startswith('c.') and len(a.split()) > 1 and len(b.split()) > 1 and \
                a.split()[1].split(',') == b.split()[1].split(',')[::-1]:
            hints.add('%s with its operands swapped: write the comparison the other way round (a == b vs b == a, '
                      'a < b vs b > a).' % oa)
    return ('\n\nHints: ' + ' '.join(sorted(hints))) if hints else ''


def callee_names():
    # callees.tsv (dare_callees.py): retail call target -> POP1 name, kept only when it outvotes all other names
    p = os.path.join(DARE, 'callees.tsv')
    rows = [l.rstrip('\n').split('\t') for l in open(p)] if os.path.exists(p) else []
    return {int(a, 16): n for a, n, v, other in rows if int(v) > int(other) and int(a, 16) not in WRONG_NAMES}


# one-vote callee names the prototype contradicts (game/ghidra/headless_dare_names_1vote.log "Differs", 29 Oct)
WRONG_NAMES = {0x15f240, 0x160b40, 0x1613c0, 0x162940, 0x164a40, 0x16a840, 0x1733c0, 0x17c540, 0x180600, 0x180680,
               0x180740, 0x182e40, 0x183780, 0x184180, 0x18acc0, 0x18bd40}


def name_callees(asm, ghidra):
    # retail jal comments and Ghidra's FUN_<addr> -> POP1 names; returns the texts and the names' classes
    names, used = callee_names(), set()

    def sub(m):
        a = int(m.group(1), 16)
        if a not in names:
            return m.group(0)
        used.add(names[a])
        c = cpp_name(names[a])
        return m.group(0).split(';')[0] + '; ' + names[a] + ('' if c == names[a] else ' = ' + c) \
            if ';' in m.group(0) else c
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
            note, desc = global_note(gl[t][0], t, gl[t][1], elf)
            notes[ia] = note
            used[gl[t][0].split('+')[0]] = (gl[t][1], desc)
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
        elif op in (0, 0x1c):  # R-type / MMI: writes rd
            const.pop(rd, None)
        elif op not in (0x28, 0x29, 0x2b, 0x3f, 0x1f, 0x39, 0x3d) and op >> 3 != 0 or op == 0x09:
            const.pop(rt, None)  # I-type ALU ops and loads write rt (stores and branches do not)
    asm = '\n'.join(l + '   ; = ' + notes[int(l[:8], 16)] if re.match(r'[0-9a-f]{8} ', l) and int(l[:8], 16) in notes
                    else l for l in asm.splitlines())
    names = {t: global_note(n, t, sz, elf)[0] for t, (n, sz) in gl.items()}
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
    globs = '\n'.join('- %s (%d bytes)' % (desc, sz) if desc else
                      '- %s (%d bytes; %s)' % (n, sz, 'declared in dare.h' if re.search(r'\b%s\b' % n, header)
                                              else 'not in dare.h: declare it yourself')
                      for n, (sz, desc) in sorted(used.items())) or 'none'
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


def selftest():
    c = cpp_name('RemoveHead__55sndCList<P18_tdstMemoryCluster,RCP18_tdstMemoryCluster>Fv')
    assert c.startswith('sndCList<_tdstMemoryCluster *, ') and c.endswith('>::RemoveHead') and '18' not in c, c
    assert cpp_name('RemoveHead__51sndCList<18stCallBackRequest_,18stCallBackRequest_>Fv') == \
        'sndCList<stCallBackRequest_, stCallBackRequest_>::RemoveHead'
    assert cpp_name('SND_fn_bInitSourceMemoryModule__Fv') == 'SND_fn_bInitSourceMemoryModule'
    assert cpp_name('gs_CallBackRequestList__10SndManager') == 'SndManager::gs_CallBackRequestList'
    assert 'static local clock of SND_ClockNative::GetInstance()' == \
        global_note('clock$localstatic3$GetInstance__15SND_ClockNativeFv', 0, 4, b'')[0]
    assert global_note('clock$localstatic3$GetInstance__15SND_ClockNativeFv+0x10', 0, 24, b'')[0].endswith('()+0x10')
    assert global_note('gs_CallBackRequestList__10SndManager+0x8', 0, 12, b'')[0] == \
        'SndManager::gs_CallBackRequestList+0x8'
    rep = ('x\ny\n>>  14  slt at,t0,v1                       | sltu at,t0,v1\n'
           '>>  3c  c.eq.s $f0,$f1                     | c.eq.s $f1,$f0\n'
           '>>  40  c.eq.s $f0,$f1                     | c.eq.s $f2,$f3\n'
           '>>  44  bne at,zero,0x188c48               | beq at,zero,0x188c48\n')
    h = diff_hints(rep)
    assert 'signed/unsigned' in h and 'operands swapped' in h and 'branch sense' in h and h.count('c.eq.s') == 1, h
    print('selftest ok')


def main(argv):
    opt = lambda k, v: argv[argv.index(k) + 1] if k in argv else v
    url, rounds, until = opt('--url', 'http://127.0.0.1:8033'), int(opt('--rounds', '8')), int(opt('--until', '0'))
    if argv[0] == '--queue':
        print('\n'.join(queue()))
        return 0
    if argv[0] == '--selftest':
        selftest()
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
    n, end, extra = last, last + rounds, False
    while n < end or (not extra and best >= size // 4 - 2 and n == end > last):
        if n == end:  # within 2 instructions after the last round (whatever it wrote): two more rounds, once
            extra, end = True, end + 2
            say('round%02d: within 2 instructions, 2 more rounds' % n)
        n += 1
        if until and time.time() > until:
            say('round%02d: past --until, stopping' % n)
            return 1
        messages = [{'role': 'user', 'content': first}]
        if prev:
            messages += [{'role': 'assistant', 'content': '```cpp\n' + prev[0] + '```'},
                         {'role': 'user', 'content': 'Judge report for that file (retail | yours, >> = differs):\n\n' +
                          prev[1] + diff_hints(prev[1]) + '\n\nChange the source so the compiler produces the retail instructions, and '
                          'reply with the complete file in one ```cpp block. Keep your reasoning short.'}]
        try:
            content, reasoning, usage, secs = ask(url, messages)
        except Exception as e:  # server hiccup: try the round again later
            say('round%02d: request failed (%s); waiting 60 s' % (n, str(e)[:100]))
            time.sleep(60)
            continue
        # a header request counts wherever the answer puts it (31 Oct: one inside the code block was compiled as code)
        need = re.search(r'NEEDS HEADER CHANGE:.*', content or '')
        if need:
            say('round%02d: %s' % (n, need.group(0)[:200]))
            open(os.path.join(out, 'HEADER'), 'w').write(need.group(0) + '\n')
            return 3
        # a draft inside the reasoning still gets judged when the answer has none (lesson from 23 Oct)
        code = extract(content) or extract(reasoning)
        if code is None:
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
    say('no match after %d more rounds' % (end - last))
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
