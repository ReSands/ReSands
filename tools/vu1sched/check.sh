#!/bin/bash
# Mac check for vu1_recomp's build-time scheduler (runtime patch 37): builds vu1_recomp as x86_64 (Rosetta),
# writes the ELF programs, generates code with VU1R_SCHED=1 and syntax-checks the generated files.
# Usage: tools/vu1sched/check.sh [glob of generated files to check, default '*_sched*.cpp']
# Work dir: game/analysis/vu1sched (git-ignored, game code).
R=$HOME/tools/PS2Recomp-sotc
P=$(cd "$(dirname "$0")/../.." && pwd)
W=$P/game/analysis/vu1sched
mkdir -p $W
INC="-I $R/ps2xRuntime/include -I $R/ps2xIOP/include -I $R/ps2xRuntime/src/lib/vu -I $R/ps2xRuntime/src/lib"
clang++ -arch x86_64 -O1 -std=c++20 -msse4.1 $INC $R/ps2xTest/tools/vu1_recomp.cpp $R/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp \
    $P/tools/vu1sched/stubs.cpp -o $W/vu1_recomp 2>&1 | grep error && exit 1
python3 -I $P/tools/vu1progs.py $P/game/ps2/disc/SLUS_207.43 $W/progs --check $P/game/analysis/vu1img | tail -2
cd $W && rm -rf out && mkdir out && VU1R_SCHED=1 ./vu1_recomp progs out | grep sched
cd out && for f in ${1:-*_sched*.cpp}; do clang++ -arch x86_64 -std=c++20 -fsyntax-only -msse4.1 $INC $f 2>&1 | grep error: ; done |
    sed "s/^[^:]*:[0-9]*:[0-9]*: //" | sort | uniq -c | sort -rn | head -20
