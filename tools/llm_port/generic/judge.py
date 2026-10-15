"""Generic judge: compiles <candidate.cpp> with driver.cpp (ASan + UBSan, -O1), runs it on every
input of the task and compares with the Python reference's expected text.
Usage: python3 -I tools/llm_port/generic/judge.py <task> <candidate.cpp>
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))


def judge(task, candidate):
    tests = os.path.join(ROOT, 'game', 'analysis', 'qwen', task, 'tests')
    exe = os.path.join(tempfile.mkdtemp(prefix='qjudge'), 'driver')
    r = subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-I', HERE, os.path.join(HERE, 'driver.cpp'), candidate, '-o', exe],
                       capture_output=True, text=True)
    if r.returncode:
        errs = [l for l in r.stderr.splitlines() if 'error' in l][:15]
        return {'ok': False, 'report': 'Your file did not compile. Errors:\n' + ('\n'.join(errs) or r.stderr[-3000:])}
    files = same = 0
    lines_total = lines_same = 0
    diffs = []
    for exp in sorted(glob.glob(os.path.join(tests, '*.expected'))):
        inp = exp[:-len('.expected')]
        try:
            r = subprocess.run([exe, inp], capture_output=True, text=True, timeout=600)
        except subprocess.TimeoutExpired:
            return {'ok': False, 'report': 'Your program ran for over 10 minutes on %s (endless loop?).' % os.path.basename(inp)}
        if r.returncode:
            return {'ok': False, 'report': 'Your program crashed on %s (AddressSanitizer/UBSan):\n%s' % (os.path.basename(inp), r.stderr[-3000:])}
        files += 1
        e, g = open(exp).read().splitlines(), r.stdout.splitlines()
        lines_total += len(e)
        lines_same += sum(1 for a, b in zip(e, g) if a == b)
        if e == g:
            same += 1
            continue
        for i in range(max(len(e), len(g))):
            a = e[i] if i < len(e) else '<missing line>'
            b = g[i] if i < len(g) else '<missing line>'
            if a != b:
                if len(diffs) < 4:
                    diffs.append('-- %s line %d (expected %d lines, got %d)\nexpected: %s\ngot:      %s' % (
                        os.path.basename(inp), i + 1, len(e), len(g), a[:400], b[:400]))
                break
    head = '%d of %d input files identical (%d of %d lines in place)' % (same, files, lines_same, lines_total)
    return {'ok': same == files and files > 0, 'report': head + ('\n' + '\n'.join(diffs) if diffs else ''),
            'score': lines_same}


if __name__ == '__main__':
    res = judge(sys.argv[1], sys.argv[2])
    print(res['report'])
    sys.exit(0 if res['ok'] else 1)
