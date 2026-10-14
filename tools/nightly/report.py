"""Turns a night's check results (tools/nightly/checks.py) into a bug report with the local model.

Usage: python3 -I tools/nightly/report.py [--date YYYY-MM-DD] [--url http://127.0.0.1:11434] [--model gemma4:26b]
Writes docs/nightly/<date>.md. The script decides the findings and their evidence; the model only
explains each one (what it probably means, a next step), one small prompt per finding. If the model
is unreachable, the findings are written without explanations.
"""
import datetime
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

PROMPT = """You help maintain a project that recompiles the PlayStation 2 game Prince of Persia: The Sands of Time to native PC code (static recompilation with PS2Recomp, plus our own converters for the game's 3D data). An automatic overnight check found this:

Check: {check}
Finding: {finding}
Evidence: {evidence}
Background: {background}

In plain English, say briefly what this finding probably means and suggest one concrete next step. Do not invent facts beyond the evidence; say "unknown" if unsure.
Reply with JSON only: {{"title": "short title", "what_it_means": "one or two sentences", "next_step": "one sentence"}}"""

BACKGROUND = {
    'mmi': 'The instruction test compares PS2Recomp\'s C++ translation of each MMI (PS2 vector) instruction with a reference implementation on random inputs. Listed instructions give wrong results; only some are used by this game.',
    'boot': 'The game was booted from a save state for 60 seconds. A "missing target" is a code address the recompiled game tried to jump to but has no compiled entry for (usually an unresolved switch table); in the past these froze the game.',
    'meshes': 'Our converter decodes the game\'s 3D world packs. A geometry record whose parse does not end exactly at the record end means our reading of its format is wrong somewhere; elements with no decoded vertices use a packet layout we do not handle yet.',
    'diff': 'Change since the previous night.',
}


def findings(results, changes):
    """The issues, decided by script, each with its evidence."""
    out = []
    mmi = results.get('mmi', {})
    if mmi.get('error'):
        out.append(('mmi', 'instruction test did not run', mmi['error'][:400]))
    elif mmi.get('wrong'):
        out.append(('mmi', f"{len(mmi['wrong'])} of {mmi.get('instructions')} MMI instructions translate wrongly", ' '.join(mmi['wrong'])))
    boot = results.get('boot', {})
    if boot.get('error'):
        out.append(('boot', 'boot test did not run', boot['error'][:400]))
    for t in boot.get('missing_targets', []):
        out.append(('boot', f'missing branch target {t}', f'log: [guest-branch:missing-target] {t}'))
    if boot.get('exception_lines'):
        out.append(('boot', 'exception/fatal lines in the boot log', ' | '.join(boot['exception_lines'][:5])[:400]))
    if boot and boot.get('ok') and boot.get('adpcm_ok') is False:
        out.append(('boot', 'sound decoder replay did not match', str(boot.get('adpcm_replay'))[:300]))
    if boot.get('fps_median') is not None and boot['fps_median'] < 28:
        out.append(('boot', 'frame rate below 28 fps', f"fps_median {boot['fps_median']}"))
    if boot and boot.get('ok') and not boot.get('framecap'):
        out.append(('boot', 'frame capture missing', 'no frame capture file was written'))
    m = results.get('meshes', {})
    if m.get('error'):
        out.append(('meshes', 'mesh check did not run', m['error'][:400]))
    for wp in m.get('worst_packs', [])[:8]:
        out.append(('meshes', f"pack {wp['pack']}: {wp['geometry_bad']} geometry records mis-parsed, {wp['decoded_empty']} elements without vertices",
                    json.dumps(wp['examples'])[:400]))
    for e in m.get('pack_errors', [])[:5]:
        out.append(('meshes', f"pack {e.get('pack')} failed", str(e.get('error'))[:300]))
    for c in changes:
        out.append(('diff', f"{c['check']}.{c['field']} changed", json.dumps(c)[:300]))
    return out


def ask(url, model, prompt):
    body = json.dumps({'model': model, 'prompt': prompt, 'stream': False, 'think': False, 'format': 'json',
                       'options': {'temperature': 0.2, 'repeat_penalty': 1.1, 'num_ctx': 16384, 'num_predict': 2500}}).encode()
    req = urllib.request.Request(url + '/api/generate', body, {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=1800) as resp:
        return json.loads(resp.read())['response']


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
    diffp = os.path.join(d, 'diff.json')
    changes = json.load(open(diffp)).get('changes', []) if os.path.exists(diffp) else []
    results.pop('diff', None)
    items = findings(results, changes)
    lines = [f'# Nightly checks {date}', '', f'{len(items)} findings (decided by script; explanations by {model}).', '']
    for n, (check, finding, evidence) in enumerate(items, 1):
        note = {}
        try:
            note = json.loads(ask(url, model, PROMPT.format(check=check, finding=finding, evidence=evidence, background=BACKGROUND.get(check, ''))))
        except Exception as e:  # noqa: BLE001
            note = {'what_it_means': f'(model unavailable: {type(e).__name__})', 'next_step': ''}
        lines += [f"## {n}. {finding} ({check})", '', f"- Evidence: `{evidence}`",
                  f"- What it probably means: {note.get('what_it_means', '')}", f"- Suggested next step: {note.get('next_step', '')}", '']
    if not items:
        lines.append('No problems found.')
    lines += ['', '---', 'Raw numbers:', '']
    for name, r in results.items():
        lines.append(f'- {name}: ' + ', '.join(f'{k}={v}' for k, v in r.items() if not isinstance(v, (list, dict))))
    open(out_path, 'w').write('\n'.join(lines) + '\n')
    print(f'{len(items)} findings -> {out_path}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
