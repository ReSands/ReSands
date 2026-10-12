#!/bin/zsh
# Boot the recompiled game from the disc folder with a time limit.
# Usage: tools/boot.sh [seconds=90] [log name=boot]   (POP_BUILD=build-mac-play for the log-free build)
# Log: game/recomp/<name>.log (capped at 200 MB). Exit 142 means the time limit was hit.
set -uo pipefail

ROOT=${0:A:h:h}
SECS=${1:-90}
LOG=$ROOT/game/recomp/${2:-boot}.log

cd $ROOT/game/ps2/disc
perl -e "alarm $SECS; exec @ARGV" $ROOT/game/recomp/runtime-src/${POP_BUILD:-build-mac}/ps2xRuntime/ps2EntryRunner ./SLUS_207.43 2>&1 \
    | head -c 200000000 > $LOG
code=${pipestatus[1]}
echo "exit=$code lines=$(wc -l < $LOG) log=$LOG"
