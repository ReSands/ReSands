"""Deterministic Ghidra-C to Python converter for porting decompiled loaders.

Usage: python3 -I tools/c2py.py <decompile.c> <out.py> [--buffer puStack_f0]

The output defines one function per decompiled function, taking a runtime `rt` (see
tools/c2py_rt.py) that provides memory (rt.r32/rt.w32 ...), the input buffer and stand-ins for
called functions (rt.call). Translation is purely syntactic and line by line, so it can be rerun
at no cost; whatever it cannot translate is kept as a `# TODO c2py:` comment with the C line,
and counted in the summary, so nothing is silently dropped.

Rules: braces become indentation; if/else/while/do-while/for become Python control flow (for:
init, while, step before each loop end; `continue` inside a for is flagged); casts are removed;
`*(T *)(e)` loads/stores become rt.r*/rt.w* by type width; the buffer pointer variable becomes
a byte offset `P` (uint* arithmetic scaled by 4) read with rt.buf32(P); `x._1_1_` style field
accesses become shifts and masks; goto/labels/switch are flagged.
"""
import re
import sys

TYPES = r'(?:undefined7|undefined6|undefined5|undefined3|undefined8|undefined4|undefined2|undefined1|undefined|uint|int|ulong|long|byte|char|short|ushort|bool|float|code|double|longlong)'
WIDTH = {'undefined7': 64, 'undefined6': 64, 'undefined5': 64, 'undefined3': 32, 'undefined8': 64, 'ulong': 64, 'long': 64, 'longlong': 64, 'double': 64, 'undefined4': 32, 'uint': 32,
         'int': 32, 'float': 32, 'code': 32, 'undefined': 32, 'undefined2': 16, 'short': 16, 'ushort': 16,
         'undefined1': 8, 'byte': 8, 'char': 8, 'bool': 8}

todo = []
PTR = {}     # pointer variable -> element size in bytes
ARRAYS = {}  # stack array -> byte size
PTR2 = {}  # double pointer -> element size of the pointer it points to
SCALARS = {}  # scalar local -> declared type


def strip_noise(src, buf='puStack_f0'):
    """Record declarations (types), then drop them and the decompiler's byte-swap blocks."""
    lines, out, i = src.split('\n'), [], 0
    while i < len(lines):
        l = lines[i]
        d = re.match(r'\s+(' + TYPES + r')\s*(\*+)?\s*(\w+)\s*(?:\[(\w+)\])?;\s*$', l)
        if d:
            base, stars, name, n = d.groups()
            if n:
                ARRAYS[name] = int(n, 0) * max(WIDTH.get(base, 32) // 8, 1)
            if not n:
                SCALARS[name] = base + (' ' + stars if stars else '')
            if stars and not n:
                PTR[name] = 4 if len(stars) > 1 else max(WIDTH.get(base, 32) // 8, 1)
                if len(stars) > 1:
                    PTR2[name] = max(WIDTH.get(base, 32) // 8, 1) if len(stars) == 2 else 4
            i += 1
            continue
        if re.match(r'\s*if \(_LOA_gb_SwapperActive != 0\) \{\s*$', l):
            depth = 1
            i += 1
            while i < len(lines) and depth:
                depth += lines[i].count('{') - lines[i].count('}')
                i += 1
            continue
        if re.match(r'\s+\w+ = \(undefined1\)\w+;\s*$', l):
            i += 1
            continue
        if l.strip():
            out.append(l)
        i += 1
    text = '\n'.join(out)
    for name, base in SCALARS.items():
        if name == buf:  # the buffer pointer is handled as P (and &P via rt.call_p)
            continue
        if re.search(r'&\s*' + name + r'\b', text):
            cell = name + '__a'
            text = re.sub(r'&\s*' + name + r'\b', cell, text)
            ctype = 'undefined4' if base.endswith('*') else base  # a pointer cell holds 32 bits
            text = re.sub(r'(?<![\w.])' + name + r'\b(?!__a)', f'(*({ctype} *){cell})', text)
            PTR.pop(name, None)
            ARRAYS[cell] = 8
    return text


def ptr_arith(e):
    """Scale + on typed pointers; NAME[k] and *NAME loads use the element width."""
    for name, size2 in PTR2.items():  # *pp + n steps by the pointee's element size
        e = re.sub(r'\*\s*' + name + r'\s*([+-])\s*(\w+)(?!\s*\*)', lambda m: f'*{name}__D {m.group(1)} {size2} * ({m.group(2)})', e)
    for name, size in PTR.items():
        w = size * 8
        if size != 1:  # scale + first, then indexing (which scales itself)
            e = re.sub(r'(?<![\w.*])(?<!\*\s)' + name + r'\s*([+-])\s*(\w+)(?!\s*\*)', lambda m: f'{name} {m.group(1)} {size} * ({m.group(2)})', e)
        e = re.sub(r'\b' + name + r'\[([^\]]+)\]', lambda m: f'rt.r{w}({name} + {size} * ({m.group(1)}))', e)
        e = re.sub(r'(?:(?<=^)|(?<=[(,=!<>&|+\-~ ]))\*\s*' + name + r'\b', f'rt.r{w}({name})', e)
    for name in ARRAYS:
        e = re.sub(r'&\s*' + name + r'\b', name, e)
    return e.replace('__D', '')


def split_balanced(s, start):
    """Index just past the parenthesis group that starts at s[start] == '('."""
    depth = 0
    for i in range(start, len(s)):
        if s[i] == '(':
            depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0:
                return i + 1
    return len(s)


CASTFN = {'undefined7': 'u64', 'undefined6': 'u64', 'undefined5': 'u64', 'undefined3': 'u32', 'long': 's64', 'longlong': 's64', 'ulong': 'u64', 'undefined8': 'u64', 'int': 's32', 'uint': 'u32',
          'undefined4': 'u32', 'undefined': 'u32', 'short': 's16', 'ushort': 'u16', 'undefined2': 'u16',
          'char': 's8', 'byte': 'u8', 'undefined1': 'u8', 'bool': 'u8', 'float': 'f32', 'code': 'u32', 'double': 'f64'}


def operand_end(e, j):
    while j < len(e) and e[j] == ' ':
        j += 1
    if j < len(e) and e[j] == '*':  # a dereference as the operand: *x, **x
        return operand_end(e, j + 1)
    if j < len(e) and e[j] == '(':
        return split_balanced(e, j)
    m = re.match(r'-?[\w.]+(\s*\()?', e[j:])
    if not m:
        return j
    k = j + m.end()
    if m.group(1):  # a call: include its argument list
        k = split_balanced(e, k - 1)
    while k < len(e) and e[k] == '[':
        k = e.index(']', k) + 1
    return k


def value_casts(e):
    pat = re.compile(r'\(\s*(' + TYPES + r')\s*\)')
    while True:
        ms = list(pat.finditer(e))
        if not ms:
            return e
        m = ms[-1]  # innermost/rightmost first, so (ulong)(byte)x nests correctly
        k = operand_end(e, m.end())
        e = e[:m.start()] + f'rt.{CASTFN[m.group(1)]}(' + e[m.end():k].strip() + ')' + e[k:]


def unary_derefs(e):
    out, i = '', 0
    while i < len(e):
        if e[i] == '*' and (not out.strip() or out.rstrip()[-1] in '(,=!<>&|+-*/~ ' and not out.rstrip().endswith((')', ']')) and not re.search(r'[\w)\]]\s*$', out)):
            k = operand_end(e, i + 1)
            out += f'rt.r32({unary_derefs(e[i + 1:k].strip())})'
            i = k
            continue
        out += e[i]
        i += 1
    return out


def expr(e, buf):
    e = e.strip()
    # buffer pointer: byte arithmetic first, then uint* arithmetic
    e = re.sub(r'\(\s*int\s*\)\s*' + buf + r'\b', 'P', e)
    e = re.sub(r'\(\s*uint\s*\*\s*\)\s*(?=\(\s*P\b|P\b)', '', e)
    # function-pointer calls: (**(code **)(addr))(args) -> rt.callptr(rt.r32(addr), args)
    e = re.sub(r'\(\s*\*\*?\s*\(\s*code\s*\*\*?\s*\)\s*(\((?:[^()]|\([^()]*\))*\))\s*\)\s*\(', r'rt.callptr(rt.r32\1, ', e)
    e = re.sub(r'\*' + buf + r'\b', 'rt.buf32(P)', e)
    e = re.sub(buf + r'\[(\w+)\]', r'rt.buf32(P + 4 * (\1))', e)
    e = re.sub(buf + r'\s*\+\s*(\w+)', r'P + 4 * (\1)', e)
    e = re.sub(r'\b' + buf + r'\b', 'P', e)
    e = ptr_arith(e)
    # typed memory loads *(T *)(addr) and *(T *)name
    out, i = '', 0
    while i < len(e):
        m = re.match(r'\*\s*\(\s*(' + TYPES + r')\s*\*+\s*\)\s*', e[i:])
        if m:
            j = i + m.end()
            k = operand_end(e, j)
            inner = e[j:k].strip()
            if inner.startswith('(') and split_balanced(inner, 0) == len(inner):
                inner = inner[1:-1]
            out += f'rt.r{WIDTH.get(m.group(1), 32)}({expr(inner, buf)})'
            i = k
            continue
        out += e[i]
        i += 1
    e = out
    # sub-field accesses: x._N_M_ = M bytes at offset N
    e = re.sub(r'(\w+)\._(\d+)_(\d+)_', lambda m: f'(({m.group(1)} >> {8 * int(m.group(2))}) & {hex((1 << (8 * int(m.group(3)))) - 1)})', e)
    # pointer casts vanish; value casts become width/sign conversions around their operand
    e = re.sub(r'\(\s*' + TYPES + r'\s*\*+\s*\)', '', e)
    e = value_casts(e)
    # untyped unary dereference: *name or *(expr) -> rt.r32(...)
    e = unary_derefs(e)
    # literals and operators
    e = re.sub(r'\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', e)
    e = e.replace("'\\0'", '0').replace("'\\x01'", '1')
    e = re.sub(r"'\\x([0-9a-fA-F]{2})'", lambda m: str(int(m.group(1), 16)), e)
    e = re.sub(r'\btrue\b', 'True', e)
    e = re.sub(r'\bfalse\b', 'False', e)
    e = e.replace('&&', ' and ').replace('||', ' or ')
    e = re.sub(r'!(?!=)', ' not ', e)
    e = re.sub(r'(\w+)\s*\+\+', r'\1 + 1', e)
    # function calls (not rt.*) go through the runtime table
    e = re.sub(r'(?<![\w.])([A-Za-z_]\w*)\s*\(', lambda m: m.group(0) if m.group(1) in ('rt', 'not', 'and', 'or', 'P') else f'rt.call(\'{m.group(1)}\', ', e)
    e = e.replace(", )", ")")
    pos = 0
    while True:
        m = re.compile(r'\((\w+) = ').search(e, pos)
        if not m:
            break
        k = split_balanced(e, m.start())
        parts = top_commas(e[m.start() + 1:k - 1])
        if len(parts) < 2:
            pos = m.end()
            continue
        first = re.match(r'\s*(\w+) = (.*)$', parts[0], re.S)
        e = e[:m.start()] + f'(({first.group(1)} := {first.group(2).strip()}), ' + ','.join(parts[1:]) + ')[-1]' + e[k:]
        pos = m.start() + 1
    return re.sub(r'\s+', ' ', e).strip()


def statement(s, buf):
    """One C statement (no trailing ';') -> Python line(s)."""
    s = s.strip()
    lhs, rhs = split_assign(s)
    while lhs is not None and lhs.startswith('(') and split_balanced(lhs, 0) == len(lhs):
        lhs = lhs[1:-1].strip()
        s = lhs + ' = ' + rhs
    if lhs is not None and re.match(r'\*\s*\(\s*(' + TYPES + r')\s*\*+\s*\)\s*\w+__a$', lhs):
        m = re.match(r'\*\s*\(\s*(' + TYPES + r')\s*\*+\s*\)\s*(\w+__a)$', lhs)
        return [f'rt.w{WIDTH.get(m.group(1), 32)}({m.group(2)}, {expr(rhs, buf)})']
    if lhs is not None and lhs.startswith('*') and not re.match(r'\*\s*\(\s*(' + TYPES + r')\s*\*+\s*\)', lhs):
        target = lhs[1:].strip()  # *<pointer expression> = v
        width = PTR.get(target, 4) * 8 if re.match(r'\w+$', target) else 32
        if target.startswith('*'):  # **(T **)(a): store through the pointer loaded from a
            return [f'rt.w32({expr(target, buf)}, {expr(rhs, buf)})']
        return [f'rt.w{width}({expr(target, buf)}, {expr(rhs, buf)})']
    m = re.match(r'\*\s*\(\s*(' + TYPES + r')\s*\*+\s*\)\s*', s)
    if m:  # typed store *(T *)(addr) = value
        j = m.end()
        k = operand_end(s, j)
        addr = s[j:k].strip()
        if addr.startswith('(') and split_balanced(addr, 0) == len(addr):
            addr = addr[1:-1]
        rest = s[k:].strip()
        if rest.startswith('='):
            return [f'rt.w{WIDTH.get(m.group(1), 32)}({expr(addr, buf)}, {expr(rest[1:], buf)})']
    m = re.match(r'\*\s*(\w+)\s*=\s*(.*)$', s)
    if m and m.group(1) not in PTR:  # untyped pointer: 32-bit store
        return [f'rt.w32({"P" if m.group(1) == buf else m.group(1)}, {expr(m.group(2), buf)})']
    m = m or re.match(r'(\w+)\[([^\]]+)\]\s*=\s*(.*)$', s)
    if m and m.group(1) in PTR:
        name, size = m.group(1), PTR[m.group(1)]
        if s.startswith('*'):
            return [f'rt.w{size * 8}({name}, {expr(m.group(2), buf)})']
        return [f'rt.w{size * 8}({name} + {size} * ({expr(m.group(2), buf)}), {expr(m.group(3), buf)})']
    m = re.match(r'(\w+)\._(\d+)_(\d+)_\s*=\s*(.*)$', s)
    if m:  # sub-field store
        v, off, n, val = m.group(1), 8 * int(m.group(2)), (1 << (8 * int(m.group(3)))) - 1, m.group(4)
        return [f'{v} = ({v} & ~{hex(n << off)}) | (({expr(val, buf)} & {hex(n)}) << {off})']
    m = re.match(r"(?:(\w+)\s*=\s*)?(\w+)\s*\((.*)\)$", s)
    if m and re.search(r"&\s*" + buf + r"\b", m.group(3)):
        # a call that advances the buffer through &buf (any argument): the stand-in gets P where
        # the pointer was (rt.PREF) and returns (new P, result)
        args = expr(re.sub(r"&\s*" + buf + r"\b", "PREF_MARK", m.group(3)), buf).replace("PREF_MARK", "rt.PREF")
        res = f"P, {m.group(1)} = " if m.group(1) else "P, _ = "
        return [res + f"rt.call_p('{m.group(2)}', P, {args})"]
    m = re.match(r'(\w+)\s*([+\-|&^]?=)\s*(.*)$', s)
    if m and not s.startswith('return'):
        target = 'P' if m.group(1) == buf else m.group(1)
        return [f'{target} {m.group(2)} {expr(m.group(3), buf)}']
    if s.startswith('return'):
        return [('return ' + expr(s[6:], buf)).strip()]
    if s in ('break', 'continue'):
        return [s]
    if re.match(r'goto \w+$', s):
        todo.append(s)
        return [f'# TODO c2py: {s}']
    return [expr(s, buf)]


def top_commas(cond):
    parts, depth, cur = [], 0, ''
    for ch in cond:
        depth += (ch == '(') - (ch == ')')
        if ch == ',' and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    return parts + [cur]


def split_assign(s):
    """Split at the top-level plain '=' (not ==, <=, >=, !=, +=, ...); (None, None) if absent."""
    depth = 0
    for i, ch in enumerate(s):
        depth += (ch == '(') - (ch == ')')
        if ch == '=' and depth == 0 and s[i + 1:i + 2] != '=' and s[i - 1:i] not in ('=', '<', '>', '!', '+', '-', '|', '&', '^', '*', '/'):
            return s[:i].strip(), s[i + 1:].strip()
    return None, None


def logical_lines(lines):
    """Join C lines until parentheses balance (multi-line conditions)."""
    acc, depth, body = '', 0, False
    for l in lines:
        if not body:  # the signature lines pass through one by one
            body = l.strip() == '{'
            yield l
            continue
        acc = (acc + ' ' + l.strip()) if acc else l
        depth += l.count('(') - l.count(')')
        t = acc.strip()
        complete = t.endswith((';', '{', '}')) or re.match(r'\w+:$', t) or t in ('else', '{') or t.startswith(('/*', '=='))
        if depth <= 0 and complete:
            yield acc
            acc, depth = '', 0
    if acc:
        yield acc


def convert(src, buf):
    lines = src.split('\n')
    py = ['# Generated by tools/c2py.py from a Ghidra decompile. Do not edit; fix the converter.', '']
    stack = []  # open blocks: ('if'|'else'|'loop'|'for', step) ; depth = len(stack)
    pending_func = None
    in_body = False
    for raw in logical_lines(lines):
        line = raw.strip()
        if not line or line.startswith('/*') or line.startswith('=='):
            continue
        ind = '    ' * (len(stack) + (1 if in_body else 0))
        if not in_body:
            if line.startswith('(') and not pending_func:
                continue
            m = re.match(r'(\w+)\s*$', line)
            if m and not pending_func and line not in TYPES:
                pending_func = m.group(1)
                continue
            m = re.match(r'(?:[\w*]+\s+)*(\w+)\s*\((.*)\)\s*$', line) or (pending_func and re.match(r'()\((.*)\)\s*$', line))
            if m:
                pending_func = m.group(1) if m.group(1) and m.group(1) not in TYPES else pending_func
                params = [p.strip().split()[-1].lstrip('*') for p in m.group(2).split(',') if p.strip() and p.strip() != 'void']
                for p_ in m.group(2).split(','):
                    d = re.match(r'\s*(' + TYPES + r')\s*(\*+)\s*(\w+)', p_)
                    if d:
                        PTR[d.group(3)] = 4 if len(d.group(2)) > 1 else max(WIDTH.get(d.group(1), 32) // 8, 1)
                        if len(d.group(2)) == 2:
                            PTR2[d.group(3)] = max(WIDTH.get(d.group(1), 32) // 8, 1)
                fname = re.sub(r'\W', '_', pending_func)
                py.append(f'def {fname}(rt, ' + ', '.join(params) + '):')
                continue
            if line == '{':
                in_body = True
                py.append('    P = 0')
                for name, size in ARRAYS.items():
                    py.append(f'    {name} = rt.alloca({size})')
                continue
            continue
        # closing braces, possibly followed by else / while
        while line.startswith('}'):
            line = line[1:].strip()
            if not stack:
                in_body = False
                pending_func = None  # function closed; ignore trailing text
                py.append('')
                break
            kind, step = stack.pop()
            ind = '    ' * (len(stack) + 1)
            if kind == 'for' and step:
                py.append(ind + '    ' + step)
            if kind == 'do':
                m = re.match(r'while\s*(\(.*\))\s*;?$', line)
                if m:
                    py.append(ind + f'    if not ({expr(m.group(1), buf)}): break')
                    line = ''
                else:
                    py.append(ind + '    break  # TODO c2py: do-while without condition')
                    todo.append('do-while')
            if not line:
                break
        if not line:
            continue
        ind = '    ' * (len(stack) + 1)
        m = re.match(r'(else\s+)?if\s*(\(.*\))\s*\{$', line)
        if m:
            py.append(ind + ('elif ' if m.group(1) else 'if ') + expr(m.group(2), buf) + ':')
            stack.append(('if', None))
            continue
        if line == 'else {':
            py.append(ind + 'else:')
            stack.append(('else', None))
            continue
        m = re.match(r'while\s*(\(.*\))\s*\{$', line)
        if m:
            parts = top_commas(m.group(1).strip()[1:-1])
            if len(parts) > 1:  # comma expression: side effects in order, then the test
                py.append(ind + 'while True:')
                for side in parts[:-1]:
                    py.extend(ind + '    ' + x for x in statement(side, buf))
                py.append(ind + f'    if not ({expr(parts[-1], buf)}): break')
            else:
                py.append(ind + 'while ' + expr(m.group(1), buf) + ':')
            stack.append(('loop', None))
            continue
        if line == 'do {':
            py.append(ind + 'while True:')
            stack.append(('do', None))
            continue
        m = re.match(r'for\s*\((.*?);(.*?);(.*)\)\s*\{$', line)
        if m:
            init, cond, step = m.group(1), m.group(2), m.group(3)
            for part in [x for x in init.split(',') if x.strip()]:
                py.extend(ind + x for x in statement(part, buf))
            parts, depth, cur = [], 0, ''
            for ch in cond:  # a comma expression as the condition: side effects, then the test
                depth += (ch == '(') - (ch == ')')
                if ch == ',' and depth == 0:
                    parts.append(cur)
                    cur = ''
                else:
                    cur += ch
            parts.append(cur)
            if len(parts) > 1:
                py.append(ind + 'while True:')
                for side in parts[:-1]:
                    py.extend(ind + '    ' + x for x in statement(side, buf))
                py.append(ind + f'    if not ({expr(parts[-1], buf)}): break')
            else:
                py.append(ind + 'while ' + (expr(cond, buf) if cond.strip() else 'True') + ':')
            stack.append(('for', '; '.join(statement(step, buf)) if step.strip() else None))
            continue
        m = re.match(r'(\w+):$', line)
        if m:
            py.append(ind + f'# TODO c2py: label {m.group(1)}')
            todo.append(line)
            continue
        if line.startswith('switch'):
            py.append(ind + f'# TODO c2py: {line}')
            todo.append(line)
            continue
        if line.endswith(';'):
            for st in [line[:-1]]:
                if st.strip() == 'continue' and any(k == 'for' for k, _ in stack):
                    todo.append('continue in for')
                    py.append(ind + '# TODO c2py: continue inside for (step skipped)')
                py.extend(ind + x for x in statement(st, buf))
            continue
        py.append(ind + f'# TODO c2py: {line}')
        todo.append(line)
    return add_free_names('\n'.join(py) + '\n')


def add_free_names(code):
    """Initialise every name a function uses: globals from rt.glob, C locals to 0."""
    import ast
    import builtins
    tree = ast.parse(code)
    lines = code.split('\n')
    for fn in reversed([n for n in tree.body if isinstance(n, ast.FunctionDef)]):
        params = {a.arg for a in fn.args.args}
        stored, loaded = set(), set()
        for node in ast.walk(fn):
            if isinstance(node, ast.Name):
                (stored if isinstance(node.ctx, ast.Store) else loaded).add(node.id)
        names = sorted((stored | loaded) - params - {'P', 'True', 'False', 'None'} - set(dir(builtins)))
        init = [f'    {n} = 0' if n in stored else f"    {n} = rt.glob('{n}')" for n in names]
        lines[fn.body[0].lineno - 1:fn.body[0].lineno - 1] = init
    return '\n'.join(lines)


def main(argv):
    buf = argv[argv.index('--buffer') + 1] if '--buffer' in argv else 'puStack_f0'
    out = convert(strip_noise(open(argv[0]).read(), buf), buf)
    open(argv[1], 'w').write(out)
    print(f'{argv[1]}: {out.count(chr(10))} lines, {len(todo)} TODO items')
    for t in todo[:40]:
        print('  TODO', t[:120])


if __name__ == '__main__':
    main(sys.argv[1:])
