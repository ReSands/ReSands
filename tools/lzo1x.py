"""Reference LZO1X decompressor (safe, pure Python) for checking the game's recompiled
decompressor against its dumps in game/recomp/dumps/.

Usage: python3 -I tools/lzo1x.py game/recomp/dumps/lzo0   (compares lzo0.in with lzo0.out)
"""
import sys


def decompress(src: bytes) -> bytes:
    out = bytearray()
    ip = 0

    def length(base, mask_value):
        nonlocal ip
        t = mask_value
        if t == 0:
            while src[ip] == 0:
                t += 255
                ip += 1
            t += base + src[ip]
            ip += 1
        return t

    def copy_match(distance, count):
        start = len(out) - distance
        if start < 0:
            raise ValueError(f"match distance {distance} before start of output at {len(out)}")
        for i in range(count):
            out.append(out[start + i])

    state = 0  # literals copied by the previous instruction (0, 1-3, or 4+)
    t = src[ip]
    if t > 17:
        ip += 1
        t -= 17
        out += src[ip:ip + t]
        ip += t
        state = 4 if t >= 4 else t
    while True:
        t = src[ip]
        ip += 1
        if t >= 64:  # M2: 3-8 byte match, distance up to 2 KB
            count = (t >> 5) - 1 + 2
            distance = ((t >> 2) & 7) + (src[ip] << 3) + 1
            ip += 1
            copy_match(distance, count)
        elif t >= 32:  # M3: distance up to 16 KB
            count = length(31, t & 31) + 2
            distance = ((src[ip] >> 2) + (src[ip + 1] << 6)) + 1
            ip += 2
            copy_match(distance, count)
        elif t >= 16:  # M4: distance 16-48 KB, or end of stream
            count = length(7, t & 7) + 2
            distance = ((t & 8) << 11) + (src[ip] >> 2) + (src[ip + 1] << 6)
            ip += 2
            if distance == 0:
                return bytes(out)  # end marker
            copy_match(distance + 0x4000, count)
        else:  # t < 16
            if state == 0:  # literal run
                count = length(15, t) + 3
                out += src[ip:ip + count]
                ip += count
                state = 4
                continue
            if state == 4:  # M1 after a literal run of 4+: 3-byte match, distance 2049-3072
                distance = (t >> 2) + (src[ip] << 2) + 0x801
                ip += 1
                copy_match(distance, 3)
            else:  # M1 after a short literal run: 2-byte match, distance 1-1024
                distance = (t >> 2) + (src[ip] << 2) + 1
                ip += 1
                copy_match(distance, 2)
        # trailing literals encoded in the low 2 bits of the last distance byte
        trailing = src[ip - 2] & 3
        out += src[ip:ip + trailing]
        ip += trailing
        state = trailing


def main():
    base = sys.argv[1]
    data_in = open(base + ".in", "rb").read()
    data_out = open(base + ".out", "rb").read()
    ref = decompress(data_in)
    print(f"input {len(data_in)} bytes, game output {len(data_out)} bytes, reference {len(ref)} bytes")
    if ref == data_out:
        print("MATCH: the recompiled decompressor's output is identical to the reference")
        return
    first = next((i for i in range(min(len(ref), len(data_out))) if ref[i] != data_out[i]), min(len(ref), len(data_out)))
    print(f"DIFFER at byte {first:#x}")
    print("  game:", data_out[first:first + 16].hex(" "))
    print("  ref: ", ref[first:first + 16].hex(" "))


if __name__ == "__main__":
    main()
