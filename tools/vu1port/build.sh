#!/bin/bash
# Builds the VU1 port tools as x86_64 (Rosetta on the Mac; the runtime's VU1 code uses SSE, and the trace
# layout must be the PC's): vu1replay (the judge's reference). Out: game/analysis/vu1port/ (git-ignored).
R=${PS2RECOMP_SOTC:-$HOME/tools/PS2Recomp-sotc}
P=$(cd "$(dirname "$0")/../.." && pwd)
W=$P/game/analysis/vu1port
mkdir -p "$W"
INC="-I $R/ps2xRuntime/include -I $R/ps2xIOP/include -I $R/ps2xRuntime/src/lib/vu -I $R/ps2xRuntime/src/lib"
clang++ -arch x86_64 -O2 -std=c++20 -msse4.1 $INC -I "$P/tools/vu1port" "$P/tools/vu1port/replay.cpp" -o "$W/vu1replay"
# judge <port.cpp> <exe>: build the judge with a stage port (no FMA contraction: the VU rounds each
# multiply and add; AddressSanitizer catches stray memory accesses)
judge() {
    clang++ -arch x86_64 -O1 -g -std=c++17 -ffp-contract=off -fsanitize=address -I "$P/tools/vu1port" "$P/tools/vu1port/judge.cpp" "$1" -o "$2"
}
[ -n "${1:-}" ] && judge "$1" "$2"
