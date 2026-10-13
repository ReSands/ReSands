"""Dump a PS2 DMA chain with its VIF codes (UNPACK formats, first values).

Usage: python3 -I tools/vifdump.py <file> <hex offset> <byte length>
"""
import struct,sys
d=open(sys.argv[1],'rb').read(); o=int(sys.argv[2],16); n=int(sys.argv[3])
U=lambda p: struct.unpack_from('<I',d,p)[0]
FMT={0:'S-32',1:'S-16',2:'S-8',4:'V2-32',5:'V2-16',6:'V2-8',8:'V3-32',9:'V3-16',0xa:'V3-8',0xc:'V4-32',0xd:'V4-16',0xe:'V4-8',0xf:'V4-5'}
EL={0:4,1:2,2:1,4:8,5:4,6:2,8:12,9:6,0xa:3,0xc:16,0xd:8,0xe:4,0xf:2}
p=o; end=o+n
def vif(p,stop):
    while p<stop:
        w=U(p); cmd=(w>>24)&0x7f; num=(w>>16)&0xff; imm=w&0xffff
        if cmd>=0x60:
            f=cmd&0xf; nb=((EL[f]*(num or 256))+3)//4*4
            vals=''
            if f==8: vals=' '.join('(%.2f %.2f %.2f)'%struct.unpack_from('<3f',d,p+4+12*i) for i in range(min(3,num)))
            elif f==9 or f==0xd: 
                k=6 if f==9 else 8
                vals=' '.join(str(struct.unpack_from('<%dh'%(k//2),d,p+4+k*i)) for i in range(min(3,num)))
            elif f==0xc: vals=' '.join('(%.3f %.3f %.3f %.3f)'%struct.unpack_from('<4f',d,p+4+16*i) for i in range(min(2,num)))
            elif f in (0xe,0xa,6): vals=d[p+4:p+4+min(12,nb)].hex()
            print(f'  {p-o:5x} UNPACK {FMT.get(f,f)} num={num} addr={imm&0x3ff:#x}{" usn" if imm&0x4000 else ""}{" flg" if imm&0x8000 else ""}{" MASK" if cmd&0x10 else ""} {vals}')
            p+=4+nb
        else:
            names={0:'NOP',1:'STCYCL',2:'OFFSET',3:'BASE',4:'ITOP',5:'STMOD',6:'MSKPATH3',7:'MARK',0x10:'FLUSHE',0x11:'FLUSH',0x13:'FLUSHA',0x14:'MSCAL',0x15:'MSCALF',0x17:'MSCNT',0x20:'STMASK',0x30:'STROW',0x31:'STCOL',0x4a:'MPG',0x50:'DIRECT',0x51:'DIRECTHL'}
            nm=names.get(cmd,hex(cmd)); print(f'  {p-o:5x} {nm} imm={imm:#x} num={num}')
            p+=4
            if cmd==0x20: p+=4
            elif cmd in(0x30,0x31): p+=16
            elif cmd in(0x50,0x51): p+=16*(imm or 65536)
            elif cmd==0x4a: p+=8*(num or 256)
    return p
while p<end:
    lo,hi=struct.unpack_from('<II',d,p); qwc=lo&0xffff; tid=(lo>>28)&7; addr=hi
    print(f'{p-o:5x} DMAtag id={tid} qwc={qwc} addr={addr:#x}')
    if tid in (1,7,6,2) or tid==0: vif(p+8,p+16+16*qwc); p+=16+16*qwc
    else: vif(p+8,p+16); p+=16
