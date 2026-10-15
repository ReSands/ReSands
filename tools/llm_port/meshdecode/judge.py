"""Judge for the mesh decoder port: compiles <candidate.cpp> with driver.cpp (AddressSanitizer +
UBSan, so out-of-bounds reads fail loudly), runs it on every test pack and compares its output line
by line with the Python reference (reference.py).

Usage: python3 -I tools/llm_port/meshdecode/judge.py <candidate.cpp> <tests dir> [--json]
Exit 0 = identical on every pack. Prints a summary and the first differences (truncated).
"""
import glob
import json
import os
import subprocess
import sys
import tempfile
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
CXX = os.environ.get('CXX', 'clang++')


def trunc(s, n=300):
    return s if len(s) <= n else s[:n] + ' ...(%d more chars)' % (len(s) - n)


def first_diff(a, b):
    i = 0
    while i < min(len(a), len(b)) and a[i] == b[i]:
        i += 1
    return i


def blocks(lines):
    """Lines grouped per element ('J <offset>' starts one), keyed by pack order + offset."""
    out, key = {}, None
    for l in lines:
        if l.startswith('J '):
            key = '%d:%s' % (len(out), l)
            out[key] = []
        elif key is not None:
            out[key].append(l)
    return out


def judge(candidate, tests):
    work = tempfile.mkdtemp(prefix='meshjudge')
    exe = os.path.join(work, 'driver')
    cmd = [CXX, '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           '-I', HERE, os.path.join(HERE, 'driver.cpp'), candidate, '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        errs = [l for l in r.stderr.splitlines() if 'error' in l][:15]
        return {'ok': False, 'stage': 'compile', 'detail': '\n'.join(errs) or r.stderr[-3000:]}
    total = same = 0
    diffs = []
    sections = Counter()
    for exp_path in sorted(glob.glob(os.path.join(tests, '*.expected'))):
        base = exp_path[:-len('.expected')]
        r = subprocess.run([exe, base + '.bin', base + '.jobs'], capture_output=True, text=True, timeout=600)
        got = r.stdout.splitlines()
        exp = open(exp_path).read().splitlines()
        if r.returncode != 0:
            return {'ok': False, 'stage': 'crash', 'pack': os.path.basename(base),
                    'detail': trunc(r.stderr[-4000:], 4000), 'last_output': trunc(got[-1] if got else '', 300)}
        eb, gb = blocks(exp), blocks(got)
        for key in eb:
            total += 1
            e_lines, g_lines = eb[key], gb.get(key, ['<element missing from output>'])
            if e_lines == g_lines:
                same += 1
                continue
            sections[next((l.split(' ')[0] for l, m in zip(e_lines, g_lines + [''] * len(e_lines)) if l != m), '?')] += 1
            if len(diffs) < 5:
                for i in range(max(len(e_lines), len(g_lines))):
                    e = e_lines[i] if i < len(e_lines) else '<missing line>'
                    g = g_lines[i] if i < len(g_lines) else '<missing line>'
                    if e != g:
                        k = first_diff(e, g)
                        diffs.append({'pack': os.path.basename(base), 'element': key, 'section': e.split(' ')[0],
                                      'expected': trunc(e[max(0, k - 80):]), 'got': trunc(g[max(0, k - 80):]),
                                      'first_difference_at_char': k})
                        break
    return {'ok': same == total, 'stage': 'compare', 'elements': total, 'identical': same,
            'failing_sections': dict(sections), 'diffs': diffs}


def main(argv):
    res = judge(argv[0], argv[1])
    if '--json' in argv:
        print(json.dumps(res, indent=1))
    else:
        if res['stage'] != 'compare':
            print(res['stage'].upper(), 'FAILED\n' + res['detail'])
        else:
            print('%d of %d elements identical; first failing section per element: %s' % (
                res['identical'], res['elements'], res['failing_sections']))
            for d in res['diffs']:
                print('-- %s element %s, section %s\n   expected: %s\n   got:      %s' % (
                    d['pack'], d['element'], d['section'], d['expected'], d['got']))
    return 0 if res['ok'] else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
