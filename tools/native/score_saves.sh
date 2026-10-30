#!/bin/bash
# Native frame of every save in a folder against the game's own picture (offline, Mac or PC with bash):
# pop_state_frame -> pop_native_view --frame, tools/popstate.py --frame, tools/livecmp.py score and side.
# Usage: tools/native/score_saves.sh <states dir> <out dir>   (run from anywhere; needs game/native-build)
# Output: <out>/<save>.{pnf,log,native.png,game.png,side.png}, <out>/scores.txt (one line per save).
# STATE_FRAME=pop_state_frame_vu1 for the VU1 paths (sprites, P5, P6, halos); VIEW_ARGS e.g. --noglow.
set -u
P=$(cd "$(dirname "$0")/../.." && pwd)
IN=$(cd "$1" && pwd)
mkdir -p "$2" && OUT=$(cd "$2" && pwd)
B=$P/game/native-build
echo 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 > "$OUT/identity.clip"
: > "$OUT/scores.txt"
cd "$P/game/ps2/disc"
for st in "$IN"/*.state; do
    n=$(basename "$st" .state)
    "$B/${STATE_FRAME:-pop_state_frame}" "$st" "$OUT/$n.pnf" --bigfile DATA/PRINCE.BF --packindex "$P/game/analysis/packindex.txt" \
        > "$OUT/$n.log" 2>&1 || { echo "$n state_frame FAILED" | tee -a "$OUT/scores.txt"; continue; }
    "$B/pop_native_view" "$OUT/$n.pnf" "$OUT/identity.clip" "$OUT/$n.native.png" --frame ${VIEW_ARGS:-} >> "$OUT/$n.log" 2>&1 ||
        { echo "$n view FAILED" | tee -a "$OUT/scores.txt"; continue; }
    python3 -I "$P/tools/popstate.py" "$st" --frame "$OUT/$n.game.png" >> "$OUT/$n.log" 2>&1
    python3 -I "$P/tools/livecmp.py" side "$OUT/$n.native.png" "$OUT/$n.game.png" "$OUT/$n.side.png" >> "$OUT/$n.log" 2>&1
    echo "$n $(python3 -I "$P/tools/livecmp.py" score "$OUT/$n.native.png" "$OUT/$n.game.png" | tail -1)" | tee -a "$OUT/scores.txt"
done
