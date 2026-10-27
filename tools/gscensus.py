"""Census of the GS features a PS2X_GS_TRACE text log uses (docs/renderer_audit.md item 10): per value, how many
draws (D lines) set DTHE, PABE, FBA, COLCLAMP, TEX1 (MXL, MMIN, MMAG, LCM, K: textured draws only) and CLAMP
(WMS / WMT, region modes 2 = REGION_CLAMP, 3 = REGION_REPEAT: textured draws only), and how many alpha-blended
draws have a vertex alpha above 0x80, per ALPHA register value.

Usage: python3 -I tools/gscensus.py TRACE.txt [...]
"""
import collections
import re
import sys

FIELD = re.compile(r'(\w+)=(\S+)')


def census(path):
    t = collections.defaultdict(collections.Counter)
    draws = 0
    for line in open(path, errors='replace'):
        if not line.startswith('D '):
            continue
        draws += 1
        head, *verts = line.split(' | ')
        f = dict(FIELD.findall(head))
        frame = f['frame'].split('/')[0]
        t['dthe'][(f['dthe'], 'fbp ' + frame)] += 1
        t['pabe'][f['pabe']] += 1
        t['fba'][f['fba']] += 1
        t['colclamp'][f['colclamp']] += 1
        if f['tme'] == '1':
            tex1 = int(f['tex1'], 16)
            t['tex1 mxl/mmin/mmag/lcm/k'][((tex1 >> 2) & 7, (tex1 >> 6) & 7, (tex1 >> 5) & 1, tex1 & 1, (tex1 >> 32) & 0xFFF)] += 1
            clamp = int(f['clamp'], 16)
            wms, wmt = clamp & 3, (clamp >> 2) & 3
            key = (wms, wmt)
            if wms >= 2 or wmt >= 2:
                key += (f'minu {(clamp >> 4) & 0x3FF} maxu {(clamp >> 14) & 0x3FF} minv {(clamp >> 24) & 0x3FF} maxv {(clamp >> 34) & 0x3FF}',)
            t['clamp wms/wmt'][key] += 1
        if f['abe'] == '1':
            over = any(int(re.search(r'rgba=(\w{8})', v).group(1)[6:], 16) > 0x80 for v in verts)
            t['blended: vertex alpha > 0x80 (alpha reg, over)'][(f['alpha'], over)] += 1
    print(f'{path}: {draws} draws')
    for name, c in t.items():
        print(f'  {name}:')
        for k, n in c.most_common():
            print(f'    {k}: {n}')


if __name__ == '__main__':
    for p in sys.argv[1:]:
        census(p)
