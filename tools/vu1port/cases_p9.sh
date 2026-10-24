#!/bin/bash
# Stage cases for the sprite program P9 (tools/vu1port/port.py STAGES_P9) from the P9 runs of saves:
#   1. record:  tools/vu1port/cases_p9.sh --record <states dir>...   (pop_state_frame_vu1 POP_P9_TRACE per save,
#               appended to game/analysis/p9trace/all.trace; needs game/native-build and the bigfile)
#   2. cut:     tools/vu1port/cases_p9.sh [stage...]                 (default: every P9 stage; game/analysis/vu1port/
#               <stage>.cases + .hints, at most 600 cases spread evenly over the trace; p9_whole.cases = whole runs)
# Needs vu1replay_r (VU_TASK_R=1 tools/vu1port/build.sh). In flight at a boundary and allowed: xgkick (the judge
# takes a kick's packet at the XGKICK), MAC and status flags (the hints show which flags each FMAND reads; P9 has no
# status reader), Q (the snapshot holds Q as the DIV in flight leaves it, and every Q reader after such a boundary reads
# that DIV's result: hints); the check at the end lists FMANDs that read flags from before their stage's entry (must
# be none).
set -u
P=$(cd "$(dirname "$0")/../.." && pwd)
T=$P/game/analysis/p9trace
W=$P/game/analysis/vu1port
HASH=04994ae7ba41b01f
MAX=600
if [ "${1:-}" = --record ]; then
    shift
    mkdir -p "$T"
    cd "$P/game/ps2/disc" || exit 1
    for d in "$@"; do ls "$d"/*.state; done | xargs -P 8 -I{} sh -c 'b=$(basename {} .state);
        POP_NATIVE_WORLD=1 POP_NATIVE_TRANSLUCENT=1 POP_NATIVE_LATE=1 POP_P9_TRACE='"$T"'/$b.trace \
        '"$P"'/game/native-build/pop_state_frame_vu1 {} /dev/null --world --bigfile DATA/PRINCE.BF \
        --packindex '"$P"'/game/analysis/packindex.txt 2>&1 | grep p9trace | sed "s/^/$b /"'
    find "$T" -name 'play_*.trace' -size +0 | sort | xargs cat > "$T/all.trace"
    ls -la "$T/all.trace"
    exit 0
fi
cd "$W" || exit 1
stages=("$@")
if [ ${#stages[@]} -eq 0 ]; then
    stages=($(python3 -I -c "import sys; sys.path.insert(0, '$P/tools/vu1port'); import port; print(' '.join(port.STAGES_P9))"))
fi
one() {
    local name=$1 spec
    spec=$(python3 -I -c "import sys; sys.path.insert(0, '$P/tools/vu1port'); import port
s = port.STAGES_P9['$name']; print('%x:%s' % (s['entry'], ','.join('%x' % e for e in s['exits'] if e != 0xffffffff) or 'ffff'))")
    local n
    n=$(./vu1replay_r "$T/all.trace" "$T" /dev/null --only $HASH --stage "$spec" --allow xgkick,MAC,status,Q --max-cases 0 2>&1 |
        sed -n 's/^stage [0-9a-f]*: \([0-9]*\) cases.*/\1/p')
    local stride=$(( (n + MAX - 1) / MAX )); [ $stride -lt 1 ] && stride=1
    ./vu1replay_r "$T/all.trace" "$T" "$name.cases.new" --only $HASH --stage "$spec" --allow xgkick,MAC,status,Q \
        --max-cases $MAX --stride $stride --hints "$name.hints.new" 2>&1 | grep -A2 '^stage' | tr '\n' ' ' | sed "s/^/$name: /"
    echo
    mv "$name.cases.new" "$name.cases" && mv "$name.hints.new" "$name.hints"
}
export -f one
export P T W HASH MAX
printf '%s\n' "${stages[@]}" | xargs -P 8 -I{} bash -c 'one {}'
if [ $# -eq 0 ]; then
    # whole runs for port.py --chain p9: the MSCALF runs (0x340) and the MSCNT runs (0x390) to the program end
    rm -f p9_whole.cases.new
    for e in 340 390; do
        ./vu1replay_r "$T/all.trace" "$T" tmp_whole.cases --only $HASH --stage $e:ffff --allow xgkick,MAC,status,Q \
            --max-cases 300 --stride 5 2>&1 | grep '^stage' | sed 's/^/p9_whole: /'
        cat tmp_whole.cases >> p9_whole.cases.new
    done
    rm -f tmp_whole.cases
    mv p9_whole.cases.new p9_whole.cases
fi
# FMANDs reading MAC flags from before their stage's entry: the boundary would not be safe
python3 -I - "${stages[@]}" <<'EOF'
import os, re, sys
sys.path.insert(0, os.path.join(os.environ['P'], 'tools'))
from vudis import disasm
img = open(os.path.join(os.environ['T'], 'vu1_%s.bin' % os.environ['HASH']), 'rb').read()
bad = 0
for name in sys.argv[1:]:
    path = os.path.join(os.environ['W'], name + '.hints')
    if not os.path.exists(path):
        continue
    for line in open(path):
        f = line.split()
        pc = int(f[0], 16)
        if 'mac' not in f:
            continue
        rest = f[f.index('mac') + 1:]
        srcs = rest[:next((k for k, t in enumerate(rest) if t in ('q', 'clip', 'vibackup')), len(rest))]
        if 'entry' in srcs:
            lower = disasm(img[pc:pc + 8], pc)[0][41:]
            if re.match(r'FM(AND|OR|EQ)', lower.strip()):
                print('%s: FMAND at 0x%x reads MAC flags from before the entry' % (name, pc))
                bad += 1
print('MAC check: %d FMANDs read flags from before their stage' % bad)
EOF
