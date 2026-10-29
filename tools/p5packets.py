"""GFX particle packets (VU1 program P5) in a save's last VIF1 chain: the SetParticleEnv block (FLUSHA, UNPACK V4-32
0x17 qw at 0) and the CGraphicParticle batches after it (BASE / OFFSET / MSCALF, UNPACKs at TOPS, MSCNT), as
FUN_004584C0 writes them (docs/native_renderer.md "P4 and P5 in the game"). One line per block; --dump also prints
the words. The reference for a C++ packet builder: its output must equal these words.
Usage: python3 -I tools/p5packets.py STATE [--dump]
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from popstate import State  # noqa: E402
from statechain import chain_words  # noqa: E402
from vifwalk import UNPACK_BYTES  # noqa: E402


def vif_codes(s):
    """Yields (code, data words) for each VIF code of the frame's last chain."""
    g = 0x70000620
    start = s.u32(g + 0xE4 + 4 * (s.u32(g + 0xF4) ^ 1))
    it = (w for _, w in chain_words(s, start))
    for w in it:
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        n = 0
        if cmd >= 0x60:
            n = (UNPACK_BYTES.get(cmd & 0xF, 4) * (num or 256) + 3) // 4
        elif cmd in (0x20,):
            n = 1
        elif cmd in (0x30, 0x31):
            n = 4
        elif cmd == 0x4A:
            n = 2 * (num or 256)
        elif cmd in (0x50, 0x51):
            n = 4 * (imm or 65536)
        yield w, [next(it) for _ in range(n)]


def main(argv):
    s = State(argv[0])
    dump = '--dump' in argv
    env = batches = 0
    pending = []   # VIF codes since the last FLUSHA
    for w, data in vif_codes(s):
        cmd = (w >> 24) & 0x7F
        if cmd == 0x13:
            pending = []
            continue
        pending.append((w, data))
        if w == 0x6C170000:            # SetParticleEnv: V4-32, 0x17 qw at VU 0
            env += 1
            gif = data[16 * 4:16 * 4 + 4]
            print('env %d: particles %d, GIF tag %08x %08x %08x %08x' % (env, gif[0] & 0x7FFF, *gif))
        elif cmd == 0x17 and any(c & 0xFF00FFFF in (0x6E00C0C8, 0x6E00C096) for c, _ in pending):   # MSCNT after the colours V4-8 at TOPS+0xC8 (billboard) or +0x96 (oriented)
            batches += 1
            parts = ['%s %08x' % ({0x03: 'BASE', 0x02: 'OFFSET', 0x15: 'MSCALF'}.get((c >> 24) & 0x7F, 'UNPACK'), c)
                     for c, _ in pending if (c >> 24) & 0x7F in (0x02, 0x03, 0x15) or (c >> 24) & 0x7F >= 0x60]
            print('  batch %d: %s' % (batches, ', '.join(parts)))
            if dump:
                for c, d in pending:
                    print('    %08x %s' % (c, ' '.join('%08x' % x for x in d[:16]) + (' ...' if len(d) > 16 else '')))
    print('%d env blocks, %d batches' % (env, batches))


if __name__ == '__main__':
    main(sys.argv[1:])
