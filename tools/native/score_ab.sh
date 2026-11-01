#!/bin/bash
# tools/native/score_ab.sh <shots dir> (0.2 step 4): every <save>_off_field-N.png against <save>_gs_field-N.png -> scores.txt + side_*.png
cd "$1" || exit 1
: > scores.txt
for off in *_off_field-*.png; do
  gs=${off/_off_field/_gs_field}; [ -f "$gs" ] || { echo "$off no gs pair" >> scores.txt; continue; }
  n=${off/_off_field/_field}
  python3 -I /Users/jon/Projects/PoP_Decomp/tools/livecmp.py side "$off" "$gs" "side_$n" >/dev/null
  echo "${n%.png} $(python3 -I /Users/jon/Projects/PoP_Decomp/tools/livecmp.py score "$off" "$gs" | tail -1)" >> scores.txt
done
cat scores.txt
