#!/bin/zsh
# Ghidra cleanup -> PS2Recomp export -> recompile -> sync into runtime worktree -> build.
# Logs go to game/ghidra/ and game/recomp/. Run from anywhere.
#
# Usage: tools/rebuild.sh          full pipeline (needed after any Ghidra-side change:
#                                  data ranges, names, PS2Recomp recompiler changes)
#        tools/rebuild.sh --fast   overrides and runtime only (tools/overrides/*.cpp or
#                                  runtime sources changed); skips Ghidra and the recompile
set -euo pipefail

FAST=0
[[ ${1:-} == --fast ]] && FAST=1

ROOT=${0:A:h:h}
GHIDRA=~/tools/ghidra_12.1.4_PUBLIC/support/analyzeHeadless
# PS2RECOMP / POP_R select another PS2Recomp checkout and output folder (e.g. the pop-sotc setup:
# PS2RECOMP=~/tools/PS2Recomp-sotc POP_R=game/recomp-sotc).
RECOMP_SRC=${PS2RECOMP:-~/tools/PS2Recomp}
R=${POP_R:-$ROOT/game/recomp}
[[ $R == /* ]] || R=$ROOT/$R
RT=$R/runtime-src

export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
export PATH=/opt/homebrew/opt/openjdk@21/bin:$PATH

cd $ROOT

STAGE_START=$SECONDS
stage() {
    (( SECONDS > STAGE_START )) && echo "   ($(( SECONDS - STAGE_START ))s)"
    STAGE_START=$SECONDS
    echo "== $1"
}

# Runs a headless Ghidra script and stops the pipeline if the script failed
# (Ghidra itself still exits 0 in that case).
ghidra() {
    local log=$1; shift
    $GHIDRA game/ghidra PoP_SLUS20743 -process SLUS_207.43 -noanalysis "$@" > $log 2>&1
    if grep -q 'SCRIPT ERROR' $log; then
        echo "GHIDRA SCRIPT FAILED, see $log"; grep -m3 -A2 'SCRIPT ERROR' $log; exit 1
    fi
}

if (( ! FAST )); then
    # Tools and runtime both come from the local "pop" branch (tools/ps2recomp-pop-branch.sh).
    stage "PS2Recomp tools ($(git -C $RECOMP_SRC rev-parse --abbrev-ref HEAD) $(git -C $RECOMP_SRC rev-parse --short HEAD))"
    cmake --build $RECOMP_SRC/build-mac --target ps2_recomp ps2_analyzer > $R/tools_build.log 2>&1 \
        || { echo "TOOLS BUILD FAILED"; grep -m10 'error:' $R/tools_build.log; exit 1; }
    git -C $RT checkout -q --detach $(git -C $RECOMP_SRC rev-parse HEAD)
    echo "runtime worktree at $(git -C $RT rev-parse --short HEAD)"

    stage "Ghidra cleanup"
    ghidra game/ghidra/headless_cleanup.log -scriptPath tools/ghidra -postScript PoPMarkDataRanges.java
    grep 'PoPMarkDataRanges.java>' game/ghidra/headless_cleanup.log | sed 's/.*> //; s/ (GhidraScript)//'

    stage "SCE SDK signature names"
    mkdir -p $R/analyzer
    $RECOMP_SRC/build-mac/ps2xAnalyzer/ps2_analyzer game/ps2/disc/SLUS_207.43 $R/analyzer/analyzer.toml \
        > $R/analyzer/analyzer_run.log 2>&1
    grep -oE '"[A-Za-z0-9_]+@0x[0-9A-Fa-f]+"' $R/analyzer/analyzer.toml | tr -d '"' | sed 's/@/,/' \
        | sort -u > $R/analyzer/sce_names.csv
    echo "$(wc -l < $R/analyzer/sce_names.csv | tr -d ' ') names from ps2_analyzer"
    ghidra game/ghidra/headless_names.log -scriptPath tools/ghidra -postScript PoPApplyNames.java $R/analyzer/sce_names.csv
    grep 'PoPApplyNames.java>' game/ghidra/headless_names.log | sed 's/.*> //; s/ (GhidraScript)//'

    # Names from the Aug 2003 prototype's symbols (tools/proto_names.py writes the list).
    # Names only: the recompiled code stays the same, because names PS2Recomp would stub
    # carry a pop_ prefix.
    stage "Prototype names"
    PROTO_NAMES=$ROOT/game/proto/analysis/proto_names.tsv
    if [[ -f $PROTO_NAMES ]]; then
        ghidra game/ghidra/headless_proto_names.log -scriptPath tools/ghidra -postScript PoPApplyProtoNames.java $PROTO_NAMES
        grep 'Prototype names:' game/ghidra/headless_proto_names.log | sed 's/.*> //; s/ (GhidraScript)//'
    else
        echo "skipped: no $PROTO_NAMES"
    fi

    stage "Export"
    ghidra game/ghidra/headless_export.log -readOnly \
        -scriptPath $RECOMP_SRC/ps2xRecomp/tools/ghidra -postScript ExportPS2Functions.java \
        $R/config.toml $R/functions.csv
    grep 'ExportPS2Functions.java> Exported [0-9]' game/ghidra/headless_export.log | sed 's/.*> //'
    # Ghidra reports the path with the casing it was launched through; keep one spelling.
    sed -i '' "s|/Users/jon/projects/POP_Decomp|$ROOT|g" $R/config.toml
    # Recompile the game's own libvu0 instead of PS2Recomp's C++ stand-ins: several of those
    # are wrong (InnerProduct and Normalize include w), and the real code is ground truth.
    # The recompiler also stubs by name (its built-in runtime call list), so rename them too.
    sed -i '' '/^  "sceVu0[A-Za-z0-9]*@0x[0-9A-F]*",$/d' $R/config.toml
    sed -i '' 's/[[:<:]]sceVu0/pop_sceVu0/g; s/[[:<:]]_sceVu0/pop__sceVu0/g' $R/functions.csv $R/config.toml
    echo "libvu0: $(grep -c '^pop_sceVu0' $R/functions.csv || true) functions renamed, $(grep -c '"sceVu0' $R/config.toml || true) stubs left"

    stage "Recompile"
    rm -rf $R/output
    $RECOMP_SRC/build-mac/ps2xRecomp/ps2_recomp $R/config.toml > $R/recomp_run.log 2>&1
    errors=$(grep -c '\[error\]' $R/recomp_run.log || true)
    echo "errors=$errors warnings=$(grep -c '\[warning\]' $R/recomp_run.log || true)"
    if (( errors > 0 )) || [[ ! -f $R/output/register_functions.cpp ]]; then
        echo "RECOMPILE FAILED, see $R/recomp_run.log"; grep -m5 '\[error\]' $R/recomp_run.log; exit 1
    fi

    stage "Sync generated code into runtime worktree"
    # Copy by content so unchanged files keep their timestamps (incremental build).
    # --delete: renamed functions get new filenames, and stale .cpp files break the build.
    # Safe because upstream's only runner file (register_functions.cpp) is also generated;
    # our overrides are kept.
    python3 -I $ROOT/tools/sync_generated.py $R/output $RT/ps2xRuntime/src/runner '*.cpp' \
        --delete --keep $(cd $ROOT/tools/overrides && ls *.cpp)
    python3 -I $ROOT/tools/sync_generated.py $R/output $RT/ps2xRuntime/include '*.h'
fi

stage "Sync game overrides"
# Our game overrides must be in the executable's sources, not the runtime library (upstream #264).
python3 -I $ROOT/tools/sync_generated.py $ROOT/tools/overrides $RT/ps2xRuntime/src/runner '*.cpp'

stage "Build runtime"
if cmake --build $RT/build-mac > $R/runtime_build.log 2>&1; then
    echo "build ok, errors=$(grep -c 'error:' $R/runtime_build.log || true)"
else
    echo "BUILD FAILED"; grep -m10 'error:' $R/runtime_build.log; exit 1
fi
stage "Done"
