"""Copy files matching a pattern from src to dst only when their content differs,
optionally deleting dst files (matching the pattern) that src no longer has.

The recompiler rewrites every output file on each run; copying by content keeps
unchanged files' timestamps, so the runtime build stays incremental.

Usage: python -I sync_generated.py SRC DST PATTERN [--delete] [--keep NAME ...]
"""
import argparse
import fnmatch
import os
import shutil


def main():
    p = argparse.ArgumentParser()
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("pattern")
    p.add_argument("--delete", action="store_true")
    p.add_argument("--keep", nargs="*", default=[], help="dst files never deleted")
    a = p.parse_args()

    os.makedirs(a.dst, exist_ok=True)
    src_names = {n for n in os.listdir(a.src) if fnmatch.fnmatch(n, a.pattern)}
    copied = 0
    for name in src_names:
        s = os.path.join(a.src, name)
        d = os.path.join(a.dst, name)
        with open(s, "rb") as f:
            data = f.read()
        if os.path.exists(d):
            with open(d, "rb") as f:
                if f.read() == data:
                    continue
        with open(d, "wb") as f:
            f.write(data)
        copied += 1

    deleted = 0
    if a.delete:
        keep = set(a.keep)
        for name in os.listdir(a.dst):
            if fnmatch.fnmatch(name, a.pattern) and name not in src_names and name not in keep:
                os.remove(os.path.join(a.dst, name))
                deleted += 1
    print(f"{a.pattern}: {copied} changed, {deleted} deleted, {len(src_names)} total")


if __name__ == "__main__":
    main()
