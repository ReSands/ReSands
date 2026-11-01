"""Read a pop-sotc save state (PS2Recomp-sotc ps2_save_state.cpp, version 1) offline.

Layout: magic u64 '2XSTATE1', version u32, tick u64, then sections tagged by a fourcc.
EMEM: EE RAM (32 MB), scratchpad (16 KB), IOP RAM (2 MB), GS privileged registers (20 x u64),
VU0 code/data (4+4 KB), VU1 code/data (16+16 KB), then serialized fields.
EGS : GSFE fields, then GSVR: VRAM as u64 size + 4 MB.

Usage as a library: s = State(path); s.ram, s.spr, s.vram, s.gsregs, s.u32(addr), s.f32(addr)
Command line: python3 -I tools/popstate.py STATE --frame OUT.png [--alpha] [--fbp N | --drawn]   (the displayed framebuffer, FBP N, or the one the last chain drew; --alpha also OUT_alpha.png)
"""
import struct
import sys

RAM, SPR, IOP = 32 << 20, 16 << 10, 2 << 20
GSREG_NAMES = ['pmode', 'smode1', 'smode2', 'srfsh', 'synch1', 'synch2', 'syncv', 'dispfb1', 'display1',
               'dispfb2', 'display2', 'extbuf', 'extdata', 'extwrite', 'bgcolor', 'csr', 'vsync', 'imr',
               'busdir', 'siglblid']


class State:
    def __init__(self, path):
        d = open(path, 'rb').read()
        magic, version, self.tick = struct.unpack_from('<QIQ', d, 0)
        if magic != 0x3145544154535832 or version != 1:
            raise ValueError(f'{path}: not a version 1 state ({magic:#x} v{version})')
        if d[0x14:0x18] != b'EMEM':
            raise ValueError(f'{path}: EMEM section not at 0x14')
        o = 0x18
        self.ram = d[o:o + RAM]; o += RAM
        self.spr = d[o:o + SPR]; o += SPR
        self.iop = d[o:o + IOP]; o += IOP
        self.gsregs = dict(zip(GSREG_NAMES, struct.unpack_from('<20Q', d, o))); o += 160
        self.vu0code = d[o:o + 4096]; o += 4096
        self.vu0data = d[o:o + 4096]; o += 4096
        self.vu1code = d[o:o + 16384]; o += 16384
        self.vu1data = d[o:o + 16384]; o += 16384
        g = d.find(b'GSVR', o)
        size = struct.unpack_from('<Q', d, g + 4)[0]
        if size != 4 << 20:
            raise ValueError(f'{path}: VRAM size {size:#x}')
        self.vram = d[g + 12:g + 12 + size]

    def mem(self, addr, n):
        if 0x70000000 <= addr < 0x70000000 + SPR:
            a = addr - 0x70000000
            return self.spr[a:a + n]
        a = addr & 0x1FFFFFFF
        return self.ram[a:a + n]

    def u32(self, addr):
        return struct.unpack('<I', self.mem(addr, 4))[0]

    def f32(self, addr):
        return struct.unpack('<f', self.mem(addr, 4))[0]

    def floats(self, addr, n):
        return struct.unpack(f'<{n}f', self.mem(addr, 4 * n))

    def display(self):
        """(FBP in 2048-word units, FBW in 64-pixel units, PSM, width, height) of the shown circuit."""
        pm = self.gsregs['pmode']
        fb, disp = (self.gsregs['dispfb2'], self.gsregs['display2']) if pm & 2 else (self.gsregs['dispfb1'], self.gsregs['display1'])
        fbp, fbw, psm = fb & 0x1FF, (fb >> 9) & 0x3F, (fb >> 15) & 0x1F
        magh = ((disp >> 23) & 0xF) + 1
        w = (((disp >> 32) & 0xFFF) + 1) // magh
        h = ((disp >> 44) & 0x7FF) + 1
        return fbp, fbw, psm, w, h


THREAD_STATUS = ['running', 'ready', 'waiting', 'waiting+suspended', 'suspended', 'dormant']
WAIT_REASON = ['none', 'sleep', 'semaphore', 'event flag', 'vsync']


def threads(path):
    """The guest threads of ESCH (ps2_save_state.cpp serialize(GuestThread)): one dict per thread.
    R5900Context is copied raw (1648 bytes on the PC build: r[32] x 16, then pc)."""
    d = open(path, 'rb').read()
    o = d.find(b'ESCH')
    # vblank period u32, then m_readyQueues (empty in every save seen: 0x400 bytes of zero counts here)
    q = o + 0x408
    n, = struct.unpack_from('<Q', d, q); q += 8
    out = []
    for _ in range(n):
        key, tid = struct.unpack_from('<ii', d, q)
        ctx = q + 8
        r = ctx + 1648
        entry, stack, ssz, gp, attr, opt, arg, prio0, prio = struct.unpack_from('<7I2i', d, r); r += 36
        status = d[r]; r += 1 + 8 + 1 + 4  # status, suspendCount, wakeupCount, ownsStack, tlsBase
        reason = d[r]; r += 1 + 16 + 16    # wait.reason, wait.resume, pendingResume
        idx, = struct.unpack_from('<Q', d, r); r += 8
        size = [1, 4, 16, 16, 16][idx]
        wait_id = struct.unpack_from('<i', d, r)[0] if size >= 4 else None
        r += size
        reg = lambda i: struct.unpack_from('<I', d, ctx + 16 * i)[0]
        out.append(dict(id=tid, entry=entry, prio=prio, status=THREAD_STATUS[status] if status < 6 else status,
                        wait=WAIT_REASON[reason] if reason < 5 else reason, wait_id=wait_id,
                        pc=struct.unpack_from('<I', d, ctx + 512)[0], ra=reg(31), sp=reg(29)))
        q = r
    return out


def main():
    if '--threads' in sys.argv:
        for t in threads(sys.argv[1]):
            print(' '.join(f'{k} {v:#x}' if isinstance(v, int) and k in ('entry', 'pc', 'ra', 'sp') else f'{k} {v}' for k, v in t.items()))
        return
    s = State(sys.argv[1])
    if '--frame' in sys.argv:
        import os
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import gsvram
        fbp, fbw, psm, w, h = s.display()
        if '--fbp' in sys.argv:  # another buffer
            fbp = int(sys.argv[sys.argv.index('--fbp') + 1], 0)
        if '--drawn' in sys.argv:
            # the buffer the save's last chain drew: the game double-buffers 0x0 / 0x8c and draws the one not shown
            # (all 40 gsoffcap + gate saves, 1 Nov; docs/gs_off.md step 4c point 3)
            fbp = 0x8C if fbp == 0 else 0
        h = min(h, 448)
        rows = []
        for y in range(h):
            row = bytearray()
            for x in range(w):
                a = gsvram.addr(fbp * 32, fbw, x, y)
                row += s.vram[a:a + 3]
            rows.append(bytes(row))
        out = sys.argv[sys.argv.index('--frame') + 1]
        gsvram.png(out, w, h, rows)
        if '--alpha' in sys.argv:  # the frame's alpha (the glow mask), grey, 0x80 = 1.0 shown as 255
            gsvram.png(out.replace('.png', '_alpha.png'), w, h, [bytes(min(255, 2 * s.vram[gsvram.addr(fbp * 32, fbw, x, y) + 3])
                                                                       for x in range(w) for _ in range(3)) for y in range(h)])
        print(f'tick {s.tick} fbp {fbp:#x} fbw {fbw} psm {psm} {w}x{h}')
    else:
        print(f'tick {s.tick}', {k: hex(v) for k, v in s.gsregs.items()})


if __name__ == '__main__':
    main()
