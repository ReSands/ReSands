"""Overnight job: a local model suggests names and summaries for anonymous functions.

Reads the queue (game/analysis/naming_queue.txt), the per-function decompiles
(game/analysis/naming/decomp/<addr>.c) and writes one JSON answer per function to
game/analysis/naming/results/<addr>.json. Finished functions are skipped, so it resumes after
any interruption; if the model server stays unreachable it exits with code 2. Suggestions are
for review only, never applied automatically. The first 60 queue entries are functions whose
real names we know (hidden in their decompile): score them with --score.

Usage: python3 -I tools/llm_port/name_functions.py [--url http://127.0.0.1:11434] [--model gemma4:26b] [--v2] [--calib]
       python3 -I tools/llm_port/name_functions.py --score

--v2 (second configuration, 14 Oct): thinking on, repeat penalty 1.0 (identifiers repeat in code),
16k context with a check that the whole prompt was read, and per-function context from
tools/ghidra/PoPNamingContext.java (game/analysis/naming/context/<addr>.txt: class, callers,
callees, strings). Results go to results_v2/. --calib = only the calibration functions.
--stop-at HH:MM: finish the current function and stop at that local time (overnight runs must not
outlive their job: a killed job leaves this process running on the GPU).
"""
import json
import re, os, sys, time, urllib.request

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


CONTEXT_INTRO = """
Context from the rest of the program (names of other functions come from the prototype build and are reliable):
"""
V2_CTX = 16384


def ask(url, model, prompt, v2=False):
    if v2:
        # thinking tokens count against num_predict; no 'format' (it constrains the thinking too)
        body = {'model': model, 'prompt': prompt, 'stream': False, 'think': True,
                'options': {'temperature': 0.2, 'repeat_penalty': 1.0, 'num_ctx': V2_CTX, 'num_predict': 8000}}
    else:
        body = {'model': model, 'prompt': prompt, 'stream': False, 'think': False, 'format': 'json',
                'options': {'temperature': 0.2, 'repeat_penalty': 1.15, 'num_ctx': 8192, 'num_predict': 400}}
    req = urllib.request.Request(url + '/api/generate', json.dumps(body).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=2400 if v2 else 600) as resp:
        r = json.loads(resp.read())
    return r['response'], {'prompt_tokens': r.get('prompt_eval_count'), 'output_tokens': r.get('eval_count'),
                           'seconds': round(r.get('total_duration', 0) / 1e9, 1), 'thinking_chars': len(r.get('thinking') or '')}


def score():
    known = dict(l.rstrip('\n').split('\t') for l in open(CALIB))

    def name(sub, addr):
        path = os.path.join(NAMING, sub, f'{addr}.json')
        return json.load(open(path)).get('name', '') if os.path.exists(path) else None
    n1 = sum(name('results', a) is not None for a in known)
    n2 = sum(name('results_v2', a) is not None for a in known)
    print(f'calibration functions answered: first configuration {n1}, v2 {n2} (of {len(known)})')
    for addr, real in known.items():
        print(f'  {addr}  real {real:40s} v1 {name("results", addr) or "-":45s} v2 {name("results_v2", addr) or "-"}')


def main(argv):
    if '--score' in argv:
        return score()
    url = argv[argv.index('--url') + 1] if '--url' in argv else 'http://127.0.0.1:11434'
    model = argv[argv.index('--model') + 1] if '--model' in argv else 'gemma4:26b'
    v2 = '--v2' in argv
    stop_at = None
    if '--stop-at' in argv:
        import datetime
        hh, mm = map(int, argv[argv.index('--stop-at') + 1].split(':'))
        now = datetime.datetime.now()
        stop_at = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
        if stop_at <= now:
            stop_at += datetime.timedelta(days=1)
        stop_at = stop_at.timestamp()
    results = os.path.join(NAMING, 'results_v2' if v2 else 'results')
    os.makedirs(results, exist_ok=True)
    queue = [l.split()[0] for l in open(QUEUE) if l.strip()]
    if '--calib' in argv:
        calib = {l.split()[0] for l in open(CALIB) if l.strip()}
        queue = [q for q in queue if f'{int(q, 16):08x}' in calib]
    done = start = time.time()
    count = 0
    for entry in queue:
        addr = f'{int(entry, 16):08x}'
        out = os.path.join(results, f'{addr}.json')
        src = os.path.join(NAMING, 'decomp', f'{addr}.c')
        if os.path.exists(out) or not os.path.exists(src):
            continue
        if stop_at and time.time() >= stop_at:
            print(f'stop time reached after {count} functions; run again to resume', flush=True)
            return
        code = open(src).read()[:6000 if not v2 else 30000]
        prompt = PROMPT + code + '\n```\n'
        ctx = os.path.join(NAMING, 'context', f'{addr}.txt')
        if v2 and os.path.exists(ctx):
            prompt += CONTEXT_INTRO + open(ctx).read()
        stats = {}
        for attempt in range(5):
            try:
                text, stats = ask(url, model, prompt, v2)
                break
            except Exception as e:
                print(f'{addr}: server unreachable ({e}); retry {attempt + 1}/5 in 60 s', flush=True)
                time.sleep(60)
        else:
            print('model server gone; stopping. Run again to resume.', flush=True)
            sys.exit(2)
        try:
            m = re.search(r'\{[^{}]*"name"[^{}]*\}', text, re.S) if v2 else None
            answer = json.loads(m.group(0) if m else text)
        except ValueError:
            # Truncated answer (repetition loop cut off by num_predict): keep the name if it came through.
            m = re.match(r'\s*\{\s*"name"\s*:\s*"([A-Za-z0-9_:~]+)"', text)
            answer = {'name': m.group(1) if m else '', 'summary': '', 'confidence': 'low', 'raw': text[:500]}
        answer['address'] = addr
        if v2:
            # Ollama silently drops the start of a prompt longer than num_ctx: record what was read
            answer['stats'] = dict(stats, prompt_chars=len(prompt), truncated=bool(stats.get('prompt_tokens') and stats['prompt_tokens'] >= V2_CTX - 8))
        tmp = out + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(answer, f)
        os.replace(tmp, out)  # atomic: an interrupted write never looks finished
        count += 1
        if v2:
            print(f'{addr}: {answer.get("name")} ({stats})', flush=True)
        elif count % 25 == 0:
            rate = (time.time() - start) / count
            print(f'{count} named this run ({rate:.1f} s each); last {addr} -> {answer.get("name")}', flush=True)
    print('queue finished', flush=True)


if __name__ == '__main__':
    main(sys.argv[1:])
