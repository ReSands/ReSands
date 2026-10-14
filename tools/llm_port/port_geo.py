"""Translate the geometry loader decompile (C) to Python with a local model, chunk by chunk.

Resumable: each chunk's result is written to <out>/chunks/NNN.py as soon as it arrives, and
finished chunks are skipped on the next run. If the model server is unreachable the script
retries for a while and then exits (code 2); just run it again later.

Usage: python3 -I tools/llm_port/port_geo.py <decompile.c> <out dir> [--only N] [--model gemma4:26b]
       [--url http://127.0.0.1:11434]   (default: an SSH tunnel to the PC, see run_port.sh)
"""
import json, os, re, sys, time, urllib.request

BRIEF = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'brief.md')
CHUNK_LINES = 40

def clean(src):
    """Drop declarations and the decompiler's byte-swap blocks (pure noise for us)."""
    lines = src.split('\n')
    out, i = [], 0
    body = False
    while i < len(lines):
        l = lines[i]
        if not body:
            if l.strip() == '{':
                body = True
                out.append(l)
            elif l.strip().startswith(('undefined', 'uint', 'int', 'float', 'void', 'long', 'ulong',
                                       'char', 'short', 'ushort', 'bool', 'byte', 'code', '/*')) and l.endswith(';'):
                pass
            else:
                out.append(l)
            i += 1
            continue
        if re.match(r'\s*if \(_LOA_gb_SwapperActive != 0\) \{\s*$', l):
            depth = 1
            i += 1
            while i < len(lines) and depth:
                depth += lines[i].count('{') - lines[i].count('}')
                i += 1
            continue
        if re.match(r'\s+\w+ = \(undefined1\)\w+;\s*$', l) or re.match(r'\s+\w+\._\d_\d_ = ', l):
            i += 1
            continue
        if re.match(r'\s+(undefined\d?|uint|int|float|long|ulong|char|short|ushort|bool|byte|code) \*?\w+(\s*\[\w+\])?;\s*$', l):
            i += 1
            continue
        out.append(l)
        i += 1
    return [l for l in out if l.strip()]

def chunks(lines):
    """Fixed windows, each tagged with the brace depth at its start."""
    res, depth = [], 0
    for start in range(0, len(lines), CHUNK_LINES):
        part = lines[start:start + CHUNK_LINES]
        res.append((depth, part))
        for l in part:
            depth += l.count('{') - l.count('}')
    return res

def ask(url, model, prompt, timeout=3600):
    body = json.dumps({'model': model, 'prompt': prompt, 'stream': False, 'think': False,
                       'options': {'temperature': 0, 'num_ctx': 8192}}).encode()
    req = urllib.request.Request(url + '/api/generate', body, {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())['response']

def main(argv):
    src, out = argv[0], argv[1]
    only = int(argv[argv.index('--only') + 1]) if '--only' in argv else None
    model = argv[argv.index('--model') + 1] if '--model' in argv else 'gemma4:26b'
    url = argv[argv.index('--url') + 1] if '--url' in argv else 'http://127.0.0.1:11434'
    os.makedirs(os.path.join(out, 'chunks'), exist_ok=True)
    lines = clean(open(src).read())
    open(os.path.join(out, 'cleaned.c'), 'w').write('\n'.join(lines) + '\n')
    parts = chunks(lines)
    brief = open(BRIEF).read()
    print(f'{len(lines)} cleaned lines, {len(parts)} chunks', flush=True)
    for n, (depth, part) in enumerate(parts):
        if only is not None and n != only:
            continue
        path = os.path.join(out, 'chunks', f'{n:03d}.py')
        if os.path.exists(path):
            continue
        prev = ''
        if n > 0 and os.path.exists(os.path.join(out, 'chunks', f'{n-1:03d}.py')):
            prev = open(os.path.join(out, 'chunks', f'{n-1:03d}.py')).read()[-1500:]
        prompt = (f'{brief}\n\nDEPTH = {max(depth - 1, 0)}\n\nEnd of the previous chunk\'s Python (for context only, do not repeat):\n'
                  f'```python\n{prev}\n```\n\nCHUNK (C lines {n * CHUNK_LINES + 1}-{n * CHUNK_LINES + len(part)}):\n```c\n'
                  + '\n'.join(part) + '\n```\n')
        for attempt in range(6):
            try:
                t = time.time()
                text = ask(url, model, prompt)
                break
            except Exception as e:
                print(f'chunk {n}: server unreachable ({e}); retry {attempt + 1}/6 in 60 s', flush=True)
                time.sleep(60)
        else:
            print('giving up for now; run again to resume', flush=True)
            sys.exit(2)
        m = re.search(r'```python\n(.*?)```', text, re.S)
        code = m.group(1) if m else text
        header = f'# chunk {n}, C lines {n * CHUNK_LINES + 1}-{n * CHUNK_LINES + len(part)}, depth {max(depth - 1, 0)}\n'
        tmp = path + '.tmp'
        open(tmp, 'w').write(header + code)
        os.replace(tmp, path)  # atomic: a half-written chunk never looks finished
        print(f'chunk {n} done in {time.time() - t:.0f} s', flush=True)
    print('all chunks done' if only is None else f'chunk {only} done', flush=True)

if __name__ == '__main__':
    main(sys.argv[1:])
