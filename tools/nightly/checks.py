"""Nightly check suite: runs our automatic checks and writes one JSON result per check to
game/analysis/nightly/<date>/, plus diff.json against the previous run. Scripts decide what is
wrong; tools/nightly/report.py only turns the results into readable bug reports.

Usage: python3 -I tools/nightly/checks.py [--only mmi,boot,meshes] [--packs N] [--hours H] [--date YYYY-MM-DD]
Order: mmi, boot (minutes), then meshes (saved per pack and resumed on later runs; --hours stops it).
Runs on the PC (Windows: from a VS developer environment, as tools/windows/llm-job.ps1 -Job nightly
sets up) or the Mac (instruction and mesh checks only).

Checks
  mmi     tools/mmitest: PS2Recomp's MMI translations vs the reference (instructions that differ).
  meshes  every world pack (key 0xFF......) of PRINCE.BF: geometry records parsed by the converted
          loader (must end exactly at the record end), mesh elements, elements that decode to
          vertices, errors. Packs run in parallel.
  boot    Windows only: boots the play build from states/gameplay1.state for 60 s with the frame
          capture and the ADPCM decoder capture; checks the log (missing branch targets,
          exceptions, frame rate), the capture and the decoder replay.
"""
import concurrent.futures
import datetime
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
WIN = os.name == 'nt'
DISC = os.path.join(ROOT, 'game', 'ps2', 'disc')
BIGFILE = os.path.join(DISC, 'DATA', 'PRINCE.BF')
ELF = os.path.join(DISC, 'SLUS_207.43')
RECOMP = r'C:\tools\PS2Recomp-sotc' if WIN else os.path.expanduser('~/tools/PS2Recomp-sotc')


def run(cmd, timeout=3600, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)
    return r.returncode, r.stdout, r.stderr


# ---------------------------------------------------------------- mmi
def check_mmi(work):
    out = {'check': 'mmi', 'ok': False}
    mmi = os.path.join(ROOT, 'tools', 'mmitest')
    if WIN:
        b = os.path.join(RECOMP, 'build-win')
        libs = [os.path.join(b, 'ps2xRecomp', 'ps2_recomp_lib.lib'), os.path.join(b, 'ps2xRecomp', 'rabbitizer.lib'),
                os.path.join(b, '_deps', 'fmt-build', 'fmt.lib')]
        dwarf = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(b, '_deps')) for f in fs if f.lower().startswith(('dwarf', 'libdwarf')) and f.endswith('.lib')]
        zlib = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(b, '_deps')) for f in fs if f.lower().startswith('zlib') and f.endswith('.lib')]
        inc = [os.path.join(RECOMP, 'ps2xRecomp', 'include'), os.path.join(b, '_deps', 'elfio-src'), os.path.join(RECOMP, 'ps2xRuntime', 'include'),
               os.path.join(b, '_deps', 'fmt-src', 'include'), os.path.join(b, '_deps', 'toml11-src', 'include'), os.path.join(b, '_deps', 'rabbitizer-src', 'include')]
        gen = os.path.join(work, 'mmigen.exe')
        code, so, se = run(['cl', '/nologo', '/std:c++20', '/EHsc', '/O1', '/MD', '/w'] + ['/I' + i for i in inc] +
                           [os.path.join(mmi, 'gen.cpp'), '/Fe' + gen, '/Fo' + os.path.join(work, '')] + ['/link'] + libs + dwarf + zlib, cwd=work)
        if code:
            out['error'] = 'generator build failed: ' + '\n'.join(l for l in (so + se).splitlines() if 'error' in l)[:1500]
            return out
        cases = os.path.join(work, 'mmi_cases.inc')
        code, so, se = run([gen, cases])
        exe = os.path.join(work, 'mmitest.exe')
        code, so, se = run(['cl', '/nologo', '/std:c++20', '/EHsc', '/O1', '/w', '/I' + work, '/I' + os.path.join(RECOMP, 'ps2xRuntime', 'include'),
                            '/I' + os.path.join(RECOMP, 'ps2xIOP', 'include'), os.path.join(mmi, 'harness.cpp'), '/Fe' + exe, '/Fo' + os.path.join(work, '')], cwd=work)
        if code:
            out['error'] = 'harness build failed: ' + '\n'.join(l for l in (so + se).splitlines() if 'error' in l)[:1500]
            return out
        code, so, se = run([exe, '20000'], timeout=1800)
    else:
        code, so, se = run([os.path.join(mmi, 'run.sh'), RECOMP, '20000'], timeout=1800)
    wrong = [l.split()[0] for l in so.splitlines() if ' WRONG ' in l]
    detail = {}
    lines = so.splitlines()
    for i, l in enumerate(lines):
        if ' WRONG ' in l:
            detail[l.split()[0]] = '\n'.join(lines[i:i + 5])
    m = re.search(r'(\d+) instructions, (\d+) wrong', so)
    out.update({'ok': True, 'instructions': int(m.group(1)) if m else None, 'wrong': wrong, 'evidence': detail})
    return out


# ---------------------------------------------------------------- meshes
def mesh_pack(key):
    import jadetex
    import jade2gltf
    t0 = time.time()
    res = {'pack': '%08x' % key}
    try:
        blob = jadetex.big_entry(BIGFILE, key)
        pack = jadetex.unpack_chunks(blob)
        recs = jadetex.records(pack)
    except Exception as e:  # noqa: BLE001
        res['error'] = 'unpack: %s: %s' % (type(e).__name__, str(e)[:200])
        return res
    geo = [k for k, (o, s) in recs.items() if s >= 8 and struct.unpack_from('<I', pack, o)[0] == 1 and 0 < struct.unpack_from('<I', pack, o + 4)[0] < 16]
    res.update({'records': len(recs), 'geometry': len(geo)})
    # exact-size parse of every geometry record
    import c2py_rt
    rt0 = c2py_rt.Runtime()
    c2py_rt.load_funcs(rt0, os.path.join(ROOT, 'game', 'analysis', 'meshport', 'py'))
    bad = []
    for k in geo:
        o, s = recs[k]
        try:
            end, _ = c2py_rt.parse_record(None, pack, o, s, rt0.funcs)
            if end != s:
                bad.append({'record': '%08x' % k, 'size': s, 'parse_end': end})
        except Exception as e:  # noqa: BLE001
            bad.append({'record': '%08x' % k, 'size': s, 'exception': '%s: %s' % (type(e).__name__, str(e)[:160])})
    res['geometry_bad'] = bad[:20]
    res['geometry_bad_count'] = len(bad)
    # element decoding
    try:
        els = jade2gltf.capture_elements(pack)
    except Exception as e:  # noqa: BLE001
        res['error'] = 'capture: %s: %s' % (type(e).__name__, str(e)[:200])
        return res
    decoded = empty = instanced = other = 0
    empty_examples = []
    for key2, o, s, mat, entry, chain in els:
        nb, lods, passes = jade2gltf.element_parts(pack, o)
        if passes:
            bs = jade2gltf.decode_packet(pack, passes[0][1], passes[0][2], {b: (lo, ls) for b, lo, ls in lods})
            if any(8 in b for b in bs):
                decoded += 1
            else:
                empty += 1
                if len(empty_examples) < 10:
                    empty_examples.append({'record': '%08x' % key2, 'element_offset': o, 'lods': [b for b, _, _ in lods], 'passes': [i for i, _, _ in passes]})
        elif any(v == 3 for v, _, _ in lods):
            instanced += 1
        else:
            other += 1
    res.update({'elements': len(els), 'decoded': decoded, 'decoded_empty': empty, 'instanced': instanced,
                'no_data': other, 'empty_examples': empty_examples, 'seconds': round(time.time() - t0, 1)})
    return res


def converter_fingerprint():
    import hashlib
    h = hashlib.sha1()
    for p in [os.path.join(ROOT, 'tools', f) for f in ('jade2gltf.py', 'c2py_rt.py', 'jadetex.py')] + \
             sorted(os.path.join(ROOT, 'game', 'analysis', 'meshport', 'py', f) for f in os.listdir(os.path.join(ROOT, 'game', 'analysis', 'meshport', 'py'))):
        h.update(open(p, 'rb').read())
    return h.hexdigest()[:12]


def check_meshes(work, limit=None, deadline=None):
    """Each pack's result is saved as soon as it is done (game/analysis/nightly/meshes/<converter
    fingerprint>/<pack>.json) and skipped on later runs, so coverage accumulates across nights and an
    interrupted run keeps everything finished so far. The summary covers all saved packs."""
    out = {'check': 'meshes', 'ok': False}
    store = os.path.join(ROOT, 'game', 'analysis', 'nightly', 'meshes', converter_fingerprint())
    os.makedirs(store, exist_ok=True)
    with open(BIGFILE, 'rb') as f:
        head = f.read(0x44)
        n = struct.unpack_from('<I', head, 8)[0]
        table = f.read(8 * n)
    keys = sorted(k for o, k in (struct.unpack_from('<II', table, 8 * i) for i in range(n)) if k >> 24 == 0xFF)
    if limit:
        keys = keys[:limit]
    todo = [k for k in keys if not os.path.exists(os.path.join(store, '%08x.json' % k))]
    # Few workers, and a fresh process per pack: one large pack's simulated memory can take
    # several GB (it exhausted a 16 GB Mac with one worker per core).
    workers = int(os.environ.get('POP_NIGHTLY_WORKERS', '3'))
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers, max_tasks_per_child=1) as ex:
        futures = {ex.submit(mesh_pack, k): k for k in todo}
        for fut in concurrent.futures.as_completed(futures):
            try:
                r = fut.result()
            except Exception as e:  # noqa: BLE001
                r = {'pack': '%08x' % futures[fut], 'error': 'worker: %s: %s' % (type(e).__name__, str(e)[:200])}
            json.dump(r, open(os.path.join(store, r['pack'] + '.json'), 'w'))
            if deadline and time.time() > deadline:
                for f in futures:
                    f.cancel()
                break
    results = [json.load(open(os.path.join(store, '%08x.json' % k))) for k in keys if os.path.exists(os.path.join(store, '%08x.json' % k))]
    out['packs_total'] = len(keys)
    out['converter'] = os.path.basename(store)
    tot = lambda k: sum(r.get(k, 0) for r in results)
    out.update({'ok': True, 'packs': len(results), 'pack_errors': [r for r in results if 'error' in r][:20],
                'geometry': tot('geometry'), 'geometry_bad': tot('geometry_bad_count'),
                'elements': tot('elements'), 'decoded': tot('decoded'), 'decoded_empty': tot('decoded_empty'),
                'instanced': tot('instanced'), 'no_data': tot('no_data'),
                'worst_packs': sorted(({'pack': r['pack'], 'geometry_bad': r.get('geometry_bad_count', 0), 'decoded_empty': r.get('decoded_empty', 0),
                                        'examples': (r.get('geometry_bad') or [])[:3] + (r.get('empty_examples') or [])[:3]}
                                       for r in results if r.get('geometry_bad_count') or r.get('decoded_empty')),
                                      key=lambda x: -(x['geometry_bad'] + x['decoded_empty']))[:15]})
    return out


# ---------------------------------------------------------------- boot
def check_boot(work):
    out = {'check': 'boot', 'ok': False}
    if not WIN:
        out['error'] = 'boot check runs on the PC only'
        return out
    rs = os.path.join(ROOT, 'game', 'recomp-sotc')
    frame, cam, adpcm = os.path.join(rs, 'nightly_frame.txt'), os.path.join(rs, 'nightly_cam.log'), os.path.join(rs, 'nightly_adpcm.cap')
    for p in (frame, cam, adpcm):
        if os.path.exists(p):
            os.remove(p)
    env = ';'.join(['POP_CAMLOG=' + cam, 'POP_FRAMECAP=' + frame + ':600', 'POP_CAPTURE_ADPCM=' + adpcm, 'POP_SNDLOG=1',
                    'PS2X_STATE_LOAD_AT=120:states/gameplay1.state'])
    code, so, se = run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(ROOT, 'tools', 'windows', 'boot.ps1'),
                        '-Play', '-Out', r'game\recomp-sotc', '-Seconds', '60', '-Name', 'nightly', '-Env', env], timeout=600, cwd=ROOT)
    logs = [os.path.join(rs, f) for f in os.listdir(rs) if f.startswith('nightly.log')]
    text = ''
    for p in logs:
        text += open(p, errors='replace').read()
    fps = [float(x) for x in re.findall(r'\[pop:fps\][^\d]*([\d.]+)', text)]
    missing = sorted(set(re.findall(r'missing-target[^\n]*?(0x[0-9a-fA-F]+)', text)))
    exceptions = [l for l in text.splitlines() if re.search(r'exception|fatal|assert', l, re.I)][:10]
    out.update({'ok': True, 'exit': code, 'log_bytes': len(text), 'fps_median': sorted(fps)[len(fps) // 2] if fps else None,
                'missing_targets': missing[:30], 'exception_lines': exceptions})
    if os.path.exists(frame):
        lines = open(frame).read().splitlines()
        pool = [l for l in lines if l.startswith('M ') and any(float(x) != 0.0 for x in l.split()[2:18])]
        out['framecap'] = {'element_draws': sum(1 for l in lines if l.startswith('E ')), 'ref_draws': sum(1 for l in lines if l.startswith('R ')),
                           'pool_matrices': len(pool)}
    else:
        out['framecap'] = None
    if os.path.exists(adpcm):
        code, so, se = run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'adpcm_replay.py'), ELF, adpcm], timeout=600)
        out['adpcm_replay'] = so.strip().splitlines()[-1] if so.strip() else se.strip()[-300:]
        out['adpcm_ok'] = 'matches every sample' in so
    else:
        out['adpcm_replay'], out['adpcm_ok'] = 'no decoder capture written', False
    return out


# ---------------------------------------------------------------- diff
def diff(prev, cur):
    changes = []
    for name, c in cur.items():
        p = prev.get(name)
        if not p:
            continue
        for k, v in c.items():
            if isinstance(v, (int, float, str, bool)) and k in p and p[k] != v and k not in ('seconds',):
                changes.append({'check': name, 'field': k, 'before': p[k], 'after': v})
            if isinstance(v, list) and k in ('wrong', 'missing_targets') and k in p:
                added, removed = sorted(set(v) - set(p[k])), sorted(set(p[k]) - set(v))
                if added or removed:
                    changes.append({'check': name, 'field': k, 'added': added, 'removed': removed})
    return changes


def main(argv):
    only = argv[argv.index('--only') + 1].split(',') if '--only' in argv else ['mmi', 'meshes', 'boot']
    limit = int(argv[argv.index('--packs') + 1]) if '--packs' in argv else None
    date = argv[argv.index('--date') + 1] if '--date' in argv else datetime.date.today().isoformat()
    base = os.path.join(ROOT, 'game', 'analysis', 'nightly')
    outdir = os.path.join(base, date)
    os.makedirs(outdir, exist_ok=True)
    work = os.path.join(tempfile.gettempdir(), 'pop-nightly')
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    results = {}
    hours = float(argv[argv.index('--hours') + 1]) if '--hours' in argv else None
    deadline = time.time() + 3600 * hours if hours else None
    # quick checks first, so even a short run has results; the mesh check resumes across runs
    for name, fn in (('mmi', check_mmi), ('boot', check_boot), ('meshes', lambda w: check_meshes(w, limit, deadline))):
        if name not in only:
            continue
        t0 = time.time()
        print(f'== {name}', flush=True)
        try:
            r = fn(work)
        except Exception as e:  # noqa: BLE001
            r = {'check': name, 'ok': False, 'error': '%s: %s' % (type(e).__name__, str(e)[:500])}
        r['seconds'] = round(time.time() - t0)
        results[name] = r
        json.dump(r, open(os.path.join(outdir, name + '.json'), 'w'), indent=1)
        print(json.dumps({k: v for k, v in r.items() if not isinstance(v, (list, dict))}), flush=True)
    previous = sorted(d for d in os.listdir(base) if d < date and os.path.isdir(os.path.join(base, d)))
    prev = {}
    if previous:
        pd = os.path.join(base, previous[-1])
        for f in os.listdir(pd):
            if f.endswith('.json') and f[:-5] in results:
                prev[f[:-5]] = json.load(open(os.path.join(pd, f)))
    json.dump({'previous': previous[-1] if previous else None, 'changes': diff(prev, results)}, open(os.path.join(outdir, 'diff.json'), 'w'), indent=1)
    print(f'results in {outdir}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
