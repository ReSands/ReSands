#!/bin/bash
# Stage cases for the water program P6 from the two water traces (game/analysis/vu1trace, PS2X_VU1_TRACE):
# tools/vu1port/cases.sh <stage name> <entry>:<exit>[,<exit>...]  -> game/analysis/vu1port/<name>.cases + hints
# (images: the courtyard pool's P6 and the two of the healing pool's; tools/vu1progs.py --check names them)
set -u
P=$(cd "$(dirname "$0")/../.." && pwd)
W=$P/game/analysis/vu1port
cd "$W" || exit 1
name=$1 spec=$2
rm -f "$name.cases" "$name.hints"
for t in "091049 1d5b02aa79c05d41" "091734 09a276b5c6469c0f" "091734 1d5b02aa79c05d41"; do
    set -- $t
    ./vu1replay ../vu1trace/play_20261017_$1.trace ../vu1trace tmp.cases --only $2 --stage "$spec" --hints tmp.hints 2>&1 |
        grep -E "^stage|flight" | sed "s/^/  $1 $2: /"
    cat tmp.cases >> "$name.cases"
    cat tmp.hints >> "$name.hints"
done
rm -f tmp.cases tmp.hints
