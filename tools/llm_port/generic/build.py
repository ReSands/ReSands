"""Builds a generic task: assembles the standalone Python reference (the spec the model sees) from
the tool functions the task names plus its canon(), runs it on the task's inputs to write the
expected outputs, and copies the inputs. Output: game/analysis/qwen/<task>/{reference.py, tests/}.
Usage: python3 -I tools/llm_port/generic/build.py <task> [<task> ...]
"""
import ast
import glob
import importlib.util
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))


def segments(module, names):
    src = open(os.path.join(ROOT, 'tools', module + '.py')).read()
    out = []
    for node in ast.parse(src).body:
        name = getattr(node, 'name', None) or (node.targets[0].id if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name) else None)
        if name in names:
            out.append(ast.get_source_segment(src, node))
    missing = set(names) - {getattr(n, 'name', None) or (n.targets[0].id if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Name) else None) for n in ast.parse(src).body}
    if missing:
        raise SystemExit(f'{module}: not found: {missing}')
    return out


def load_task(name):
    spec = importlib.util.spec_from_file_location('task_' + name, os.path.join(HERE, 'tasks', name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build(name):
    t = load_task(name)
    parts = ['"""Reference for task %s: canon(data) -> text. Standalone (assembled by build.py)."""' % name,
             'import math\nimport struct']
    for module, names in t.SOURCES:
        parts += segments(module, names)
    parts.append(open(os.path.join(HERE, 'tasks', name + '.py')).read().split('# --- canon')[1].strip())
    ref = '\n\n\n'.join(parts) + '\n'
    for a, b in getattr(t, 'REPLACE', []):
        ref = ref.replace(a, b)
    out = os.path.join(ROOT, 'game', 'analysis', 'qwen', name)
    tests = os.path.join(out, 'tests')
    os.makedirs(tests, exist_ok=True)
    open(os.path.join(out, 'reference.py'), 'w').write(ref)
    ns = {}
    exec(compile(ref, 'reference.py', 'exec'), ns)
    for path in t.inputs(ROOT):
        base = os.path.join(tests, os.path.basename(path))
        if not os.path.exists(base):
            shutil.copyfile(path, base) if os.path.getsize(path) < (64 << 20) else os.symlink(path, base)
        data = open(path, 'rb').read()
        open(base + '.expected', 'w').write(ns['canon'](data))
        print(name, os.path.basename(path), 'ok')


if __name__ == '__main__':
    for n in sys.argv[1:]:
        build(n)
