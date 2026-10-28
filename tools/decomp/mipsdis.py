# mipsdis.py: small MIPS (R5900 integer + COP1) disassembler for match feedback, same text for both sides.
# dis(word, pc) -> 'addiu sp,sp,-0x10'; unknown encodings come out as '.word 0x...'.
R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
SPECIAL = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 0xa: 'movz',
           0xb: 'movn', 0xc: 'syscall', 0xd: 'break', 0xf: 'sync', 0x10: 'mfhi', 0x11: 'mthi', 0x12: 'mflo',
           0x13: 'mtlo', 0x14: 'dsllv', 0x16: 'dsrlv', 0x17: 'dsrav', 0x18: 'mult', 0x19: 'multu', 0x1a: 'div',
           0x1b: 'divu', 0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu', 0x24: 'and', 0x25: 'or',
           0x26: 'xor', 0x27: 'nor', 0x28: 'mfsa', 0x29: 'mtsa', 0x2a: 'slt', 0x2b: 'sltu', 0x2c: 'dadd',
           0x2d: 'daddu', 0x2e: 'dsub', 0x2f: 'dsubu', 0x38: 'dsll', 0x3a: 'dsrl', 0x3b: 'dsra', 0x3c: 'dsll32',
           0x3e: 'dsrl32', 0x3f: 'dsra32'}
IMM = {8: 'addi', 9: 'addiu', 0xa: 'slti', 0xb: 'sltiu', 0xc: 'andi', 0xd: 'ori', 0xe: 'xori', 0x18: 'daddi',
       0x19: 'daddiu'}
MEM = {0x1a: 'ldl', 0x1b: 'ldr', 0x1e: 'lq', 0x1f: 'sq', 0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw',
       0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr', 0x27: 'lwu', 0x28: 'sb', 0x29: 'sh', 0x2a: 'swl', 0x2b: 'sw',
       0x2c: 'sdl', 0x2d: 'sdr', 0x2e: 'swr', 0x2f: 'cache', 0x31: 'lwc1', 0x33: 'pref', 0x36: 'lqc2',
       0x37: 'ld', 0x39: 'swc1', 0x3e: 'sqc2', 0x3f: 'sd'}
BR = {4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 0x14: 'beql', 0x15: 'bnel', 0x16: 'blezl', 0x17: 'bgtzl'}
REGIMM = {0: 'bltz', 1: 'bgez', 2: 'bltzl', 3: 'bgezl', 0x10: 'bltzal', 0x11: 'bgezal'}
FPU_S = {0: 'add.s', 1: 'sub.s', 2: 'mul.s', 3: 'div.s', 4: 'sqrt.s', 5: 'abs.s', 6: 'mov.s', 7: 'neg.s',
         0x16: 'rsqrt.s', 0x18: 'adda.s', 0x19: 'suba.s', 0x1a: 'mula.s', 0x1c: 'madd.s', 0x1d: 'msub.s',
         0x1e: 'madda.s', 0x1f: 'msuba.s', 0x24: 'cvt.w.s', 0x28: 'max.s', 0x29: 'min.s', 0x30: 'c.f.s',
         0x32: 'c.eq.s', 0x34: 'c.lt.s', 0x36: 'c.le.s'}
MMI = {0x00: 'madd', 0x01: 'maddu', 0x04: 'plzcw', 0x10: 'mfhi1', 0x11: 'mthi1', 0x12: 'mflo1', 0x13: 'mtlo1',
       0x18: 'mult1', 0x19: 'multu1', 0x1a: 'div1', 0x1b: 'divu1', 0x20: 'madd1', 0x21: 'maddu1'}


def h(v):
    return ('-0x%x' % -v) if v < 0 else ('0x%x' % v)


def dis(w, pc=0):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xffff
    simm = imm - 0x10000 if imm & 0x8000 else imm
    tgt = (pc + 4 + (simm << 2)) & 0xffffffff
    if w == 0:
        return 'nop'
    if op == 0:
        n = SPECIAL.get(fn)
        if n is None:
            return '.word 0x%08x' % w
        if fn in (0, 2, 3, 0x38, 0x3a, 0x3b, 0x3c, 0x3e, 0x3f):
            return '%s %s,%s,%d' % (n, R[rd], R[rt], sa)
        if fn in (4, 6, 7, 0x14, 0x16, 0x17):
            return '%s %s,%s,%s' % (n, R[rd], R[rt], R[rs])
        if fn == 8:
            return 'jr %s' % R[rs]
        if fn == 9:
            return 'jalr %s,%s' % (R[rd], R[rs])
        if fn in (0xc, 0xd, 0xf):
            return n
        if fn in (0x10, 0x12, 0x28):
            return '%s %s' % (n, R[rd])
        if fn in (0x11, 0x13, 0x29):
            return '%s %s' % (n, R[rs])
        if fn in (0x18, 0x19, 0x1a, 0x1b):
            return '%s %s,%s,%s' % (n, R[rd], R[rs], R[rt]) if rd else '%s %s,%s' % (n, R[rs], R[rt])
        return '%s %s,%s,%s' % (n, R[rd], R[rs], R[rt])
    if op == 1:
        n = REGIMM.get(rt)
        return '%s %s,0x%x' % (n, R[rs], tgt) if n else '.word 0x%08x' % w
    if op in (2, 3):
        return '%s 0x%x' % ('j' if op == 2 else 'jal', ((pc + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2))
    if op in BR:
        if op in (4, 0x14) and rt == 0 and rs == 0:
            return 'b 0x%x' % tgt
        if op in (6, 7, 0x16, 0x17):
            return '%s %s,0x%x' % (BR[op], R[rs], tgt)
        return '%s %s,%s,0x%x' % (BR[op], R[rs], R[rt], tgt)
    if op in IMM:
        if op in (0xc, 0xd, 0xe):
            return '%s %s,%s,0x%x' % (IMM[op], R[rt], R[rs], imm)
        return '%s %s,%s,%s' % (IMM[op], R[rt], R[rs], h(simm))
    if op == 0xf:
        return 'lui %s,0x%x' % (R[rt], imm)
    if op in MEM:
        t = ('$f%d' % rt) if op in (0x31, 0x39) else ('vf%d' % rt) if op in (0x36, 0x3e) else R[rt]
        return '%s %s,%s(%s)' % (MEM[op], t, h(simm), R[rs])
    if op == 0x11:      # COP1
        if rs == 0:
            return 'mfc1 %s,$f%d' % (R[rt], rd)
        if rs == 4:
            return 'mtc1 %s,$f%d' % (R[rt], rd)
        if rs == 2:
            return 'cfc1 %s,$%d' % (R[rt], rd)
        if rs == 6:
            return 'ctc1 %s,$%d' % (R[rt], rd)
        if rs == 8:
            return '%s 0x%x' % (['bc1f', 'bc1t', 'bc1fl', 'bc1tl'][rt & 3], tgt)
        fs, fd = rd, sa
        if rs == 0x10:
            n = FPU_S.get(fn)
            if n is None:
                return '.word 0x%08x' % w
            if fn >= 0x30:
                return '%s $f%d,$f%d' % (n, fs, rt)
            if fn in (4, 5, 6, 7, 0x24):
                return '%s $f%d,$f%d' % (n, fd, fs)
            if fn in (0x18, 0x19, 0x1a, 0x1e, 0x1f):
                return '%s $f%d,$f%d' % (n, fs, rt)
            return '%s $f%d,$f%d,$f%d' % (n, fd, fs, rt)
        if rs == 0x14 and fn == 0x20:
            return 'cvt.s.w $f%d,$f%d' % (fd, fs)
        return '.word 0x%08x' % w
    if op == 0x1c:      # MMI (only the scalar ones DARE-style code uses)
        n = MMI.get(fn)
        if n:
            return '%s %s,%s,%s' % (n, R[rd], R[rs], R[rt])
        return 'mmi 0x%08x' % w
    return '.word 0x%08x' % w


if __name__ == '__main__':
    # self-check against lines Ghidra printed for retail GetPos__17OneShotSAMPLEPathFv (0x167e40)
    for w, pc, want in [(0x27bdfff0, 0x167e40, 'addiu sp,sp,-0x10'), (0xffbf0000, 0, 'sd ra,0x0(sp)'),
                        (0x8c840078, 0, 'lw a0,0x78(a0)'), (0x10800003, 0x167e4c, 'beq a0,zero,0x167e5c'),
                        (0x3c02ffff, 0, 'lui v0,0xffff'), (0x03e00008, 0, 'jr ra')]:
        assert dis(w, pc) == want, (hex(w), dis(w, pc), want)
    print('ok')
