#!/bin/bash
# Builds the VU1 port tools as x86_64 (Rosetta on the Mac; the runtime's VU1 code uses SSE, and the trace
# layout must be the PC's). Out: game/analysis/vu1port/ (git-ignored).
#   build.sh                        vu1replay (the judge's reference)
#   build.sh <port.cpp> <exe>       the judge with one stage port
#                                   (or a whole program: port.py --chain writes a port of every stage)
R=${PS2RECOMP_SOTC:-$HOME/tools/PS2Recomp-sotc}
P=$(cd "$(dirname "$0")/../.." && pwd)
W=$P/game/analysis/vu1port
mkdir -p "$W"
# no FMA contraction: the VU rounds each multiply and add; AddressSanitizer catches stray memory accesses
JFLAGS="-arch x86_64 -O1 -g -std=c++17 -ffp-contract=off -fsanitize=address -I $P/tools/vu1port"
# VU1_TIMING=1: an optimised judge without the sanitizer, for judge --time
if [ "${VU1_TIMING:-0}" = 1 ]; then JFLAGS="-arch x86_64 -O2 -std=c++17 -ffp-contract=off -I $P/tools/vu1port"; fi
# VU_TASK_R=1: programs that use the random-number register (P9): VuState carries R, the replayer is vu1replay_r
RDEF="" RSUF=""
if [ "${VU_TASK_R:-0}" = 1 ]; then RDEF="-DVU_TASK_R=1" RSUF="_r"; JFLAGS="$JFLAGS $RDEF"; fi
if [ -z "${1:-}" ]; then
    INC="-I $R/ps2xRuntime/include -I $R/ps2xIOP/include -I $R/ps2xRuntime/src/lib/vu -I $R/ps2xRuntime/src/lib"
    clang++ -arch x86_64 -O2 -std=c++20 -msse4.1 $RDEF $INC -I "$P/tools/vu1port" "$P/tools/vu1port/replay.cpp" -o "$W/vu1replay$RSUF"
else
    clang++ $JFLAGS "$P/tools/vu1port/judge.cpp" "$1" -o "$2"
fi
