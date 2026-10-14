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

# --spec: documented behaviour per instruction (as a public instruction reference states it), so the
# model translates a specification into code instead of recalling the instruction from memory.
SPECS = {
    'PABSW': 'Parallel Absolute Word. For each of the 4 words i: rd.w[i] = |rt.w[i]| as signed 32-bit; the most negative value 0x80000000 gives 0x7FFFFFFF. rs is not used.',
    'PABSH': 'Parallel Absolute Halfword. For each of the 8 halfwords i: rd.h[i] = |rt.h[i]| as signed 16-bit; 0x8000 gives 0x7FFF. rs is not used.',
    'PADDUH': 'Parallel Add with Unsigned Saturation Halfword. For each of the 8 halfwords i, treating both as unsigned 16-bit: rd.h[i] = min(rs.h[i] + rt.h[i], 0xFFFF).',
    'PSUBUH': 'Parallel Subtract with Unsigned Saturation Halfword. For each of the 8 halfwords i, unsigned: rd.h[i] = rs.h[i] - rt.h[i] if rs.h[i] > rt.h[i], else 0.',
    'PMTHI': 'Parallel Move To HI. HI (all 128 bits) = rs. rd is not used.',
    'PMTLO': 'Parallel Move To LO. LO (all 128 bits) = rs. rd is not used.',
    'PMTHL_LW': 'Parallel Move To HI/LO, Lower Word format (sa = 0). LO.w[0] = rs.w[0], HI.w[0] = rs.w[1], LO.w[2] = rs.w[2], HI.w[2] = rs.w[3]; LO.w[1], LO.w[3], HI.w[1], HI.w[3] keep their values.',
    'PMULTUW': 'Parallel Multiply Unsigned Word. For k = 0 and 1, with i = 2k: p = (uint64) rs.w[i] * rt.w[i] (unsigned 32x32 -> 64). rd doubleword k = p (rd.w[i] = low 32 bits, rd.w[i+1] = high 32 bits). LO.w[i] = low 32 bits of p and LO.w[i+1] = its sign extension (0xFFFFFFFF if bit 31 of the low part is set, else 0); HI.w[i] = high 32 bits of p and HI.w[i+1] = its sign extension.',
    'PMADDW': 'Parallel Multiply-Add Word (signed). Uses only words 0 and 2 of rs and rt (words 1 and 3 are ignored). For k = 0 and 1, with i = 2k (so i is 0, then 2): acc = ((uint64)HI.w[i] << 32) | LO.w[i]; r = acc + (uint64)((int64)(int32)rs.w[i] * (int32)rt.w[i]) (64-bit wrapping). rd.w[i] = low 32 bits of r, rd.w[i+1] = high 32 bits of r. LO.w[i] = low 32 bits of r, LO.w[i+1] = (0xFFFFFFFF if bit 31 of LO.w[i] is set, else 0); HI.w[i] = high 32 bits of r, HI.w[i+1] = (0xFFFFFFFF if bit 31 of HI.w[i] is set, else 0).',
    'PMSUBW': 'Parallel Multiply-Subtract Word (signed). Uses only words 0 and 2 of rs and rt (words 1 and 3 are ignored). For k = 0 and 1, with i = 2k (so i is 0, then 2): acc = ((uint64)HI.w[i] << 32) | LO.w[i]; r = acc - (uint64)((int64)(int32)rs.w[i] * (int32)rt.w[i]) (64-bit wrapping). rd.w[i] = low 32 bits of r, rd.w[i+1] = high 32 bits of r. LO.w[i] = low 32 bits of r, LO.w[i+1] = (0xFFFFFFFF if bit 31 of LO.w[i] is set, else 0); HI.w[i] = high 32 bits of r, HI.w[i+1] = (0xFFFFFFFF if bit 31 of HI.w[i] is set, else 0).',
    'PMADDUW': 'Parallel Multiply-Add Unsigned Word. Uses only words 0 and 2 of rs and rt (words 1 and 3 are ignored). For k = 0 and 1, with i = 2k (so i is 0, then 2): acc = ((uint64)HI.w[i] << 32) | LO.w[i]; r = acc + (uint64)rs.w[i] * (uint64)rt.w[i] (64-bit wrapping). rd.w[i] = low 32 bits of r, rd.w[i+1] = high 32 bits of r. LO.w[i] = low 32 bits of r, LO.w[i+1] = (0xFFFFFFFF if bit 31 of LO.w[i] is set, else 0); HI.w[i] = high 32 bits of r, HI.w[i+1] = (0xFFFFFFFF if bit 31 of HI.w[i] is set, else 0).',
    'PDIVW': 'Parallel Divide Word (signed). Uses only words 0 and 2 of rs and rt (words 1 and 3 are ignored). For k = 0 and 1, with i = 2k (so i is 0, then 2): q = (int32)rs.w[i] / (int32)rt.w[i], r = (int32)rs.w[i] % (int32)rt.w[i] (C semantics). LO.w[i] = q, LO.w[i+1] = (0xFFFFFFFF if q is negative, else 0); HI.w[i] = r, HI.w[i+1] = (0xFFFFFFFF if r is negative, else 0). rd is not used.',
    'PDIVUW': 'Parallel Divide Unsigned Word. Uses only words 0 and 2 of rs and rt (words 1 and 3 are ignored). For k = 0 and 1, with i = 2k (so i is 0, then 2): q = rs.w[i] / rt.w[i], r = rs.w[i] % rt.w[i] (unsigned 32-bit). LO.w[i] = q, LO.w[i+1] = (0xFFFFFFFF if bit 31 of q is set, else 0); HI.w[i] = r, HI.w[i+1] = (0xFFFFFFFF if bit 31 of r is set, else 0). rd is not used.',
    'PDIVBW': 'Parallel Divide Broadcast Word. d = (int16)rt.h[0] (signed). For each of the 4 words i: LO.w[i] = (int32)rs.w[i] / d, HI.w[i] = (int32)rs.w[i] % d (C semantics, signed). rd is not used.',
    'PMADDH': 'Parallel Multiply-Add Halfword. p[i] = (int32)(int16)rs.h[i] * (int16)rt.h[i] for i = 0..7. Then LO.w[0] += p[0], LO.w[1] += p[1], HI.w[0] += p[2], HI.w[1] += p[3], LO.w[2] += p[4], LO.w[3] += p[5], HI.w[2] += p[6], HI.w[3] += p[7] (32-bit wrapping). rd = {LO.w[0], HI.w[0], LO.w[2], HI.w[2]} after the update (rd.w[0] = LO.w[0], rd.w[1] = HI.w[0], rd.w[2] = LO.w[2], rd.w[3] = HI.w[2]).',
    'PMSUBH': 'Parallel Multiply-Subtract Halfword. p[i] = (int32)(int16)rs.h[i] * (int16)rt.h[i] for i = 0..7. Then LO.w[0] -= p[0], LO.w[1] -= p[1], HI.w[0] -= p[2], HI.w[1] -= p[3], LO.w[2] -= p[4], LO.w[3] -= p[5], HI.w[2] -= p[6], HI.w[3] -= p[7] (32-bit wrapping). rd = {LO.w[0], HI.w[0], LO.w[2], HI.w[2]} after the update.',
    'PHMADH': 'Parallel Horizontal Multiply-Add Halfword. p[i] = (int32)(int16)rs.h[i] * (int16)rt.h[i] for i = 0..7. LO.w[0] = p[1] + p[0], HI.w[0] = p[3] + p[2], LO.w[2] = p[5] + p[4], HI.w[2] = p[7] + p[6] (32-bit wrapping); the other HI/LO words are undefined (leave them). rd = {LO.w[0], HI.w[0], LO.w[2], HI.w[2]}.',
    'PHMSBH': 'Parallel Horizontal Multiply-Subtract Halfword. p[i] = (int32)(int16)rs.h[i] * (int16)rt.h[i] for i = 0..7. LO.w[0] = p[1] - p[0], HI.w[0] = p[3] - p[2], LO.w[2] = p[5] - p[4], HI.w[2] = p[7] - p[6] (32-bit wrapping); the other HI/LO words are undefined (leave them). rd = {LO.w[0], HI.w[0], LO.w[2], HI.w[2]}.',
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
// Common alternative spellings models use.
#define getW W
#define getH H
#define getLOw LOw
#define getHIw HIw
'''

PROMPT = """Implement the PlayStation 2 Emotion Engine (R5900) MMI instruction {insn} exactly as the hardware does, as a C++ function.
{spec}
Register model:
- General-purpose registers are 128 bits. Word i (32-bit, i = 0..3, 0 = least significant) of register r: W(ctx, r, i) / setW(ctx, r, i, value) (uint32_t). Halfword i (16-bit, i = 0..7): H(ctx, r, i) / setH(ctx, r, i, value) (int16_t).
- HI and LO are 128 bits too. Word i (0..3) of LO: LOw(ctx, i) / setLOw(ctx, i, value); of HI: HIw(ctx, i) / setHIw(ctx, i, value) (uint32_t).
- rd, rs, rt are register numbers from the instruction encoding (rd is 0 when the instruction has no destination register); sa is the instruction's sa field.
- Standard C++ only (<cstdint> types available). Read all inputs before writing outputs (rd may equal rs or rt).
- No comments or explanations: code only.
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
    # Keep from the function's signature on; drop Markdown fence lines (answers cut off by the
    # token limit lack the closing fence).
    start = text.find('static void mmi_impl')
    if start < 0:
        return None
    code = '\n'.join(l for l in text[start:].splitlines() if not l.strip().startswith('```'))
    depth, opened = 0, False
    for i, ch in enumerate(code):
        if ch == '{':
            depth, opened = depth + 1, True
        elif ch == '}':
            depth -= 1
            if opened and depth == 0:
                return code[:i + 1]
    return code


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
    use_spec = '--spec' in argv
    flags = {'--url', '--model', '--tries', '--only'}
    positional = [a for i, a in enumerate(argv) if not a.startswith('--') and (i == 0 or argv[i - 1] not in flags)]
    recomp = positional[0] if positional else (r'C:\tools\PS2Recomp-sotc' if os.name == 'nt' else os.path.expanduser('~/tools/PS2Recomp-sotc'))
    global OUTDIR
    if use_spec:
        OUTDIR = OUTDIR + '_spec'
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
            spec = ('\nSpecification: ' + SPECS[name] + '\n') if use_spec else ''
            prompt = PROMPT.format(insn=name.replace('_', '.'), spec=spec, feedback=('\n' + feedback + '\n') if feedback else '')
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
