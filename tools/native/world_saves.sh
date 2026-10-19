#!/bin/bash
# POP_NATIVE_WORLD offline over every save in a folder: pop_state_frame --world (the draws the game would
# leave to the native renderer) -> pop_native_view --gsz, and the same for the whole native frame; then
# tools/native/world_check.py: how much of the game's final picture (by its Z buffer) the native world
# covers, and whether a native surface ever lies in front of the game's (it must not).
# Usage: tools/native/world_saves.sh <states dir> <out dir>   (needs game/native-build)
set -u
P=$(cd "$(dirname "$0")/../.." && pwd)
IN=$(cd "$1" && pwd)
mkdir -p "$2" && OUT=$(cd "$2" && pwd)
B=$P/game/native-build
echo 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 > "$OUT/identity.clip"
: > "$OUT/world.txt"
cd "$P/game/ps2/disc"
for st in "$IN"/*.state; do
    n=$(basename "$st" .state)
    for mode in world full; do
        flag=; [ $mode = world ] && flag=--world
        "$B/pop_state_frame" "$st" "$OUT/$n.$mode.pnf" --bigfile DATA/PRINCE.BF --packindex "$P/game/analysis/packindex.txt" $flag \
            > "$OUT/$n.$mode.log" 2>&1 || { echo "$n $mode state_frame FAILED" | tee -a "$OUT/world.txt"; continue 2; }
        "$B/pop_native_view" "$OUT/$n.$mode.pnf" "$OUT/identity.clip" "$OUT/$n.$mode.png" --frame --gsz "$OUT/$n.$mode.gsz" \
            >> "$OUT/$n.$mode.log" 2>&1 || { echo "$n $mode view FAILED" | tee -a "$OUT/world.txt"; continue 2; }
    done
    echo "$n $(grep -o '[0-9]* element draws' "$OUT/$n.world.log" | head -1)/$(grep -o '[0-9]* element draws' "$OUT/$n.full.log" | head -1 | cut -d' ' -f1) $(python3 -I "$P/tools/native/world_check.py" "$st" "$OUT/$n.world.gsz" "$OUT/$n.full.gsz")" | tee -a "$OUT/world.txt"
    rm -f "$OUT/$n.full.pnf" "$OUT/$n.world.pnf"
done
