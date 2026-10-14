"""Turns a night's check results (tools/nightly/checks.py) into a bug report with the local model.

Usage: python3 -I tools/nightly/report.py [--date YYYY-MM-DD] [--url http://127.0.0.1:11434] [--model gemma4:26b]
Writes docs/nightly/<date>.md. The model only writes; every issue it reports must quote evidence
copied from the results (a value or line that appears in them verbatim), and issues whose evidence
cannot be found in the results are dropped and listed as rejected. If the model is unreachable, a
plain summary is written instead.
"""
import datetime
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

PROMPT = """You are writing the morning bug report for a project that recompiles the PlayStation 2 game Prince of Persia: The Sands of Time to native code. Automatic checks ran overnight; their JSON results are below. The checks are:
- mmi: PS2Recomp's translation of MMI instructions vs a reference ('wrong' lists instructions that differ; 'evidence' shows the failing inputs).
- meshes: every world pack decoded by our converter ('geometry_bad': geometry records whose parse did not end exactly at the record end; 'decoded_empty': mesh elements whose packets gave no vertices; 'worst_packs' gives examples).
- boot: the game booted from a save state for 60 s ('missing_targets': code addresses the recompiled game could not jump to; 'exception_lines'; 'fps_median'; 'framecap': draws recorded in one frame; 'adpcm_ok': whether the sound decoder matched its reference).
- diff: what changed since the previous run.

Write a list of issues, most important first. Only report problems the results actually show; do not guess causes beyond what the data says, and say "unknown" when unsure. Group related failures into one issue.

Reply with JSON only:
{{"summary": "two sentences on the overall state",
  "issues": [{{"title": "...", "check": "mmi|meshes|boot|diff", "severity": "high|medium|low",
               "evidence": "a value or line copied EXACTLY from the results below",
               "what_it_means": "one or two sentences", "next_step": "one concrete step"}}]}}

Results:
{results}
"""


def ask(url, model, prompt):
    body = json.dumps({'model': model, 'prompt': prompt, 'stream': False, 'think': False, 'format': 'json',
                       'options': {'temperature': 0.2, 'repeat_penalty': 1.1, 'num_ctx': 16384, 'num_predict': 2500}}).encode()
    req = urllib.request.Request(url + '/api/generate', body, {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=1800) as resp:
        return json.loads(resp.read())['response']


def compact(results):
    """Results small enough for the model's context: drop bulky per-pack lists."""
    out = {}
    for name, r in results.items():
        out[name] = {k: v for k, v in r.items() if k not in ('evidence',)} if name != 'mmi' else r
    return json.dumps(out, indent=1)[:40000]


def plain(results, date):
    lines = [f'# Nightly checks {date}', '', '(model unavailable; plain summary)', '']
    for name, r in results.items():
        lines.append(f'## {name}')
        for k, v in r.items():
            if not isinstance(v, (list, dict)):
                lines.append(f'- {k}: {v}')
        lines.append('')
    return '\n'.join(lines)


def main(argv):
    date = argv[argv.index('--date') + 1] if '--date' in argv else datetime.date.today().isoformat()
    url = argv[argv.index('--url') + 1] if '--url' in argv else 'http://127.0.0.1:11434'
    model = argv[argv.index('--model') + 1] if '--model' in argv else 'gemma4:26b'
    d = os.path.join(ROOT, 'game', 'analysis', 'nightly', date)
    results = {f[:-5]: json.load(open(os.path.join(d, f))) for f in sorted(os.listdir(d)) if f.endswith('.json') and f != 'meshes_per_pack.json'}
    os.makedirs(os.path.join(ROOT, 'docs', 'nightly'), exist_ok=True)
    out_path = os.path.join(ROOT, 'docs', 'nightly', date + '.md')
    blob = json.dumps(results)
    try:
        answer = json.loads(ask(url, model, PROMPT.format(results=compact(results))))
    except Exception as e:  # noqa: BLE001
        open(out_path, 'w').write(plain(results, date) + f'\n(model error: {e})\n')
        print('model unavailable; plain summary written to', out_path)
        return 0
    kept, rejected = [], []
    for issue in answer.get('issues', []):
        ev = str(issue.get('evidence', '')).strip().strip('"')
        # evidence must appear verbatim in the results (allowing JSON escaping)
        if ev and (ev in blob or json.dumps(ev)[1:-1] in blob):
            kept.append(issue)
        else:
            rejected.append(issue)
    lines = [f'# Nightly checks {date}', '', answer.get('summary', ''), '']
    for n, i in enumerate(kept, 1):
        lines += [f"## {n}. {i.get('title', '')} ({i.get('check', '')}, {i.get('severity', '')})", '',
                  f"- Evidence: `{str(i.get('evidence', ''))[:300]}`", f"- What it means: {i.get('what_it_means', '')}",
                  f"- Next step: {i.get('next_step', '')}", '']
    if rejected:
        lines += ['## Rejected (evidence not found in the results)', '']
        lines += [f"- {i.get('title', '')}: `{str(i.get('evidence', ''))[:120]}`" for i in rejected]
    lines += ['', '---', 'Raw numbers:', '']
    for name, r in results.items():
        lines.append(f'- {name}: ' + ', '.join(f'{k}={v}' for k, v in r.items() if not isinstance(v, (list, dict))))
    open(out_path, 'w').write('\n'.join(lines) + '\n')
    print(f'{len(kept)} issues kept, {len(rejected)} rejected -> {out_path}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
