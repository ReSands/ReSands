"""Local-model job: the PS2 movie header parser (stage 1 of the native movie decoder, docs/movies.md),
same loop as tools/llm_port/meshdecode/port.py: the model writes movie.cpp against movie.h, judged by
judge.py against reference.py on every disc movie. Results: game/analysis/movie/stage1/.
Usage: python3 -I tools/llm_port/movie/port.py [--url http://127.0.0.1:8033] [--rounds 15]
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'meshdecode'))
from port import ask, extract  # noqa: E402  (meshdecode/port.py: the model API and code extraction)
import importlib.util  # noqa: E402

# Loaded by path: meshdecode/ (on sys.path for ask/extract) has a judge.py of its own, which an
# `import judge` picked up instead of this folder's (every attempt "failed to compile": no movie.h).
_spec = importlib.util.spec_from_file_location('movie_judge', os.path.join(HERE, 'judge.py'))
judge = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(judge)

TASK = """You are writing a small C++17 parser. Correctness is judged automatically: your output must
be identical to the Python reference on every movie file (every field, every list).

## Fixed API (movie.h, do not change it; your file must #include "movie.h")
```cpp
{header}
```

## Python reference (the behaviour to reproduce exactly; `parse` returns None -> return false)
```python
{python}
```

## Notes
- struct formats are little-endian ('<I' u32, '<f' f32). struct.unpack_from past the end of the
  data raises -> return false. Never read outside [0, len); do 64-bit arithmetic on offsets and
  counts before comparing with len (counts come from the file and can be huge in damaged data:
  check sizes before allocating).
- Fill every field of MovieHeader that the reference fills (widthMb/heightMb/frameCount from the
  movie info words 0x60/4, 0x64/4, 0x70/4; frameIndex only when movieInfo[0x6C/4] != 0).
- Write the complete file in ONE ```cpp code block. No other files, no main().
"""


def main(argv):
    opt = lambda k, v: argv[argv.index(k) + 1] if k in argv else v
    url, rounds = opt('--url', 'http://127.0.0.1:8033'), int(opt('--rounds', '15'))
    tests = os.path.join(ROOT, 'game', 'analysis', 'movie', 'tests')
    out = os.path.join(ROOT, 'game', 'analysis', 'movie', 'stage1')
    att = os.path.join(out, 'attempts')
    os.makedirs(att, exist_ok=True)
    log = open(os.path.join(out, 'port.log'), 'a')
    say = lambda s: (print(s, flush=True), log.write(time.strftime('%H:%M:%S ') + s + '\n'), log.flush())
    ref = open(os.path.join(HERE, 'reference.py')).read()
    ref = ref[ref.index('def parse'):ref.index('def text')]
    task = TASK.format(header=open(os.path.join(HERE, 'movie.h')).read(), python='import struct\n\n' + ref)
    done = sorted(f for f in os.listdir(att) if f.endswith('.cpp'))
    prev_code = open(os.path.join(att, done[-1])).read() if done else None
    prev_report = open(os.path.join(att, done[-1][:-4] + '.report.txt')).read() if done else None
    for n in range(len(done) + 1, rounds + 1):
        messages = [{'role': 'user', 'content': task}]
        if prev_code:
            messages += [{'role': 'assistant', 'content': '```cpp\n' + prev_code + '```'},
                         {'role': 'user', 'content': 'Judge report for that file:\n\n' + prev_report +
                          '\n\nFix it and reply with the complete corrected file in one ```cpp block.'}]
        content, _reasoning, usage, secs = ask(url, messages)
        code = extract(content)
        name = 'round%02d' % n
        if code is None:
            say('%s: no code block (%.0f s)' % (name, secs))
            continue
        path = os.path.join(att, name + '.cpp')
        open(path, 'w').write(code)
        res = judge.judge(path, tests)
        rep = res.get('detail') or '%d of %d files identical\n%s' % (res['identical'], res['files'], '\n'.join(res['diffs']))
        if res['stage'] != 'compare':
            rep = res['stage'] + ' failed:\n' + rep
        open(os.path.join(att, name + '.report.txt'), 'w').write(rep)
        say('%s: %s; %.0f s, tokens %s' % (name, rep.splitlines()[0], secs, usage.get('completion_tokens')))
        prev_code, prev_report = code, rep
        if res['ok']:
            open(os.path.join(out, 'movie.cpp'), 'w').write(code)
            say('PASS in %s' % name)
            return 0
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
