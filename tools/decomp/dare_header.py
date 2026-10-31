# dare_header.py <POP1.ELF> <out dare.h> [<out protos.tsv>]
# C++ header for the DARE sound engine from POP1.ELF's DWARF 1 (CodeWarrior): every struct/class/union/enum/
# typedef the DARE source files saw, with sizes and member offsets checked at compile time, class methods (virtual
# ones in vtable slot order, from the ELF's __vt__ tables), C functions in extern "C", globals as extern.
# protos.tsv: mangled name -> declaration and the function's parameter/local names, for the per-function prompts.
# Struct layouts are POP1's DARE version; SoT's can differ (a header fix is then found by matching).
import sys, os, re, struct, zlib
from collections import defaultdict, Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dwarf1

FT = {1: 'char', 2: 'signed char', 3: 'unsigned char', 4: 'short', 5: 'short', 6: 'unsigned short', 7: 'int',
      8: 'int', 9: 'unsigned int', 0xa: 'long', 0xb: 'long', 0xc: 'unsigned long', 0xd: 'void *', 0xe: 'float',
      0xf: 'double', 0x10: 'long double', 0x14: 'void', 0x15: 'bool', 0x8008: 'long long',
      0x8108: 'long long', 0x8208: 'unsigned long long'}
RECORD = ('class_type', 'structure_type', 'union_type')
# members whose type the DWARF lost ({} = the member name)
MEMBER_TYPES = {('TAdder', 'm_pGetDataSampleProc'):
                'int (TAdder::*{})(_SND_tdstToolBufferSample *, unsigned int *, unsigned int)'}
# sndCList is a template in the source; DWARF keeps only "sndCList" (args from the mangled method names)
SNDCLIST = '''template <class TYPE, class ARG_TYPE> struct sndCList {
    struct sndCNode {
        sndCNode*pNext;
        sndCNode*pPrev;
        TYPE data;
    };
    sndCNode*m_pNodeHead;
    sndCNode*m_pNodeTail;
    unsigned int m_nCount;
    sndCList(void);
    ~sndCList();
    __sndPos*AddHead(ARG_TYPE newElement);
    __sndPos*AddTail(ARG_TYPE newElement);
    __sndPos*Find(ARG_TYPE searchValue, __sndPos *startAfter) const;
    __sndPos*FindIndex(unsigned int nIndex) const;
    void FreeNode(sndCNode *pNode);
    void RemoveAll(void);
    void RemoveAt(__sndPos *position);
    TYPE RemoveHead(void);
    TYPE RemoveTail(void);
};'''
SNDCLIST_ARGS = {('CTheme', 'm_PartList'): 'CThemePart *, CThemePart *',
                 ('CThemeChannel', 'm_OldThemeList'): 'CTheme *, CTheme *',
                 ('CThemeChannel', 'm_PartSentList'): '_stPartInfo, _stPartInfo',
                 ('CThemeManager', 'm_ThemeChannelList'): 'CThemeChannel *, CThemeChannel *',
                 (None, 'gs_CallBackRequestList'): 'stCallBackRequest_, stCallBackRequest_'}
TEMPLATED = {'sndCList', 'sndCNode'}
# methods retail calls without 'this' (DWARF does not mark static)
STATIC_METHODS = {('SndManager', 'GetLengthSample'), ('SndManager', 'AddCallBack'), ('RPS2', 'GetResLength'),
                  ('SndManager', 'GetRendererRate')}  # 0x189180 never reads a0; its callers set none (31 Oct)
# C functions whose parameters POP1's DWARF lost (retail passes __FILE__, __LINE__)
C_PARAMS = {'MouchardThreadTool': 'char *file, int line', 'SND_fn_vMouchardThreadsnd': 'char *file, int line'}


def symbols(d):
    shoff, = struct.unpack_from('<I', d, 0x20)
    es, n, si = struct.unpack_from('<HHH', d, 0x2e)
    sh = [struct.unpack_from('<10I', d, shoff + i * es) for i in range(n)]
    sym = next(s for s in sh if s[1] == 2)
    strs = sh[sym[6]][4]
    main = next(s for s in sh if s[3] == 0x100000)
    out = []
    for i in range(sym[5] // 16):
        o, v, sz, info, _, _ = struct.unpack_from('<IIIBBH', d, sym[4] + i * 16)
        out.append((d[strs + o:d.index(b'\0', strs + o)].decode('latin-1'), v, sz, info & 15))
    return out, (lambda a: struct.unpack_from('<I', d, main[4] + a - main[3])[0])


def mangled_class(m):
    # Name__<n>Class[C]F... -> Class (None for free functions / nested names)
    i = m.find('__', 1 if not m.startswith('__') else 2)
    mm = re.match(r'(\d+)', m[i + 2:]) if i > 0 else None
    if not mm:
        return None, None
    n = int(mm.group(1))
    cls = m[i + 2 + len(mm.group(1)):i + 2 + len(mm.group(1)) + n]
    rest = m[i + 2 + len(mm.group(1)) + n:]
    return cls, rest.startswith('C')


BASIC = {'v': 'void', 'c': 'char', 's': 'short', 'i': 'int', 'l': 'long', 'x': 'long long', 'f': 'float',
         'd': 'double', 'r': 'long double', 'b': 'bool', 'w': 'wchar_t'}


def demangle_params(sig):
    # CodeWarrior (cfront-style) parameter list, e.g. 'ii16_SND_tdeFadeTypeii' -> declarator builders
    pos = [0]
    out = []

    def one():
        q, sign = [], ''
        while sig[pos[0]] in 'CVUS':
            c = sig[pos[0]]; pos[0] += 1
            if c in 'CV':
                q.append('const' if c == 'C' else 'volatile')
            else:
                sign = 'unsigned ' if c == 'U' else 'signed '
        c = sig[pos[0]]
        pre = (' '.join(q) + ' ') if q else ''
        if c in 'PR':
            pos[0] += 1
            inner = one()
            p = '*' if c == 'P' else '&'
            return lambda d, inner=inner: inner(('(' + p + ' '.join(q) + (' ' if q else '') + d + ')')
                                                if inner.__name__ == 'fn' or inner.__name__ == 'arr'
                                                else p + (' '.join(q) + ' ' if q else '') + d)
        if c == 'A':
            pos[0] += 1
            j = sig.index('_', pos[0]); n = sig[pos[0]:j]; pos[0] = j + 1
            elem = one()
            def arr(d, elem=elem, n=n):
                return elem(d + '[%s]' % n)
            return arr
        if c == 'F':
            pos[0] += 1
            ps = []
            while pos[0] < len(sig) and sig[pos[0]] != '_':
                ps.append(one())
            pos[0] += 1
            ret = one()
            def fn(d, ps=ps, ret=ret):
                args = ', '.join(x('') for x in ps)
                return ret(d + '(' + ('void' if args == 'void' else args) + ')')
            return fn
        if c == 'Q':
            k = int(sig[pos[0] + 1]); pos[0] += 2
            parts = []
            for _ in range(k):
                j = pos[0]
                while sig[j].isdigit():
                    j += 1
                n = int(sig[pos[0]:j]); parts.append(sig[j:j + n]); pos[0] = j + n
            name = parts[-1]        # nested types are top-level in the DWARF (and in this header)
            return lambda d, name=name, pre=pre: pre + name + (' ' + d if d else '')
        if c.isdigit():
            j = pos[0]
            while sig[j].isdigit():
                j += 1
            n = int(sig[pos[0]:j]); name = sig[j:j + n]; pos[0] = j + n
            return lambda d, name=name, pre=pre: pre + name + (' ' + d if d else '')
        if c == 'e':
            pos[0] += 1
            return lambda d: '...'
        if c in BASIC:
            pos[0] += 1
            return lambda d, b=sign + BASIC[c], pre=pre: pre + b + (' ' + d if d else '')
        raise ValueError('cannot demangle %r at %d' % (sig, pos[0]))

    while pos[0] < len(sig):
        if sig[pos[0]] == 'T' or sig[pos[0]] == 'N':
            raise ValueError('repeat code in %r' % sig)
        out.append(one())
    return out


def split_mangled(m):
    # -> (function name, class or None, const, parameter signature) or None
    i = m.find('__', 2 if m.startswith('__') else 1)
    if i < 0:
        return None
    name, rest = m[:i], m[i + 2:]
    cls = None
    mm = re.match(r'(\d+)', rest)
    if mm:
        n = int(mm.group(1)); cls = rest[len(mm.group(1)):len(mm.group(1)) + n]; rest = rest[len(mm.group(1)) + n:]
    const = rest.startswith('C')
    if const:
        rest = rest[1:]
    if not rest.startswith('F'):
        return None
    return name, cls, const, rest[1:]


class Gen:
    def __init__(self, elf):
        self.d = elf
        self.t = dwarf1.dies(elf)
        self.cus = [c for c in self.t.values() if c.tag == 'compile_unit' and '\\dare\\' in c.attrs['name'].lower()]
        self.names = {}
        self.typedef_of = {}
        for cu in self.cus:
            for k in cu.children:
                x = self.t[k]
                if x.tag == 'typedef' and 'user_def_type' in x.attrs:
                    tgt = self.t[x.attrs['user_def_type']]
                    if tgt.tag in RECORD + ('enumeration_type',) and not tgt.attrs.get('name'):
                        self.typedef_of[tgt.off] = x.attrs['name']

    # canonical name of a record/enum/typedef DIE
    def name(self, off):
        if off in self.names:
            return self.names[off]
        x = self.t[off]
        n = x.attrs.get('name')
        if n and n.startswith('@'):     # CodeWarrior's anonymous names (@class, @anon0) are not unique
            n = None
        if not n and off in self.typedef_of:
            n = self.typedef_of[off] + '__s'
        if not n:
            sig = '%s %d ' % (x.tag, x.attrs.get('byte_size', 0)) + ' '.join(
                '%s@%s' % (self.t[c].attrs.get('name'), self.t[c].attrs.get('location', b'').hex()) for c in x.children)
            n = '__anon_%08x' % zlib.crc32(sig.encode())
        self.names[off] = n
        return n

    def base(self, a):
        # (kind, value): kind 'ft' fund type code or 'udt' DIE offset; plus modifier bytes (outermost first)
        if 'fund_type' in a:
            return [], ('ft', a['fund_type'])
        if 'user_def_type' in a:
            return [], ('udt', a['user_def_type'])
        if 'mod_fund_type' in a:
            b = a['mod_fund_type']
            return list(b[:-2]), ('ft', struct.unpack('<H', b[-2:])[0])
        if 'mod_u_d_type' in a:
            b = a['mod_u_d_type']
            return list(b[:-4]), ('udt', struct.unpack('<I', b[-4:])[0])
        return [], ('ft', 0x14)

    def render(self, a, inner=''):
        mods, b = self.base(a)
        return self._render(mods, b, inner)

    def _render(self, mods, b, inner):
        sp = lambda s: (' ' + s) if s and not s.startswith(('*', '&', '[', '(')) else s
        if mods:
            m, rest = mods[0], mods[1:]
            if m in (1, 2):
                p = '*' if m == 1 else '&'
                if not rest and b[0] == 'udt' and self.t[b[1]].tag in ('array_type', 'subroutine_type'):
                    return self._render(rest, b, '(%s%s)' % (p, inner))
                return self._render(rest, b, p + inner)
            q = 'const' if m == 3 else 'volatile'
            if not rest:
                return '%s %s' % (q, self._render([], b, inner).strip())
            return self._render(rest, b, '%s %s' % (q, inner) if inner else q)
        if b[0] == 'ft':
            return (FT.get(b[1], 'int /*ft %#x*/' % b[1]) + sp(inner)).replace('* *', '**')
        x = self.t[b[1]]
        if x.tag == 'array_type':
            return self.array(x, inner)
        if x.tag == 'subroutine_type':
            ps = [self.render(self.t[c].attrs) for c in x.children if self.t[c].tag == 'formal_parameter']
            if any(self.t[c].tag == 'unspecified_parameters' for c in x.children):
                ps.append('...')
            return self.render(x.attrs, '%s(%s)' % (inner, ', '.join(ps) or 'void'))
        if x.tag in ('pointer_type', 'reference_type'):
            return self.render(x.attrs, ('*' if x.tag == 'pointer_type' else '&') + inner)
        return self.name(b[1]) + sp(inner)

    def array(self, x, inner):
        s, q, dims, elem = x.attrs['subscr_data'], 0, [], None
        while q < len(s):
            fmt = s[q]; q += 1
            if fmt == 0:      # FT_C_C: index fund type, low, high
                lo, hi = struct.unpack_from('<ii', s, q + 2); q += 10
                dims.append(hi - lo + 1)
            elif fmt == 8:    # FMT_ET: element type attribute
                at, = struct.unpack_from('<H', s, q); q += 2
                form = at & 0xf
                if form in (1, 2, 6):
                    v = struct.unpack_from('<I', s, q)[0]; q += 4
                elif form == 5:
                    v = struct.unpack_from('<H', s, q)[0]; q += 2
                else:
                    n = struct.unpack_from('<H', s, q)[0]; v = s[q + 2:q + 2 + n]; q += 2 + n
                elem = {0x005: 'fund_type', 0x007: 'user_def_type', 0x006: 'mod_fund_type',
                        0x008: 'mod_u_d_type'}[at >> 4]
                elem = {elem: v}
            else:
                raise ValueError('subscr format %d' % fmt)
        return self.render(elem, inner + ''.join('[%d]' % n for n in dims))

    # by-value dependencies of a type use (for ordering definitions)
    def deps(self, a):
        mods, b = self.base(a)
        if any(m in (1, 2) for m in mods) or b[0] == 'ft':
            return set()
        x = self.t[b[1]]
        if x.tag == 'array_type':
            return self.deps(self.array_elem(x))
        if x.tag == 'subroutine_type':
            return set()
        return {self.name(b[1])}

    def array_elem(self, x):
        out = {}
        orig = self.render
        self.render = lambda a, inner='': out.update(a) or ''
        try:
            self.array(x, '')
        finally:
            self.render = orig
        return out


def member_offset(loc):
    # OP_CONST n, OP_ADD
    return struct.unpack_from('<I', loc, 1)[0] if loc and loc[0] == 4 else 0


def main(elf_path, out_h, out_tsv=None):
    elf = open(elf_path, 'rb').read()
    g = Gen(elf)
    t = g.t
    syms, word = symbols(elf)
    fsym = {v: n for n, v, sz, ty in syms if ty == 2}

    # one definition per name: the most common layout (warn on others)
    defs = defaultdict(list)
    enums, typedefs, globals_, funcs = {}, {}, {}, {}
    for cu in g.cus:
        for k in cu.children:
            x = t[k]
            if x.tag in RECORD and (x.children or x.attrs.get('byte_size')):
                defs[g.name(k)].append(x)
            elif x.tag == 'enumeration_type':
                enums.setdefault(g.name(k), x)
            elif x.tag == 'typedef':
                typedefs.setdefault(x.attrs['name'], x)
            elif x.tag == 'global_variable' and x.attrs.get('name'):
                globals_.setdefault(x.attrs['name'], (x, cu))
            elif x.tag in ('global_subroutine', 'subroutine') and x.attrs.get('name'):
                funcs.setdefault(x.attrs.get(0x200, x.attrs['name']), (x, cu))
    records, conflicts = {}, []
    for n, xs in defs.items():
        sigs = Counter((x.attrs.get('byte_size'), len(x.children)) for x in xs)
        best = sigs.most_common(1)[0][0]
        full = [x for x in xs if (x.attrs.get('byte_size'), len(x.children)) == best]
        records[n] = full[0]
        if len(sigs) > 1:
            conflicts.append('%s: %s' % (n, dict(sigs)))

    # vtables: class -> [function symbol per slot] (entries from +8)
    vt = {}
    for n, v, sz, ty in syms:
        if n.startswith('__vt__') and sz >= 8:
            m = re.match(r'__vt__(\d+)(.*)', n)
            if m and len(m.group(2)) == int(m.group(1)):
                vt[m.group(2)] = [fsym.get(word(v + o), '?') for o in range(8, sz, 4)]

    base_of = {}
    for n, xs in defs.items():
        for x in xs[:1]:
            bs = [t[c] for c in x.children if t[c].tag == 'inheritance']
            if bs:
                base_of[n] = g.render(bs[0].attrs).strip()

    def ancestors(n):
        out = []
        while n in base_of:
            n = base_of[n]
            out.append(n)
        return out

    # methods by class
    methods = defaultdict(list)
    cfuncs = []
    for m, (x, cu) in sorted(funcs.items()):
        cls, const = mangled_class(m)
        if cls:
            methods[cls].append((m, x, const))
        elif '__' not in m.strip('_') or m == x.attrs['name']:
            cfuncs.append((m, x))

    def params(x, skip_this=True):
        ps = []
        for c in x.children:
            p = t[c]
            if p.tag == 'formal_parameter':
                if skip_this and p.attrs.get('name') == 'this':
                    continue
                ps.append(g.render(p.attrs, p.attrs.get('name', '')))
            elif p.tag == 'unspecified_parameters':
                ps.append('...')
        return ps

    def decl(m, x, in_class=None, const=False, virtual=False):
        # parameter types from the mangled name (DWARF sometimes lists none), names and return type from DWARF
        sm = split_mangled(m)
        name = x.attrs['name'] if x is not None else (sm[0] if sm else m)
        dw = [] if x is None else [t[c] for c in x.children if t[c].tag in ('formal_parameter', 'unspecified_parameters')]
        dw = [p for p in dw if p.attrs.get('name') != 'this']
        ps = None
        if sm:
            try:
                ms = demangle_params(sm[3])
                if len(ms) == 1 and ms[0]('') == 'void':
                    ms = []
                nm = [p.attrs.get('name', '') if len(dw) == len(ms) else '' for p in dw] if len(dw) == len(ms) else [''] * len(ms)
                ps = ', '.join(f(nm[i]) for i, f in enumerate(ms)) or 'void'
            except (ValueError, IndexError):
                ps = None
        if ps is None:
            ps = ', '.join(params(x)) if x is not None else '...'
            ps = ps or 'void'
        ps = C_PARAMS.get(m, ps) if not in_class else ps
        if name == '__ct' or m.startswith('__ct__'):
            return '%s(%s);' % (in_class, ps)
        if name == '__dt' or m.startswith('__dt__'):
            return '%s~%s();' % ('virtual ' if virtual else '', in_class)
        if name.startswith('__') and name[2:4] in ('nw', 'dl', 'as', 'eq', 'ne', 'pl', 'mi', 'ml', 'dv', 'vc', 'cl', 'op'):
            return '/* %s: operator, not declared */' % m
        ret = g.render(x.attrs, name + '(%s)' % ps) if x is not None else 'void %s(%s) /* return type unknown */' % (name, ps)
        static = (in_class, name) in STATIC_METHODS
        pre = ('virtual ' if virtual else '') + ('static ' if static else '')
        return pre + ret + (' const' if const else '') + ';'

    # order record definitions: by-value deps first
    rec_deps = {}
    for n, x in records.items():
        dd = set()
        for c in x.children:
            dd |= g.deps(t[c].attrs)
        rec_deps[n] = dd
    td_deps = {n: g.deps(x.attrs) for n, x in typedefs.items()}
    done, order = set(enums), []

    def visit(n, stack=()):
        if n in done or n in stack:
            return
        for dname in sorted(rec_deps.get(n, set()) | td_deps.get(n, set())):
            visit(dname, stack + (n,))
        done.add(n)
        order.append(n)
    for n in sorted(set(records) | set(typedefs)):
        visit(n)

    L = ['// Generated by tools/decomp/dare_header.py from POP1.ELF DWARF (DARE sound engine). Do not edit:',
         '// fixes go in dare_fix.h (included last) or into the generator.',
         '#ifndef DARE_H', '#define DARE_H',
         '#define DARE_CHECK(n, c) typedef char n[(c) ? 1 : -1]',
         '#define DARE_OFS(t, m) ((unsigned long)&((t *)0)->m)', '']
    def is_union(n, x):
        # CodeWarrior tags unions as classes: every member at offset 0 means a union
        mems = [t[c] for c in x.children if t[c].tag == 'member']
        return x.tag == 'union_type' or (len(mems) > 1 and n not in vt and len(mems) == len(x.children) and
                                         all(member_offset(m.attrs.get('location')) == 0 for m in mems))
    unions = {n for n, x in records.items() if is_union(n, x)}
    L += ['%s %s;' % ('union' if n in unions else 'struct', n) for n in sorted(records) if n not in TEMPLATED]
    # records only ever used through pointers in DARE (defined elsewhere): forward declarations
    extra = sorted({g.name(o) for cu in g.cus for k in cu.children for o in refs(t, t[k])
                    if t[o].tag in RECORD} - set(records))
    L += ['struct %s;  // not defined in DARE' % n for n in extra] + ['']
    seen_enumerators = set()
    for n, x in sorted(enums.items()):
        vals = []
        el = x.attrs.get('element_list', b'')
        q = 0
        w = x.attrs.get('byte_size', 4)
        while q < len(el):          # element_list: (value of the enum's own size, name) pairs
            v = int.from_bytes(el[q:q + w], 'little', signed=True)
            z = el.index(b'\0', q + w)
            vals.append('%s = %d' % (el[q + w:z].decode('latin-1'), v))
            q = z + 1
        # the same enumerator in two enums (files never included together): later ones get the enum's prefix
        vals = [v if v.split(' = ')[0] not in seen_enumerators else '%s__%s' % (n, v) for v in vals]
        seen_enumerators.update(v.split(' = ')[0] for v in vals)
        L.append('enum %s { %s };' % (n, ', '.join(vals)))
    L.append('')
    vtable_of = {}
    plain = {s[0] for s in syms}
    # globals whose POP1 symbol is name__<n>Class are static data members
    static_members = {}
    for s in syms:
        mm = re.match(r'(\w+?)__(\d+)(\w+)$', s[0])
        k = int(mm.group(2)) if mm else 0
        if (mm and mm.group(1) in globals_ and mm.group(1) not in plain and mm.group(3)[:k] in records
                and not mm.group(3)[k:].lstrip('C').startswith('F')):
            static_members[mm.group(1)] = mm.group(3)[:k]

    def gdecl(n):
        if (None, n) in SNDCLIST_ARGS:
            return 'sndCList<%s> %s' % (SNDCLIST_ARGS[None, n], n)
        return g.render(globals_[n][0].attrs, n)
    L += [SNDCLIST, '']
    for n in order:
        if n in TEMPLATED:
            continue
        if n in typedefs:
            x = typedefs[n]
            mods, b = g.base(x.attrs)
            if b[0] == 'udt' and not mods and g.name(b[1]) == n:
                continue
            L.append('typedef %s;' % g.render(x.attrs, n))
            continue
        x = records[n]
        bases = [t[c] for c in x.children if t[c].tag == 'inheritance']
        mems = [t[c] for c in x.children if t[c].tag == 'member']
        kw = 'union' if n in unions else 'struct'
        head = '%s %s' % (kw, n)
        if bases:
            head += ' : ' + ', '.join(g.render(b.attrs) for b in bases)
        L.append(head + ' {')
        slots = vt.get(n, [])
        base_slots = len(vt.get(g.render(bases[0].attrs).strip(), [])) if bases else 0
        virt = set(slots)
        mlines = []
        for i, sl in enumerate(slots):
            sm = split_mangled(sl) if sl != '?' else None
            sc = sm[1] if sm else None
            if sc == n:
                mlines.append('    ' + decl(sl, funcs[sl][0] if sl in funcs else None, n, sm[2], virtual=True))
            elif i >= base_slots:
                over = Counter(dv[i] for dn, dv in vt.items() if dn != n and len(dv) > i and dv[i] != '?'
                               and n in ancestors(dn))
                if over:
                    o = over.most_common(1)[0][0]
                    om = split_mangled(o)
                    mlines.append('    ' + decl(o.replace('__%d%s' % (len(om[1]), om[1]), '__%d%s' % (len(n), n)),
                                                 funcs[o][0] if o in funcs else None, n, om[2], virtual=True)
                                  + '  // slot %d, named after %s' % (i, o))
                else:
                    mlines.append('    virtual void __slot%d();  // %s' % (i, sl))
        for m, fx, c in methods.get(n, []):
            if m not in virt:
                mlines.append('    ' + decl(m, fx, n, c))
        mlines += ['    static %s;' % gdecl(k) for k in sorted(static_members) if static_members[k] == n]
        pos = max([member_offset(b.attrs.get('location')) + records.get(g.render(b.attrs).strip(), x).attrs.get(
            'byte_size', 0) for b in bases] or [0])
        if n in vt and not bases:
            pos = 4         # the vptr: CodeWarrior puts it where the first virtual is declared, so methods first
            L += mlines
            mlines = []
        for mbr in mems:
            off = member_offset(mbr.attrs.get('location'))
            if kw == 'struct' and off > pos:
                L.append('    char _pad%x[%d];' % (pos, off - pos))
            nm = mbr.attrs.get('name') or '_m%x' % off
            if 'bit_size' in mbr.attrs:
                L.append('    %s : %d;' % (g.render(mbr.attrs, nm), mbr.attrs['bit_size']))
            elif (n, nm) in MEMBER_TYPES:
                L.append('    %s;' % MEMBER_TYPES[n, nm].format(nm))
                sz = 12
            elif mbr.attrs.get('fund_type') == 0x14:
                L.append('    void *%s;  // DWARF lost this type (a function pointer?)' % nm)
            elif (n, nm) in SNDCLIST_ARGS:
                L.append('    sndCList<%s> %s;' % (SNDCLIST_ARGS[n, nm], nm))
            else:
                L.append('    %s;' % g.render(mbr.attrs, nm))
            sz = 12 if (n, nm) in MEMBER_TYPES else (mbr.attrs.get('byte_size') or g_size(g, t, records, mbr.attrs))
            pos = max(pos, off + (sz or 0))
        if not mems and not bases and n not in vt and x.attrs.get('byte_size'):
            L.append('    char _data[%d];  // no members in DWARF' % x.attrs['byte_size'])
        L += mlines
        L.append('};')
        bs = x.attrs.get('byte_size')
        if bs:
            L.append('DARE_CHECK(__size_%s, sizeof(%s) == %d);' % (n, n, bs))
        for mbr in mems:
            if 'bit_size' not in mbr.attrs and mbr.attrs.get('name') and kw == 'struct':
                L.append('DARE_CHECK(__ofs_%s_%s, DARE_OFS(%s, %s) == %d);' % (
                    n, mbr.attrs['name'], n, mbr.attrs['name'], member_offset(mbr.attrs.get('location'))))
    L.append('')
    # functions known only by a declaration whose POP1 symbol is mangled were compiled as C++ (fn_vUbiEELoop__FPv)
    mangled = {s[0].split('__F')[0] for s in syms if '__F' in s[0][1:]}
    cplus = {n for n in globals_ if n in mangled and n not in plain}
    L.append('extern "C" {')
    for m, x in cfuncs:
        L.append(decl(m, x))
    for n, (x, cu) in sorted(globals_.items()):
        if n not in cplus and n not in static_members:
            L.append('extern %s;' % gdecl(n))
    L += ['}'] + ['extern %s;' % gdecl(n) for n in sorted(cplus)] + ['', '#include "dare_fix.h"', '#endif']
    open(out_h, 'w').write('\n'.join(L) + '\n')
    print('%d records, %d enums, %d typedefs, %d methods, %d C functions, %d globals, %d vtables -> %s' % (
        len(records), len(enums), len(typedefs), sum(map(len, methods.values())), len(cfuncs), len(globals_),
        len(vt), out_h))
    if conflicts:
        print('%d names with more than one layout (most common used):' % len(conflicts), *conflicts[:20], sep='\n  ')

    if out_tsv:
        with open(out_tsv, 'w') as o:
            for m, (x, cu) in sorted(funcs.items()):
                cls, const = mangled_class(m)
                ps = '; '.join(params(x, False))
                locs = '; '.join(g.render(t[c].attrs, t[c].attrs.get('name', '')) for c in walk(t, x)
                                 if t[c].tag == 'local_variable')
                o.write('%s\t%s\t%s\t%s\t%s\n' % (m, cu.attrs['name'].split('\\engine\\')[-1].replace('\\', '/'),
                                                  decl(m, x, cls, const), ps, locs))


def refs(t, x):
    for a in [x] + [t[c] for c in walk(t, x)]:
        for k in ('user_def_type', 'mod_u_d_type'):
            v = a.attrs.get(k)
            if v is not None:
                o = v if isinstance(v, int) else struct.unpack('<I', v[-4:])[0]
                if o in t:
                    yield o


def walk(t, x):
    for c in x.children:
        yield c
        yield from walk(t, t[c])


def g_size(g, t, records, a):
    mods, b = g.base(a)
    if mods and mods[0] in (1, 2):
        return 4
    if b[0] == 'ft':
        return {1: 1, 2: 1, 3: 1, 4: 2, 5: 2, 6: 2, 0xa: 8, 0xb: 8, 0xc: 8, 0xf: 8, 0x10: 8, 0x8008: 8, 0x8108: 8, 0x8208: 8}.get(b[1], 4)
    x = t[b[1]]
    if x.tag == 'array_type':
        n = 1
        s = x.attrs['subscr_data']
        q = 0
        while q < len(s) and s[q] == 0:
            lo, hi = struct.unpack_from('<ii', s, q + 3); q += 11
            n *= hi - lo + 1
        return n * (g_size(g, t, records, g.array_elem(x)) or 0)
    if x.tag == 'typedef':
        return g_size(g, t, records, x.attrs)
    return x.attrs.get('byte_size') or records.get(g.name(b[1]), x).attrs.get('byte_size', 4)


if __name__ == '__main__':
    main(*sys.argv[1:4])
