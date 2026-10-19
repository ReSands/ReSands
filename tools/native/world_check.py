"""POP_NATIVE_WORLD check of one save: the game's final Z buffer (ZBUF 0x118, PSMZ24, 640 wide) against the
native world's GS Z (pop_native_view --gsz of pop_state_frame --world) and the whole native frame's.

Usage: python3 -I tools/native/world_check.py STATE WORLD.gsz FULL.gsz
Only x >= 448 is used: the glow/haze work buffers share memory with the Z buffer (FBP 280) and overwrite its
left part after the 3D. Prints the share of those pixels whose final surface is the native world's, another
native draw's, or neither; 'ahead' = native world surface in front of the game's final one, 'full ahead' = the
same for the whole native frame, 'extra' = world ahead where the whole frame is not. 'extra' must be ~0 (the
injected depth would hide something the GS draws). Where the saved Z does not belong to the camera in RAM
(a moving camera, or the haze/glow passes drawing into the Z buffer's memory) 'neither' is high and 'ahead'
and 'full ahead' are both large: that save says nothing.
"""
import array
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from popstate import State  # noqa: E402
import gsvram  # noqa: E402


def zaddr(zbp, x, y):
    page = (zbp >> 5) + (y >> 5) * 10 + (x >> 6)
    bid = ((zbp & 0x1F) + gsvram.BLOCK[(y >> 3) & 3][(x >> 3) & 7]) ^ 24  # Z32/Z24 blocks: CT32's ^ 24
    return ((page << 13) + (bid & 0x1F) * 256 + gsvram.COLUMN[y & 7][x & 7] * 4) % (4 << 20)


def main(argv):
    s = State(argv[0])
    world, full = array.array('I'), array.array('I')
    world.frombytes(open(argv[1], 'rb').read())
    full.frombytes(open(argv[2], 'rb').read())
    zbp = 0x118 * 32
    n = mine = other = ahead = fullAhead = extra = 0
    for y in range(448):
        for x in range(448, 640):
            a = struct.unpack_from('<I', s.vram, zaddr(zbp, x, y))[0] & 0xFFFFFF
            if not a:
                continue
            n += 1
            b, f = world[y * 640 + x], full[y * 640 + x]
            if b and abs(a / b - 1) < 2e-3:
                mine += 1
            elif f and abs(a / f - 1) < 2e-3:
                other += 1
            if b and b > a * 1.002:
                ahead += 1
                if not (f and f > a * 1.002):
                    extra += 1
            if f and f > a * 1.002:
                fullAhead += 1
    if not n:
        print('no Z')
        return
    print(f'world {100 * mine / n:5.1f}% other {100 * other / n:5.1f}% neither {100 * (n - mine - other) / n:5.1f}% ahead {100 * ahead / n:5.2f}% full ahead {100 * fullAhead / n:5.2f}% extra {100 * extra / n:5.2f}%')


if __name__ == '__main__':
    main(sys.argv[1:])
