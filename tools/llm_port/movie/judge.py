"""Judge for the movie header parser: compiles <candidate.cpp> with driver.cpp (ASan + UBSan) and
compares its output with reference.py's for every movie (and a truncated copy of each).
Usage: python3 -I tools/llm_port/movie/judge.py <candidate.cpp> <tests dir>
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def judge(candidate, tests):
    exe = os.path.join(tempfile.mkdtemp(prefix='moviejudge'), 'driver')
    r = subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-I', HERE, os.path.join(HERE, 'driver.cpp'), candidate, '-o', exe],
                       capture_output=True, text=True)
    if r.returncode:
        return {'ok': False, 'stage': 'compile', 'detail': '\n'.join([l for l in r.stderr.splitlines() if 'error' in l][:15])}
    files, same, diffs = 0, 0, []
    for exp in sorted(glob.glob(os.path.join(tests, '*.expected'))):
        base = exp[:-len('.expected')]
        r = subprocess.run([exe, base + '.head'], capture_output=True, text=True, timeout=120)
        if r.returncode:
            return {'ok': False, 'stage': 'crash', 'pack': os.path.basename(base), 'detail': r.stderr[-3000:]}
        files += 1
        e, g = open(exp).read().splitlines(), r.stdout.splitlines()
        if e == g:
            same += 1
            continue
        for i in range(max(len(e), len(g))):
            el, gl = (e[i] if i < len(e) else '<missing>'), (g[i] if i < len(g) else '<missing>')
            if el != gl:
                diffs.append('%s line %d\n expected: %s\n got:      %s' % (os.path.basename(base), i + 1, el[:300], gl[:300]))
                break
    return {'ok': same == files, 'stage': 'compare', 'files': files, 'identical': same, 'diffs': diffs[:5]}


if __name__ == '__main__':
    res = judge(sys.argv[1], sys.argv[2])
    print(res if res['stage'] != 'compare' else '%d of %d files identical\n%s' % (res['identical'], res['files'], '\n'.join(res['diffs'])))
    sys.exit(0 if res['ok'] else 1)
