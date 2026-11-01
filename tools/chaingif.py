"""The sprites a save's last VIF1 chain sends the GS through DIRECT (post effects, halos, 2D), one line each with the
state in effect: FRAME (FBP, FBW, FBMSK), TEX0 (TBP, TBW), ALPHA + FIX, the vertex colour, XY (GS units minus 2048) and
UV (texels). PACKED A+D / RGBAQ / UV / XYZ2 only; REGLIST and IMAGE skipped. Packets VU1 builds (meshes) are not seen.
Used for the glow check (docs/gs_off.md step 4c point 6).

Usage: python3 -I tools/chaingif.py STATE
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402
from statechain import chain_words  # noqa: E402
from vifwalk import UNPACK_BYTES  # noqa: E402
s = State(sys.argv[1]); g = 0x70000620
start = s.u32(g + 0xE4 + 4 * (s.u32(g + 0xF4) ^ 1))
recs = {}
for rec, w in chain_words(s, start):
    recs.setdefault(rec, []).append(w)
gif = []
for rec in sorted(recs):
    w = recs[rec]; i = 0
    while i < len(w):
        c = w[i]; i += 1
        cmd, num, imm = (c >> 24) & 0x7F, (c >> 16) & 0xFF, c & 0xFFFF
        if cmd >= 0x60: i += (UNPACK_BYTES.get(cmd & 0xF, 4) * (num or 256) + 3) // 4
        elif cmd == 0x4A: i += 2 * (num or 256)
        elif cmd in (0x50, 0x51):
            while (i + 2) % 4: i += 1
            n = 4 * (imm or 65536); gif += w[i:i + n]; i += n
        elif cmd == 0x20: i += 1
        elif cmd in (0x30, 0x31): i += 4
st = {}; q = 0; out = []
def put(reg, v):
    st[reg] = v
    if reg == 0: st['vtx'] = []
    if reg in (4, 5):
        st.setdefault('vtx', []).append((st.get(3, 0), st.get(1, 0), v))
        prim = st.get(0, 0) & 7
        if prim == 6 and len(st['vtx']) == 2:
            (uv0, c0, p0), (uv1, c1, p1) = st['vtx']; st['vtx'] = []
            fr = st.get(0x4C, 0); tex = st.get(0x06, 0); al = st.get(0x42, 0)
            out.append(f"FBP {(fr & 0x1FF):3x} fbw {(fr >> 16) & 0x3F:2} msk {fr >> 32:08x} | TBP {tex & 0x3FFF:4x} tbw {(tex >> 14) & 0x3F:2} | "
                       f"ALPHA {al & 0xFF:02x} FIX {(al >> 32) & 0xFF:3} | rgba {c0 & 0xFFFFFFFF:08x} | xy {(p0 & 0xFFFF) / 16 - 2048:.1f},{((p0 >> 16) & 0xFFFF) / 16 - 2048:.1f}"
                       f" - {(p1 & 0xFFFF) / 16 - 2048:.1f},{((p1 >> 16) & 0xFFFF) / 16 - 2048:.1f} | uv {(uv0 & 0x3FFF) / 16:.1f},{((uv0 >> 16) & 0x3FFF) / 16:.1f} - {(uv1 & 0x3FFF) / 16:.1f},{((uv1 >> 16) & 0x3FFF) / 16:.1f}")
while q + 4 <= len(gif):
    lo = gif[q] | gif[q + 1] << 32; hi = gif[q + 2] | gif[q + 3] << 32; q += 4
    nloop, pre, prim, flg = lo & 0x7FFF, (lo >> 46) & 1, (lo >> 47) & 0x7FF, (lo >> 58) & 3
    nreg = (lo >> 60) or 16
    if pre: put(0, prim)
    if flg >= 2: q += 4 * nloop; continue
    for l in range(nloop):
        for r in range(nreg):
            d = (hi >> (4 * r)) & 0xF
            if flg == 0:
                a, b, c, e = gif[q:q + 4]; q += 4
                if d == 0xE: put(c & 0xFF, a | b << 32)
                elif d == 1: put(1, a & 0xFF | (b & 0xFF) << 8 | (c & 0xFF) << 16 | (e & 0xFF) << 24)
                elif d == 3: put(3, a & 0xFFFF | (b & 0xFFFF) << 16)  # UV
                elif d == 2: put(2, a | b << 32)
                elif d in (4, 5): put(d, a & 0xFFFF | (b & 0xFFFF) << 16)
                elif d == 0: put(0, a)
    if flg == 1: q += 4 * ((nloop * nreg + 1) // 2)
print("\n".join(out))
