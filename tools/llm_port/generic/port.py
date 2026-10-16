"""Generic local-model task loop: the model writes C++ run() (task.h) that returns exactly the text
of the task's Python reference canon(), judged on the task's inputs (build.py prepares them).
Results: game/analysis/qwen/<task>/{port.log, attempts/, best.cpp, PASS}. Resumable.
Usage: python3 -I tools/llm_port/generic/port.py <task> [--url http://127.0.0.1:8033] [--rounds 15]
"""
import importlib.util
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'meshdecode'))
from port import ask, extract  # noqa: E402  (meshdecode/port.py: model API, code extraction)


def _load(name, path):
    # Loaded by path: meshdecode/ (on sys.path for ask/extract) has a judge.py of its own, which an
    # `import judge` picked up instead of this folder's (every report came back empty).
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


judge = _load('generic_judge', os.path.join(HERE, 'judge.py'))

TASK = """Write a C++17 program part. Correctness is judged automatically: for each input file, your
run() must return EXACTLY the text that the Python function canon(data) below returns (every
character, every line), on real game data files.

## Fixed API (task.h, do not change; your file must #include "task.h")
```cpp
{header}
```

## Python reference (the behaviour to reproduce exactly)
```python
{python}
```

## Notes
- struct formats are little-endian ('<I' u32, '<H' u16, '<Q' u64, '<f' f32, '<h' s16). A Python
  exception means what the reference does with it (see canon); struct.unpack_from past the end of
  the data raises. Python ints are unbounded: use 64-bit arithmetic, and never read outside
  [0, len) (out-of-bounds reads are caught by AddressSanitizer and count as a failure).
- Format numbers exactly as Python's % formatting does ('%08x', '%016x', '%d', '%x').
- Keep it reasonably fast (inputs are up to 7 MB; the judge allows 10 minutes per file).
{notes}
- Write the complete file in ONE ```cpp code block. No other files, no main().
"""


def main(argv):
    task = argv[0]
    opt = lambda k, v: argv[argv.index(k) + 1] if k in argv else v
    url, rounds = opt('--url', 'http://127.0.0.1:8033'), int(opt('--rounds', '15'))
    spec = importlib.util.spec_from_file_location('t', os.path.join(HERE, 'tasks', task + '.py'))
    t = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(t)
    out = os.path.join(ROOT, 'game', 'analysis', 'qwen', task)
    att = os.path.join(out, 'attempts')
    os.makedirs(att, exist_ok=True)
    if os.path.exists(os.path.join(out, 'PASS')):
        print(task, 'already passed')
        return 0
    log = open(os.path.join(out, 'port.log'), 'a')
    say = lambda s: (print(s, flush=True), log.write(time.strftime('%H:%M:%S ') + s + '\n'), log.flush())
    prompt = TASK.format(header=open(os.path.join(HERE, 'task.h')).read(), python=open(os.path.join(out, 'reference.py')).read(),
                         notes=getattr(t, 'NOTES', ''))
    done = sorted(f for f in os.listdir(att) if f.endswith('.cpp'))
    prev_code = open(os.path.join(att, done[-1])).read() if done else None
    prev_report = open(os.path.join(att, done[-1][:-4] + '.report.txt')).read() if done else None
    best = -1
    for n in range(len(done) + 1, rounds + 1):
        messages = [{'role': 'user', 'content': prompt}]
        if prev_code:
            messages += [{'role': 'assistant', 'content': '```cpp\n' + prev_code + '```'},
                         {'role': 'user', 'content': 'Judge report for that file:\n\n' + prev_report +
                          '\n\nFind the cause by comparing your code with the Python reference, fix it, and reply with '
                          'the complete corrected file in one ```cpp block. Keep your reasoning short.'}]
        try:
            content, _r, usage, secs = ask(url, messages)
        except Exception as e:  # server hiccup: try the round again later
            say('round%02d: request failed (%s); waiting 60 s' % (n, str(e)[:100]))
            time.sleep(60)
            continue
        code = extract(content)
        name = 'round%02d' % n
        if code is None:
            say('%s: no code block (%.0f s, %s tokens)' % (name, secs, usage.get('completion_tokens')))
            continue
        path = os.path.join(att, name + '.cpp')
        open(path, 'w').write(code)
        res = judge.judge(task, path)
        open(os.path.join(att, name + '.report.txt'), 'w').write(res['report'])
        say('%s: %s; %.0f s, %s tokens' % (name, (res['report'].splitlines() or ['(empty report)'])[0][:120], secs, usage.get('completion_tokens')))
        if res.get('score', -1) > best:
            best = res['score']
            open(os.path.join(out, 'best.cpp'), 'w').write(code)
        prev_code, prev_report = code, res['report']
        if res['ok']:
            open(os.path.join(out, 'best.cpp'), 'w').write(code)
            open(os.path.join(out, 'PASS'), 'w').write(name + '\n')
            say('PASS in %s' % name)
            return 0
    say('stopped after %d rounds' % rounds)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
