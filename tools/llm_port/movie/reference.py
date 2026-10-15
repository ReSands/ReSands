"""Reference for the movie header parser (stage 1): canonical text of every movie's header.
Usage: python3 -I tools/llm_port/movie/reference.py <disc dir> <out dir>
  writes <out>/<NAME>.head (first 1 MB of the movie: all headers fit) and <NAME>.expected.
"""
import glob
import os
import struct
import sys


def parse(d):
    """dict of the header fields, or None when the data is too short / video version is not 3."""
    try:
        fps, fpc, n, t = struct.unpack_from('<fIII', d, 4)
        flags = tuple(d[0x14:0x18])
        if len(flags) < 4:
            return None
        vh, ah, sh, sub = struct.unpack_from('<4I', d, 0x18)
        o = 0x28
        chunks = struct.unpack_from('<%dI' % (n + 1), d, o)
        o += (n + 1) * 4
        h = {'fps': fps, 'fpc': fpc, 'n': n, 't': t, 'flags': flags, 'sizes': (vh, ah, sh, sub), 'chunks': chunks}
        vo = o
        ver = struct.unpack_from('<I', d, vo)[0]
        info = struct.unpack_from('<32I', d, vo + 4)
        if flags[0] and ver != 3:
            return None
        nidx = info[0x70 // 4]
        index = struct.unpack_from('<%dI' % nidx, d, vo + 4 + 0x80) if info[0x6C // 4] else ()
        h.update(ver=ver, info=info, index=index)
        o += vh + ah
        tracks = []
        for _ in range(t):
            fields = struct.unpack_from('<5I', d, o)
            subs = struct.unpack_from('<%dI' % (n + 1), d, o + 0x14)
            tracks.append((fields, subs))
            o += sh
        h['tracks'] = tracks
        h['data'] = o + sub
        return h
    except struct.error:
        return None


def text(h):
    if h is None:
        return 'X\n'
    out = ['fps %08x' % struct.unpack('<I', struct.pack('<f', h['fps']))[0],
           'chunking %u %u %u' % (h['fpc'], h['n'], h['t']), 'flags %u %u %u %u' % h['flags'],
           'sizes %u %u %u %u' % h['sizes'], 'chunks ' + ' '.join('%x' % c for c in h['chunks']),
           'video %u mb %u %u frames %u' % (h['ver'], h['info'][0x60 // 4], h['info'][0x64 // 4], h['info'][0x70 // 4]),
           'info ' + ' '.join('%x' % x for x in h['info']), 'index ' + ' '.join('%x' % x for x in h['index'])]
    for f, s in h['tracks']:
        out.append('track ' + ' '.join('%x' % x for x in f) + ' | ' + ' '.join('%x' % x for x in s))
    out.append('data %x' % h['data'])
    return '\n'.join(out) + '\n'


def main(argv):
    os.makedirs(argv[1], exist_ok=True)
    for path in sorted(glob.glob(os.path.join(argv[0], '*.INT'))):
        name = os.path.basename(path)[:-4]
        head = open(path, 'rb').read(1 << 20)
        open(os.path.join(argv[1], name + '.head'), 'wb').write(head)
        for cut, suffix in ((len(head), ''), (0x60, '_short')):  # also a truncated case: must give X
            open(os.path.join(argv[1], name + suffix + '.expected'), 'w').write(text(parse(head[:cut])))
            if suffix:
                open(os.path.join(argv[1], name + suffix + '.head'), 'wb').write(head[:cut])
        print(name)


if __name__ == '__main__':
    main(sys.argv[1:])
