"""Local-model job: port the mesh packet decoder (tools/jade2gltf.py) to C++ against the fixed API in
meshdecode.h, judged by judge.py (identical output to the Python reference on every element of the
test packs). Each round the model gets the task, its previous file and the judge's report, and
returns a complete meshdecode.cpp. Attempts and reports are kept in <outdir>/attempts/; the best
file so far is <outdir>/meshdecode.cpp. Resumable (continues from the last attempt).

Usage: python3 -I tools/llm_port/meshdecode/port.py [--url http://127.0.0.1:8033] [--rounds 30]
                                                    [--tests game/analysis/meshdecode/tests]
                                                    [--out game/analysis/meshdecode]
Needs llama-server (OpenAI-compatible API) reachable at --url, e.g. through
`ssh -N -L 8033:127.0.0.1:8033 10.0.0.202` (tools/windows/llama-serve.ps1 starts it on the PC).
"""
import ast
import json
import os
import re
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
import judge  # noqa: E402

FUNCS = ['element_parts', 'EL', 'decode_packet', 'variant3_batches', 'skinned_batches', 'batch_mesh']


def reference_source():
    src = open(os.path.join(ROOT, 'tools', 'jade2gltf.py')).read()
    tree = ast.parse(src)
    parts = []
    for node in tree.body:
        name = getattr(node, 'name', None) or (node.targets[0].id if isinstance(node, ast.Assign) else None)
        if name in FUNCS:
            parts.append(ast.get_source_segment(src, node))
    return 'import struct\n\n' + '\n\n\n'.join(parts)


TASK = """You are porting a small Python module to C++17. Correctness is judged automatically: your C++
must produce exactly the same results as the Python reference on about 1,800 real inputs
(every bit of every float, every byte, every list length).

## Fixed API (meshdecode.h, do not change it; your file must #include "meshdecode.h")
```cpp
{header}
```

## Python reference (the behaviour to reproduce exactly)
```python
{python}
```

## Notes
- `d` is the whole input file as bytes; struct formats are little-endian ('<I' u32, '<f' f32, '<h'
  s16, '<b' s8). Offsets can come from the data itself, so they may point anywhere: never read
  outside [0, len). A Python slice past the end is just shorter; struct.unpack_from past the end
  raises -> your function returns false. Any other Python exception (e.g. KeyError for a format
  missing from EL) also means: return false.
- The Python functions return lists/dicts; fill the `out` parameters in the same order. For
  decode_packet, a batch is a dict {{format: (addr, raw bytes, count)}} -> Batch (std::map, so the
  printed order is by format, as the judge sorts Python's keys too).
- Python ints are unbounded: watch for overflow in offset arithmetic (use 64-bit for offsets and
  sizes before comparing with len).
- batch_mesh: uv = s / uvscale and colour = min(byte / 128.0, 1.0) are double, as in Python.
  Default colours (1.0, 1.0, 1.0, 1.0) and the `col[m:]` tail must behave exactly like the Python
  list expression.
- Write the complete file in ONE ```cpp code block. No other files, no main().
"""


def ask(url, messages):
    body = json.dumps({'messages': messages, 'temperature': 0.6, 'top_p': 0.95, 'top_k': 20, 'min_p': 0.0,
                       'max_tokens': 32768, 'stream': False}).encode()
    req = urllib.request.Request(url.rstrip('/') + '/v1/chat/completions', data=body,
                                 headers={'Content-Type': 'application/json'})
    t = time.time()
    with urllib.request.urlopen(req, timeout=3600) as r:
        res = json.load(r)
    msg = res['choices'][0]['message']
    return msg.get('content') or '', msg.get('reasoning_content') or '', res.get('usage', {}), time.time() - t


def extract(text):
    blocks = re.findall(r'```(?:cpp|c\+\+)?\s*\n(.*?)```', text, re.S)
    return max(blocks, key=len) if blocks else None


def report_text(res):
    if res['stage'] == 'compile':
        return 'Your file did not compile. Errors:\n' + res['detail']
    if res['stage'] == 'crash':
        return ('Your code crashed (AddressSanitizer/UBSan) on pack %s. Last output line: %s\nReport:\n%s'
                % (res['pack'], res.get('last_output', ''), res['detail']))
    t = '%d of %d elements identical. First differing section per failing element: %s\n' % (
        res['identical'], res['elements'], res['failing_sections'])
    t += ('Output format: "parts", "v3" (variant3_batches; floats as IEEE hex bits), "pkt <offset> <0|1>" '
          '(decode_packet without/with blobs; " B" starts a batch, then "format:addr:count:rawhex"), '
          '"mesh <uvscale>" (batch_mesh: positions | uvs as double bits | colours as double bits | tris), '
          '"skin" (skinned_batches). " X" = the function returned false.\n')
    for d in res['diffs']:
        t += '\n-- %s, element %s, section %s (from the first difference):\nexpected: %s\ngot:      %s\n' % (
            d['pack'], d['element'], d['section'], d['expected'], d['got'])
    return t


def main(argv):
    opt = lambda k, v: argv[argv.index(k) + 1] if k in argv else v
    url, rounds = opt('--url', 'http://127.0.0.1:8033'), int(opt('--rounds', '30'))
    tests = opt('--tests', os.path.join(ROOT, 'game', 'analysis', 'meshdecode', 'tests'))
    out = opt('--out', os.path.join(ROOT, 'game', 'analysis', 'meshdecode'))
    att = os.path.join(out, 'attempts')
    os.makedirs(att, exist_ok=True)
    log = open(os.path.join(out, 'port.log'), 'a')
    say = lambda s: (print(s, flush=True), log.write(time.strftime('%H:%M:%S ') + s + '\n'), log.flush())
    task = TASK.format(header=open(os.path.join(HERE, 'meshdecode.h')).read(), python=reference_source())
    done = sorted(f for f in os.listdir(att) if f.endswith('.cpp'))
    prev_code, prev_report, best = None, None, (-1, None)
    if done:
        prev_code = open(os.path.join(att, done[-1])).read()
        prev_report = open(os.path.join(att, done[-1][:-4] + '.report.txt')).read()
        for f in done:
            r = json.load(open(os.path.join(att, f[:-4] + '.json')))
            best = max(best, (r.get('identical', -1), f))
    for n in range(len(done) + 1, rounds + 1):
        messages = [{'role': 'user', 'content': task}]
        if prev_code:
            messages += [{'role': 'assistant', 'content': '```cpp\n' + prev_code + '```'},
                         {'role': 'user', 'content': 'Judge report for that file:\n\n' + prev_report +
                          '\n\nFind the cause in your code by comparing it with the Python reference, fix it, and '
                          'reply with the complete corrected file in one ```cpp block.'}]
        content, reasoning, usage, secs = ask(url, messages)
        code = extract(content)
        name = 'round%02d' % n
        open(os.path.join(att, name + '.answer.md'), 'w').write(content)
        if code is None:
            say('%s: no code block in the answer (%.0f s, %s)' % (name, secs, usage))
            continue
        path = os.path.join(att, name + '.cpp')
        open(path, 'w').write(code)
        res = judge.judge(path, tests)
        rep = report_text(res)
        json.dump(res, open(os.path.join(att, name + '.json'), 'w'), indent=1)
        open(os.path.join(att, name + '.report.txt'), 'w').write(rep)
        score = res.get('identical', -1)
        say('%s: %s, %s; %.0f s, tokens %s' % (name, res['stage'],
            '%d/%d elements identical' % (score, res['elements']) if res['stage'] == 'compare' else 'failed',
            secs, usage.get('completion_tokens')))
        if score > best[0]:
            best = (score, name + '.cpp')
            open(os.path.join(out, 'meshdecode.cpp'), 'w').write(code)
        prev_code, prev_report = code, rep
        if res['ok']:
            say('PASS in %s' % name)
            return 0
    say('stopped after %d rounds; best %s with %d identical' % (rounds, best[1], best[0]))
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
