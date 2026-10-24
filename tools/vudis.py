"""VU micro-mode disassembler (VU1 programs uploaded with VIF MPG), from the public VU instruction
encoding. Each instruction is 64 bits: lower word first in memory, then the upper word.
Usage (module): disasm(code_bytes, base_pc) -> list of text lines.
"""
import struct

BC = 'xyzw'
UPPER = {0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi', 0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi', 0x23: 'MADDi',
         0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi', 0x28: 'ADD', 0x29: 'MADD', 0x2A: 'MUL', 0x2B: 'MAX',
         0x2C: 'SUB', 0x2D: 'MSUB', 0x2E: 'OPMSUB', 0x2F: 'MINI'}
UPPER_BC = {0x00: 'ADD', 0x04: 'SUB', 0x08: 'MADD', 0x0C: 'MSUB', 0x10: 'MAX', 0x14: 'MINI', 0x18: 'MUL'}
UPPER_SP = {0x10: 'ITOF0', 0x11: 'ITOF4', 0x12: 'ITOF12', 0x13: 'ITOF15', 0x14: 'FTOI0', 0x15: 'FTOI4', 0x16: 'FTOI12',
            0x17: 'FTOI15', 0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIP', 0x20: 'ADDAq', 0x21: 'MADDAq',
            0x22: 'ADDAi', 0x23: 'MADDAi', 0x24: 'SUBAq', 0x25: 'MSUBAq', 0x26: 'SUBAi', 0x27: 'MSUBAi', 0x28: 'ADDA',
            0x29: 'MADDA', 0x2A: 'MULA', 0x2C: 'SUBA', 0x2D: 'MSUBA', 0x2E: 'OPMULA', 0x2F: 'NOP'}
UPPER_SP_BC = {0x00: 'ADDA', 0x04: 'SUBA', 0x08: 'MADDA', 0x0C: 'MSUBA', 0x18: 'MULA'}
LOWER = {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW', 0x08: 'IADDIU', 0x09: 'ISUBIU', 0x10: 'FCEQ', 0x11: 'FCSET',
         0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR', 0x18: 'FMEQ', 0x1A: 'FMAND',
         0x1B: 'FMOR', 0x1C: 'FCGET', 0x20: 'B', 0x21: 'BAL', 0x24: 'JR', 0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE',
         0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
LOWER_SP = {0x30: 'IADD', 0x31: 'ISUB', 0x32: 'IADDI', 0x34: 'IAND', 0x35: 'IOR'}
LOWER_SP2 = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV', 0x39: 'SQRT',
             0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR', 0x3F: 'ISWR', 0x40: 'RNEXT',
             0x41: 'RGET', 0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP', 0x68: 'XTOP', 0x69: 'XITOP', 0x6C: 'XGKICK',
             0x70: 'ESADD', 0x71: 'ERSADD', 0x72: 'ELENG', 0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz',
             0x76: 'ESUM', 0x78: 'ESQRT', 0x79: 'ERSQRT', 0x7A: 'ERCPR', 0x7B: 'WAITP', 0x7C: 'ESIN', 0x7D: 'EATAN', 0x7E: 'EEXP'}


def dest(w):
    d = (w >> 21) & 0xF
    return ''.join(c for i, c in enumerate('xyzw') if d & (8 >> i))


def upper(w):
    op, ft, fs, fd = w & 0x3F, (w >> 16) & 0x1F, (w >> 11) & 0x1F, (w >> 6) & 0x1F
    d = dest(w)
    if op < 0x1C:
        base = op & 0x3C
        return '%s%s.%s vf%d, vf%d, vf%d%s' % (UPPER_BC.get(base, '?%x' % op), BC[op & 3], d, fd, fs, ft, BC[op & 3])
    if op < 0x30:
        name = UPPER.get(op, '?%x' % op)
        if name.endswith(('q', 'i')) and name not in ('MINI',):
            return '%s.%s vf%d, vf%d, %s' % (name, d, fd, fs, name[-1].upper())
        return '%s.%s vf%d, vf%d, vf%d' % (name, d, fd, fs, ft)
    if op >= 0x3C:
        op11 = ((w >> 6) & 0x1F) << 2 | (w & 3)
        if op11 < 0x10 or 0x18 <= op11 < 0x1C:
            base = op11 & 0x3C
            return '%s%s.%s ACC, vf%d, vf%d%s' % (UPPER_SP_BC.get(base, '?%x' % op11), BC[op11 & 3], d, fs, ft, BC[op11 & 3])
        name = UPPER_SP.get(op11, '?sp%x' % op11)
        if name == 'NOP':
            return 'NOP'
        if name == 'CLIP':
            return 'CLIPw.xyz vf%d, vf%d' % (fs, ft)
        if name.startswith(('ITOF', 'FTOI', 'ABS')):
            return '%s.%s vf%d, vf%d' % (name, d, ft, fs)
        if name.endswith(('q', 'i')):
            return '%s.%s ACC, vf%d, %s' % (name, d, fs, name[-1].upper())
        return '%s.%s ACC, vf%d, vf%d' % (name, d, fs, ft)
    return '?u%08x' % w


def lower(w, pc):
    if w == 0x8000033C:
        return 'NOP'
    if w & 0x80000000:
        op = w & 0x3F
        it, is_, id_ = (w >> 16) & 0x1F, (w >> 11) & 0x1F, (w >> 6) & 0x1F
        ft, fs = it, is_
        d = dest(w)
        if op < 0x3C:
            name = LOWER_SP.get(op, '?l%x' % op)
            if name == 'IADDI':
                imm = (w >> 6) & 0x1F
                imm = imm - 32 if imm & 0x10 else imm
                return 'IADDI vi%d, vi%d, %d' % (it, is_, imm)
            return '%s vi%d, vi%d, vi%d' % (name, id_, is_, it)
        op11 = ((w >> 6) & 0x1F) << 2 | (w & 3)
        name = LOWER_SP2.get(op11, '?l2_%x' % op11)
        fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
        if name in ('DIV',):
            return 'DIV Q, vf%d%s, vf%d%s' % (fs, BC[fsf], ft, BC[ftf])
        if name in ('SQRT', 'RSQRT'):
            return '%s Q, vf%d%s, vf%d%s' % (name, fs, BC[fsf], ft, BC[ftf])
        if name in ('LQI', 'LQD'):
            return '%s.%s vf%d, (vi%d%s)' % (name, d, ft, is_, '++' if name == 'LQI' else '--')
        if name in ('SQI', 'SQD'):
            return '%s.%s vf%d, (vi%d%s)' % (name, d, fs, it, '++' if name == 'SQI' else '--')
        if name in ('MOVE', 'MR32'):
            return '%s.%s vf%d, vf%d' % (name, d, ft, fs)
        if name == 'MTIR':
            return 'MTIR vi%d, vf%d%s' % (it, fs, BC[fsf])
        if name == 'MFIR':
            return 'MFIR.%s vf%d, vi%d' % (d, ft, is_)
        if name in ('ILWR', 'ISWR'):
            return '%s.%s vi%d, (vi%d)' % (name, d, it, is_)
        if name == 'XGKICK':
            return 'XGKICK vi%d' % is_
        if name in ('XTOP', 'XITOP'):
            return '%s vi%d' % (name, it)
        if name == 'MFP':
            return 'MFP.%s vf%d, P' % (d, ft)
        if name in ('RINIT', 'RXOR'):
            return '%s R, vf%d%s' % (name, fs, BC[fsf])
        if name in ('RGET', 'RNEXT'):
            return '%s.%s vf%d, R' % (name, d, ft)
        if name.startswith('E') or name == 'WAITQ' or name == 'WAITP':
            return '%s P, vf%d' % (name, fs) if name.startswith('E') else name
        return '%s vf%d, vf%d' % (name, ft, fs)
    op = (w >> 25) & 0x7F
    name = LOWER.get(op, '?L%x' % op)
    it, is_ = (w >> 16) & 0x1F, (w >> 11) & 0x1F
    imm11 = w & 0x7FF
    imm11 = imm11 - 0x800 if imm11 & 0x400 else imm11
    d = dest(w)
    if name in ('LQ', 'SQ'):
        return '%s.%s vf%d, %d(vi%d)' % (name, d, it if name == 'LQ' else is_, imm11, is_ if name == 'LQ' else it)
    if name in ('ILW', 'ISW'):
        return '%s.%s vi%d, %d(vi%d)' % (name, d, it, imm11, is_)
    if name in ('IADDIU', 'ISUBIU'):
        imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
        return '%s vi%d, vi%d, %d' % (name, it, is_, imm15)
    if name in ('B', 'BAL', 'IBEQ', 'IBNE', 'IBLTZ', 'IBGTZ', 'IBLEZ', 'IBGEZ'):
        target = pc + 8 + imm11 * 8
        if name in ('B',):
            return 'B 0x%x' % target
        if name == 'BAL':
            return 'BAL vi%d, 0x%x' % (it, target)
        if name in ('IBEQ', 'IBNE'):
            return '%s vi%d, vi%d, 0x%x' % (name, it, is_, target)
        return '%s vi%d, 0x%x' % (name, is_, target)
    if name in ('JR', 'JALR'):
        return '%s vi%d' % (name, is_) if name == 'JR' else 'JALR vi%d, vi%d' % (it, is_)
    # flag instructions (public VU encoding): the clip ones take a 24-bit immediate and leave their
    # result in vi1; FCGET and the status / MAC ones name their registers (status: 12-bit immediate)
    if name in ('FCEQ', 'FCAND', 'FCOR'):
        return '%s vi1, 0x%06x' % (name, w & 0xFFFFFF)
    if name == 'FCSET':
        return 'FCSET 0x%06x' % (w & 0xFFFFFF)
    if name == 'FCGET':
        return 'FCGET vi%d' % it
    if name in ('FSEQ', 'FSAND', 'FSOR', 'FSSET'):
        imm12 = ((w >> 10) & 0x800) | (w & 0x7FF)
        return 'FSSET 0x%03x' % imm12 if name == 'FSSET' else '%s vi%d, 0x%03x' % (name, it, imm12)
    if name in ('FMEQ', 'FMAND', 'FMOR'):
        return '%s vi%d, vi%d' % (name, it, is_)
    return '%s ?%08x' % (name, w)


def disasm(code, base_pc=0):
    out = []
    for i in range(0, len(code) - 7, 8):
        lo, up = struct.unpack_from('<II', code, i)
        pc = base_pc + i
        flags = ''.join(f for b, f in ((31, 'I'), (30, 'E'), (29, 'M'), (28, 'D'), (27, 'T')) if up & (1 << b))
        u = upper(up & 0x07FFFFFF)
        l = ('LOI %g' % struct.unpack('<f', struct.pack('<I', lo))[0]) if up & 0x80000000 else lower(lo, pc)
        out.append('%04x: %-34s %-34s %s' % (pc, u, l, flags))
    return out
