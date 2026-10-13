"""Find CJMeshElement records (PS2 VU1 mesh packets) in an unpacked PS2 world pack.

Usage: python3 -I tools/jademesh_find.py <pack.bin>   (pack from: jadetex.py unpack PRINCE.BF <key> <out>)
Grammar follows CJMeshElement::ReadElement (see CLAUDE.md, "Native renderer probe").
"""
import struct,sys,math
d=open(sys.argv[1],'rb').read()
U=lambda p: struct.unpack_from('<I',d,p)[0]
F=lambda p: struct.unpack_from('<3f',d,p)
def fin(v): return all(math.isfinite(x) and abs(x)<1e5 for x in v)
def elem(p):
    if not fin(F(p)) or not fin(F(p+16)): return None
    q=p+12+4+12+4+4
    nb=U(q); q+=4
    if nb>256: return None
    q+=nb
    n=U(q); q+=4
    if n>5: return None
    lods=[]
    for i in range(n):
        v=U(q); s=U(q+4); q+=8
        if v>31 or s>0x100000: return None
        q+=s
    n2=U(q); q+=4
    if not 1<=n2<=5: return None
    pk=[]
    for i in range(n2):
        idx=U(q); s=U(q+4); q+=8
        if idx>4 or s==0 or s%16 or s>0x200000: return None
        pk.append((idx,q,s)); q+=s
    return q,nb,n,pk
hits=[]
p=0
while p<len(d)-64:
    r=elem(p)
    if r: hits.append((p,r)); p=r[0]
    else: p+=4
print(len(hits))
for p,(e,nb,n,pk) in hits[:12]:
    print(hex(p),'bones',nb,'lod',n,[(i,hex(o),s) for i,o,s in pk], ['%.2f'%x for x in F(p)+F(p+16)])
