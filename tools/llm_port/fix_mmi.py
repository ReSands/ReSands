"""Local-model job: implement EE MMI instructions that PS2Recomp translates wrongly, judged by the
instruction test (tools/mmitest/harness.cpp). The model gets only the instruction's name and the
register model (no reference code), writes a plain C++ function, and the harness compares it with
the reference on 20,000 random/edge inputs. On failure the model sees the failing case (or the
compile errors) and tries again, up to --tries times. Passing functions are written to
game/analysis/mmifix/<NAME>.cpp (wiring them into the recompiler is a separate, mechanical step).
Resumable: finished instructions are skipped.

Usage: python3 -I tools/llm_port/fix_mmi.py [--url http://127.0.0.1:11434] [--model gemma4:26b]
                                            [--tries 3] [--only NAME[,NAME]] [PS2Recomp dir]
Needs the PC's Ollama tunnelled to --url (as tools/overnight.command does).
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUTDIR = os.path.join(ROOT, 'game', 'analysis', 'mmifix')
HARNESS = os.path.join(ROOT, 'tools', 'mmitest', 'harness.cpp')

# Instructions PS2Recomp still translates wrongly (tools/mmitest, 14 Oct 2026), with their
# encoding needs: rd used? sa value.
TARGETS = {
    'PABSW': (True, 0), 'PABSH': (True, 0), 'PADDUH': (True, 0), 'PSUBUH': (True, 0),
    'PMTHI': (False, 0), 'PMTLO': (False, 0), 'PMTHL_LW': (False, 0),
    'PMULTUW': (True, 0), 'PMADDW': (True, 0), 'PMSUBW': (True, 0), 'PMADDUW': (True, 0),
    'PDIVW': (False, 0), 'PDIVUW': (False, 0), 'PDIVBW': (False, 0),
    'PMADDH': (True, 0), 'PMSUBH': (True, 0), 'PHMADH': (True, 0), 'PHMSBH': (True, 0),
}

HELPERS = r'''
// Register helpers for model-written instruction implementations (r[0] reads as zero, writes ignored).
static inline uint32_t W(const R5900Context *c, int r, int i) { if (!r) return 0; uint32_t v[4]; std::memcpy(v, &c->r[r], 16); return v[i]; }
static inline void setW(R5900Context *c, int r, int i, uint32_t x) { if (!r) return; uint32_t v[4]; std::memcpy(v, &c->r[r], 16); v[i] = x; std::memcpy(&c->r[r], v, 16); }
static inline int16_t H(const R5900Context *c, int r, int i) { if (!r) return 0; int16_t v[8]; std::memcpy(v, &c->r[r], 16); return v[i]; }
static inline void setH(R5900Context *c, int r, int i, int16_t x) { if (!r) return; int16_t v[8]; std::memcpy(v, &c->r[r], 16); v[i] = x; std::memcpy(&c->r[r], v, 16); }
static inline uint32_t LOw(const R5900Context *c, int i) { const uint64_t d = i < 2 ? c->lo : c->lo1; return (uint32_t)(d >> (32 * (i & 1))); }
static inline uint32_t HIw(const R5900Context *c, int i) { const uint64_t d = i < 2 ? c->hi : c->hi1; return (uint32_t)(d >> (32 * (i & 1))); }
static inline void setLOw(R5900Context *c, int i, uint32_t x) { uint64_t &d = i < 2 ? c->lo : c->lo1; const int s = 32 * (i & 1); d = (d & ~(0xFFFFFFFFull << s)) | ((uint64_t)x << s); }
static inline void setHIw(R5900Context *c, int i, uint32_t x) { uint64_t &d = i < 2 ? c->hi : c->hi1; const int s = 32 * (i & 1); d = (d & ~(0xFFFFFFFFull << s)) | ((uint64_t)x << s); }
'''

PROMPT = """Implement the PlayStation 2 Emotion Engine (R5900) MMI instruction {insn} exactly as the hardware does, as a C++ function.

Register model:
- General-purpose registers are 128 bits. Word i (32-bit, i = 0..3, 0 = least significant) of register r: W(ctx, r, i) / setW(ctx, r, i, value) (uint32_t). Halfword i (16-bit, i = 0..7): H(ctx, r, i) / setH(ctx, r, i, value) (int16_t).
- HI and LO are 128 bits too. Word i (0..3) of LO: LOw(ctx, i) / setLOw(ctx, i, value); of HI: HIw(ctx, i) / setHIw(ctx, i, value) (uint32_t).
- rd, rs, rt are register numbers from the instruction encoding (rd is 0 when the instruction has no destination register); sa is the instruction's sa field.
- Standard C++ only (<cstdint> types available). Read all inputs before writing outputs (rd may equal rs or rt).
{feedback}
Reply with only the complete function, in a ```cpp block:
```cpp
static void mmi_impl(R5900Context *ctx, int rd, int rs, int rt, int sa)
{{
    ...
}}
```"""


def ask(url, model, prompt):
    body = json.dumps({'model': model, 'prompt': prompt, 'stream': False, 'think': False,
                       'options': {'temperature': 0.2, 'repeat_penalty': 1.05, 'num_ctx': 8192, 'num_predict': 1500}}).encode()
    req = urllib.request.Request(url + '/api/generate', body, {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=900) as resp:
        return json.loads(resp.read())['response']


def extract(text):
    m = re.search(r'```(?:cpp|c\+\+|c)?\s*\n(.*?)```', text, re.S)
    code = m.group(1) if m else text
    return code if 'mmi_impl' in code else None


def judge(name, code, recomp, workdir):
    """Compile the candidate into the harness for this one instruction; returns (ok, feedback)."""
    has_rd, sa = TARGETS[name]
    rd = 7 if has_rd else 0
    inc = (HELPERS + '\n' + code + '\n'
           f'static void t_{name}(R5900Context *ctx, uint8_t *, PS2Runtime *) {{ mmi_impl(ctx, {rd}, 5, 6, {sa}); }}\n'
           f'static const TestCase kCases[] = {{ {{"{name}", t_{name}}} }};\n')
    os.makedirs(workdir, exist_ok=True)
    open(os.path.join(workdir, 'mmi_cases.inc'), 'w').write(inc)
    includes = [workdir, os.path.join(recomp, 'ps2xRuntime', 'include'), os.path.join(recomp, 'ps2xIOP', 'include')]
    if os.name == 'nt':
        # MSVC (run from a VS developer environment, as tools/windows/llm-mmifix.ps1 does): real SSE.
        exe = os.path.join(workdir, 'mmitest_one.exe')
        cmd = ['cl', '/nologo', '/std:c++20', '/EHsc', '/O1', '/w'] + ['/I' + i for i in includes] + \
              [HARNESS, '/Fe' + exe, '/Fo' + os.path.join(workdir, '')]
    else:
        exe = os.path.join(workdir, 'mmitest_one')
        cmd = ['c++', '-std=gnu++20', '-O1', '-DUSE_SSE2NEON', '-I' + os.path.join(recomp, 'build-mac', '_deps', 'sse2neon-src')] + \
              ['-I' + i for i in includes] + [HARNESS, '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=workdir)
    if r.returncode:
        out = (r.stdout + '\n' + r.stderr).splitlines()
        errors = [l for l in out if 'error' in l and 'mmi_cases.inc' in l][:8] or [l for l in out if 'error' in l][:8] or out[:8]
        return False, 'Your previous attempt did not compile:\n' + '\n'.join(errors)
    r = subprocess.run([exe, '20000'], capture_output=True, text=True, timeout=120)
    if r.returncode == 0:
        return True, ''
    lines = [l for l in r.stdout.splitlines() if not l.startswith(('110 ', '1 instructions'))][:6]
    return False, ('Your previous attempt gave a wrong result (registers shown as words 3 2 1 0, hex; '
                   '"want" is the correct hardware result):\n' + '\n'.join(lines))


def main(argv):
    url = argv[argv.index('--url') + 1] if '--url' in argv else 'http://127.0.0.1:11434'
    model = argv[argv.index('--model') + 1] if '--model' in argv else 'gemma4:26b'
    tries = int(argv[argv.index('--tries') + 1]) if '--tries' in argv else 3
    only = argv[argv.index('--only') + 1].split(',') if '--only' in argv else None
    flags = {'--url', '--model', '--tries', '--only'}
    positional = [a for i, a in enumerate(argv) if not a.startswith('--') and (i == 0 or argv[i - 1] not in flags)]
    recomp = positional[0] if positional else (r'C:\tools\PS2Recomp-sotc' if os.name == 'nt' else os.path.expanduser('~/tools/PS2Recomp-sotc'))
    os.makedirs(OUTDIR, exist_ok=True)
    workdir = os.path.join(tempfile.gettempdir(), 'pop-mmifix')
    summary = []
    for name in TARGETS:
        if only and name not in only:
            continue
        result_path = os.path.join(OUTDIR, name + '.json')
        if os.path.exists(result_path):
            summary.append((name, json.load(open(result_path))['status'], '(earlier run)'))
            continue
        feedback, history, status, t0 = '', [], 'failed', time.time()
        for attempt in range(1, tries + 1):
            prompt = PROMPT.format(insn=name.replace('_', '.'), feedback=('\n' + feedback + '\n') if feedback else '')
            try:
                text = ask(url, model, prompt)
            except Exception as e:  # noqa: BLE001
                print(f'{name}: model server unreachable ({e}); stopping', flush=True)
                return 2
            code = extract(text)
            if not code:
                ok, feedback = False, 'Your previous reply did not contain the mmi_impl function in a ```cpp block.'
            else:
                ok, feedback = judge(name, code, recomp, workdir)
            history.append({'attempt': attempt, 'ok': ok, 'feedback': feedback, 'code': code or text[:2000]})
            print(f'{name}: attempt {attempt} {"PASS" if ok else "fail"}' + ('' if ok else ' - ' + feedback.splitlines()[0][:90]), flush=True)
            if ok:
                status = 'pass'
                open(os.path.join(OUTDIR, name + '.cpp'), 'w').write(code)
                break
        json.dump({'name': name, 'status': status, 'attempts': len(history), 'seconds': round(time.time() - t0),
                   'history': history}, open(result_path, 'w'), indent=1)
        summary.append((name, status, f'{len(history)} attempt(s), {round(time.time() - t0)} s'))
    print('\n'.join(f'{n:10} {s:6} {info}' for n, s, info in summary))
    print(f'{sum(1 for _, s, _ in summary if s == "pass")} of {len(summary)} pass')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
