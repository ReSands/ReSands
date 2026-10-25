#!/bin/bash
# Stage cases for the character program P4 (tools/vu1port/port.py STAGES_P4) from the three play traces that ran it
# (game/analysis/vu1trace: play_20261017_091049 / 091734, play_20261022_192521; 4,836 runs):
#   tools/vu1port/cases_p4.sh [stage...]   (default: every P4 stage, then p4_whole.cases for port.py --chain p4)
# Out: game/analysis/vu1port/<stage>.cases + .hints, at most 600 cases per stage spread evenly over the three traces.
# Stage borders come from tools/vu1port/cutscan.py, which only puts entries where nothing unsafe is in flight; the
# replayer's per-stage lines below must show no "(!)" kind and no "clip flags read before ..." line (checked at the end).
set -u
P=$(cd "$(dirname "$0")/../.." && pwd)
T=$P/game/analysis/vu1trace
W=$P/game/analysis/vu1port
HASH=31829861c88dc716
MAX=600
TRACES="play_20261017_091049 play_20261017_091734 play_20261022_192521"
cd "$W" || exit 1
stages=("$@")
if [ ${#stages[@]} -eq 0 ]; then
    stages=($(python3 -I -c "import sys; sys.path.insert(0, '$P/tools/vu1port'); import port; print(' '.join(port.STAGES_P4))"))
fi
# cases of one stage spec from every trace, at most $2 in all, each trace's share spread evenly over it
cut() {
    local name=$1 spec=$2 max=$3 log=$4 n total=0 counts="" t
    for t in $TRACES; do
        n=$(./vu1replay "$T/$t.trace" "$T" /dev/null --only $HASH --stage "$spec" --allow xgkick,MAC,status,Q --max-cases 0 2>&1 |
            sed -n 's/^stage [0-9a-f]*: \([0-9]*\) cases.*/\1/p')
        counts="$counts ${n:-0}"
        total=$((total + ${n:-0}))
    done
    rm -f "$name.cases.new" "$name.hints.new"
    set -- $counts
    for t in $TRACES; do
        n=$1; shift
        [ "$n" -eq 0 ] && continue
        local share=$(( (max * n + total - 1) / total ))
        local stride=$(( (n + share - 1) / share )); [ $stride -lt 1 ] && stride=1
        ./vu1replay "$T/$t.trace" "$T" "$name.$t.tmp" --only $HASH --stage "$spec" --allow xgkick,MAC,status,Q \
            --max-cases $share --stride $stride --hints "$name.$t.htmp" 2>&1 | grep -E '^stage|flight|before a CLIP' |
            sed "s/^/$name $t: /" >> "$log"
        cat "$name.$t.tmp" >> "$name.cases.new"
        cat "$name.$t.htmp" >> "$name.hints.new"
        rm -f "$name.$t.tmp" "$name.$t.htmp"
    done
    mv "$name.cases.new" "$name.cases" && { [ -f "$name.hints.new" ] && mv "$name.hints.new" "$name.hints" || true; }
}
one() {
    local name=$1 spec
    spec=$(python3 -I -c "import sys; sys.path.insert(0, '$P/tools/vu1port'); import port
s = port.STAGES_P4['$name']; print('%x:%s' % (s['entry'], ','.join('%x' % e for e in s['exits'] if e != 0xffffffff) or 'ffff'))")
    cut "$name" "$spec" $MAX "$name.cutlog"
    echo "$name: $(python3 -I -c "print(open('$name.cases', 'rb').read().count(b'CASE'))") cases"
}
export -f cut one
export P T W HASH MAX TRACES
rm -f p4_*.cutlog
printf '%s\n' "${stages[@]}" | xargs -P 8 -I{} bash -c 'one {}'
if [ $# -eq 0 ]; then
    # whole runs for port.py --chain p4: from each run start (0 and the MSCNT resume points) to the program end
    rm -f p4_whole.cases.acc
    # (a stage entry 16 bytes after a pair with the E bit: the upper word at entry - 12, bit 30)
    for e in $(python3 -I -c "import sys, struct; sys.path.insert(0, '$P/tools/vu1port'); import port
img = open(port.P4_IMAGE, 'rb').read()
entries = [s['entry'] for s in port.STAGES_P4.values()]
print(' '.join('%x' % e for e in [0] + sorted(e for e in entries if e >= 16 and struct.unpack_from('<I', img, e - 12)[0] >> 30 & 1)))"); do
        cut p4_whole_$e $e:ffff 100 p4_whole.cutlog
        cat p4_whole_$e.cases >> p4_whole.cases.acc
        rm -f p4_whole_$e.cases p4_whole_$e.hints
    done
    mv p4_whole.cases.acc p4_whole.cases
    echo "p4_whole: $(python3 -I -c "print(open('p4_whole.cases','rb').read().count(b'CASE'))") cases"
fi
# the replayer's verdicts: no stage may have an unsafe kind in flight at a border or read clip flags too early
cat p4_*.cutlog > p4_cut.log
bad=$(grep -E '\(!\)|before a CLIP' p4_cut.log | grep -v '^p4_whole' | wc -l)
echo "border check: $bad unsafe lines (p4_cut.log)"
grep -E '\(!\)|before a CLIP' p4_cut.log | grep -v '^p4_whole' | head -20
rm -f p4_*.cutlog
