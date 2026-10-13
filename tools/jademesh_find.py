"""Find CJMeshElement records (PS2 VU1 mesh packets) in an unpacked PS2 world pack.

Usage: python3 -I tools/jademesh_find.py <pack.bin>   (pack from: jadetex.py unpack PRINCE.BF <key> <out>)
Grammar follows CJMeshElement::ReadElement (see CLAUDE.md, "Native renderer probe").
"""
import struct,sys,math
d=open(sys.argv[1],'rb').read()
U=lambda p: struct.unpack_from('<I',d,p)[0] if p+4<=len(d) else 0xFFFFFFFF
F=lambda p: struct.unpack_from('<3f',d,p) if p+12<=len(d) else None
def fin(v): return v is not None and all(math.isfinite(x) and abs(x)<1e5 for x in v)
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
        if v>31 or s>0x100000 or s==0: return None
        lods.append((v,q,s)); q+=s
    n2=U(q); q+=4
    if not 0<=n2<=5 or (n2==0 and n==0): return None
    pk=[]
    for i in range(n2):
        idx=U(q); s=U(q+4); q+=8
        if idx>4 or s==0 or s%16 or s>0x200000: return None
        pk.append((idx,q,s)); q+=s
    return q,nb,lods,pk
hits=[]
p=0
while p<len(d)-64:
    r=elem(p)
    if r: hits.append((p,r)); p=r[0]
    else: p+=4
print(len(hits))
print('elements with LOD blobs:',sum(1 for p,r in hits if r[2]),' with pass packets:',sum(1 for p,r in hits if r[3]))
print('bytes: LOD blobs %.2f MB, pass packets %.2f MB'%(sum(s for p,r in hits for v,o,s in r[2])/1e6, sum(s for p,r in hits for i,o,s in r[3])/1e6))
for p,(e,nb,lods,pk) in hits[:12]:
    print(hex(p),'bones',nb,'lods',[(v,hex(o),s) for v,o,s in lods],'packets',[(i,hex(o),s) for i,o,s in pk], ['%.2f'%x for x in F(p)+F(p+16)])
