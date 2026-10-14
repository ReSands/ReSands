"""Overnight job: a local model suggests names and summaries for anonymous functions.

Reads the queue (game/analysis/naming_queue.txt), the per-function decompiles
(game/analysis/naming/decomp/<addr>.c) and writes one JSON answer per function to
game/analysis/naming/results/<addr>.json. Finished functions are skipped, so it resumes after
any interruption; if the model server stays unreachable it exits with code 2. Suggestions are
for review only, never applied automatically. The first 60 queue entries are functions whose
real names we know (hidden in their decompile): score them with --score.

Usage: python3 -I tools/llm_port/name_functions.py [--url http://127.0.0.1:11434] [--model gemma4:26b]
       python3 -I tools/llm_port/name_functions.py --score
"""
import json, os, sys, time, urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
NAMING = os.path.join(ROOT, 'game', 'analysis', 'naming')
QUEUE = os.path.join(ROOT, 'game', 'analysis', 'naming_queue.txt')
CALIB = os.path.join(ROOT, 'game', 'analysis', 'naming_calibration.tsv')

PROMPT = """You are helping reverse-engineer Prince of Persia: The Sands of Time (PlayStation 2, 2003), built on Ubisoft's Jade engine.
Below is Ghidra's decompilation of one function. Names of OTHER functions and globals in it come from a prototype build and are reliable; FUN_xxxxxxxx, DAT_xxxxxxxx and similar are unknown.
Suggest a name for this function in the engine's own style, e.g. SND_fn_vStopSound, GEO_fn_lGetNbPoints, LOA_ReadVector, or Class::Method for C++ code (fn_v = returns nothing, fn_l = returns a long, fn_b = returns a bool, fn_p = returns a pointer). Base it only on what the code does.
Reply with JSON only: {"name": "...", "summary": "one sentence on what it does", "confidence": "low" | "medium" | "high"}

```c
"""


def ask(url, model, prompt):
    body = json.dumps({'model': model, 'prompt': prompt, 'stream': False, 'think': False, 'format': 'json',
                       'options': {'temperature': 0, 'num_ctx': 8192}}).encode()
    req = urllib.request.Request(url + '/api/generate', body, {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=600) as resp:
        return json.loads(resp.read())['response']


def score():
    known = dict(l.rstrip('\n').split('\t') for l in open(CALIB))
    rows = []
    for addr, real in known.items():
        path = os.path.join(NAMING, 'results', f'{addr}.json')
        if os.path.exists(path):
            rows.append((addr, real, json.load(open(path)).get('name', '')))
    print(f'{len(rows)} of {len(known)} calibration functions answered')
    for addr, real, guess in rows:
        print(f'  {addr}  real {real:45s} guess {guess}')


def main(argv):
    if '--score' in argv:
        return score()
    url = argv[argv.index('--url') + 1] if '--url' in argv else 'http://127.0.0.1:11434'
    model = argv[argv.index('--model') + 1] if '--model' in argv else 'gemma4:26b'
    os.makedirs(os.path.join(NAMING, 'results'), exist_ok=True)
    queue = [l.split()[0] for l in open(QUEUE) if l.strip()]
    done = start = time.time()
    count = 0
    for entry in queue:
        addr = f'{int(entry, 16):08x}'
        out = os.path.join(NAMING, 'results', f'{addr}.json')
        src = os.path.join(NAMING, 'decomp', f'{addr}.c')
        if os.path.exists(out) or not os.path.exists(src):
            continue
        code = open(src).read()[:6000]
        for attempt in range(5):
            try:
                text = ask(url, model, PROMPT + code + '\n```\n')
                break
            except Exception as e:
                print(f'{addr}: server unreachable ({e}); retry {attempt + 1}/5 in 60 s', flush=True)
                time.sleep(60)
        else:
            print('model server gone; stopping. Run again to resume.', flush=True)
            sys.exit(2)
        try:
            answer = json.loads(text)
        except ValueError:
            answer = {'name': '', 'summary': '', 'confidence': 'low', 'raw': text[:500]}
        answer['address'] = addr
        tmp = out + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(answer, f)
        os.replace(tmp, out)  # atomic: an interrupted write never looks finished
        count += 1
        if count % 25 == 0:
            rate = (time.time() - start) / count
            print(f'{count} named this run ({rate:.1f} s each); last {addr} -> {answer.get("name")}', flush=True)
    print('queue finished', flush=True)


if __name__ == '__main__':
    main(sys.argv[1:])
