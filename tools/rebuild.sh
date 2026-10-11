#!/bin/zsh
# Ghidra cleanup -> PS2Recomp export -> recompile -> sync into runtime worktree -> build.
# Logs go to game/ghidra/ and game/recomp/. Run from anywhere.
set -euo pipefail

ROOT=${0:A:h:h}
GHIDRA=~/tools/ghidra_12.1.4_PUBLIC/support/analyzeHeadless
RECOMP_SRC=~/tools/PS2Recomp
R=$ROOT/game/recomp
RT=$R/runtime-src

export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
export PATH=/opt/homebrew/opt/openjdk@21/bin:$PATH

cd $ROOT

echo "== Ghidra cleanup"
$GHIDRA game/ghidra PoP_SLUS20743 -process SLUS_207.43 -noanalysis \
    -scriptPath tools/ghidra -postScript PoPMarkDataRanges.java > game/ghidra/headless_cleanup.log 2>&1
grep 'PoPMarkDataRanges.java>' game/ghidra/headless_cleanup.log | sed 's/.*> //; s/ (GhidraScript)//'

echo "== SCE SDK signature names"
mkdir -p $R/analyzer
$RECOMP_SRC/build-mac/ps2xAnalyzer/ps2_analyzer game/ps2/disc/SLUS_207.43 $R/analyzer/analyzer.toml \
    > $R/analyzer/analyzer_run.log 2>&1
grep -oE '"[A-Za-z0-9_]+@0x[0-9A-Fa-f]+"' $R/analyzer/analyzer.toml | tr -d '"' | sed 's/@/,/' \
    | sort -u > $R/analyzer/sce_names.csv
echo "$(wc -l < $R/analyzer/sce_names.csv | tr -d ' ') names from ps2_analyzer"
$GHIDRA game/ghidra PoP_SLUS20743 -process SLUS_207.43 -noanalysis \
    -scriptPath tools/ghidra -postScript PoPApplyNames.java $R/analyzer/sce_names.csv \
    > game/ghidra/headless_names.log 2>&1
grep 'PoPApplyNames.java>' game/ghidra/headless_names.log | sed 's/.*> //; s/ (GhidraScript)//'

echo "== Export"
$GHIDRA game/ghidra PoP_SLUS20743 -process SLUS_207.43 -noanalysis -readOnly \
    -scriptPath $RECOMP_SRC/ps2xRecomp/tools/ghidra -postScript ExportPS2Functions.java \
    $R/config.toml $R/functions.csv > game/ghidra/headless_export.log 2>&1
grep 'ExportPS2Functions.java> Exported [0-9]' game/ghidra/headless_export.log | sed 's/.*> //'
# Ghidra reports the path with the casing it was launched through; keep one spelling.
sed -i '' "s|/Users/jon/projects/POP_Decomp|$ROOT|g" $R/config.toml

echo "== Recompile"
rm -rf $R/output
$RECOMP_SRC/build-mac/ps2xRecomp/ps2_recomp $R/config.toml > $R/recomp_run.log 2>&1
echo "errors=$(grep -c '\[error\]' $R/recomp_run.log || true) warnings=$(grep -c '\[warning\]' $R/recomp_run.log || true)"

echo "== Sync into runtime worktree"
# --delete: renamed functions get new filenames, and stale .cpp files break the build.
# Safe because upstream's only runner file (register_functions.cpp) is also generated.
rsync -rc --delete --include='*.cpp' --exclude='*' $R/output/ $RT/ps2xRuntime/src/runner/
rsync -rc --include='*.h' --exclude='*' $R/output/ $RT/ps2xRuntime/include/

echo "== Build runtime"
if cmake --build $RT/build-mac > $R/runtime_build.log 2>&1; then
    echo "build ok, errors=$(grep -c 'error:' $R/runtime_build.log || true)"
else
    echo "BUILD FAILED"; grep -m10 'error:' $R/runtime_build.log; exit 1
fi
