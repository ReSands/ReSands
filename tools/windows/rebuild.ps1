# Windows version of tools/rebuild.sh: Ghidra cleanup -> SDK names -> export ->
# recompile -> sync into runtime worktree -> MSVC build. Logs in game\ghidra and game\recomp.
#
# Usage: tools\windows\rebuild.ps1          full pipeline
#        tools\windows\rebuild.ps1 -Fast    overrides/runtime only (skips Ghidra and the recompile)
#        tools\windows\rebuild.ps1 -Setup   one-time: runtime worktree + CMake configure, then full
param([switch]$Fast, [switch]$Setup)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root      = (Resolve-Path "$PSScriptRoot\..\..").Path
$Tools     = 'C:\tools'
$Ghidra    = "$Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat"
$RecompSrc = "$Tools\PS2Recomp"
$R         = "$Root\game\recomp"
$RT        = "$R\runtime-src"
$Git       = 'C:\Program Files\Git\cmd\git.exe'
$Python    = (Get-Command python -ErrorAction Stop).Source

# Java 21 for Ghidra (Temurin from winget), unless JAVA_HOME is already set.
if (-not $env:JAVA_HOME) {
    $env:JAVA_HOME = (Get-ChildItem 'C:\Program Files\Eclipse Adoptium' -Directory -Filter 'jdk-21*' |
                      Sort-Object Name -Descending | Select-Object -First 1).FullName
}
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"

# MSVC x64 environment (cl, link) for CMake/Ninja.
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC not found (Visual Studio Build Tools with the C++ workload)' }
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

Set-Location $Root
$sw = [Diagnostics.Stopwatch]::StartNew()
function Stage($name) {
    if ($sw.Elapsed.TotalSeconds -ge 1) { '   ({0:N0}s)' -f $sw.Elapsed.TotalSeconds }
    $sw.Restart()
    "== $name"
}
function Run-Ghidra($log, [string[]]$scriptArgs) {
    & $Ghidra "$Root\game\ghidra" PoP_SLUS20743 -process SLUS_207.43 -noanalysis @scriptArgs *> $log
    if (Select-String -Path $log -Pattern 'SCRIPT ERROR' -Quiet) {
        "GHIDRA SCRIPT FAILED, see $log"; Select-String -Path $log -Pattern 'SCRIPT ERROR' -Context 0,2 | Select-Object -First 3
        exit 1
    }
}
function Show-ScriptLines($log, $script) {
    Select-String -Path $log -Pattern "$script> " | ForEach-Object {
        ($_.Line -replace ".*$script> ", '') -replace ' \(GhidraScript\)\s*$', ''
    }
}

if ($Setup) {
    Stage 'One-time setup'
    New-Item -ItemType Directory -Force -Path $R | Out-Null
    if (-not (Test-Path "$RT\.git")) { & $Git -C $RecompSrc worktree add -q --detach $RT HEAD }
    cmake -S $RecompSrc -B "$RecompSrc\build-win" -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF *> "$R\tools_configure.log"
    if ($LASTEXITCODE) { 'TOOLS CONFIGURE FAILED'; Get-Content "$R\tools_configure.log" -Tail 20; exit 1 }
    cmake -S $RT -B "$RT\build-win" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RECOMP=OFF `
        -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF *> "$R\runtime_configure.log"
    if ($LASTEXITCODE) { 'RUNTIME CONFIGURE FAILED'; Get-Content "$R\runtime_configure.log" -Tail 20; exit 1 }
}

if (-not $Fast) {
    $branch = & $Git -C $RecompSrc rev-parse --abbrev-ref HEAD
    $commit = & $Git -C $RecompSrc rev-parse --short HEAD
    Stage "PS2Recomp tools ($branch $commit)"
    cmake --build "$RecompSrc\build-win" --target ps2_recomp ps2_analyzer *> "$R\tools_build.log"
    if ($LASTEXITCODE) { 'TOOLS BUILD FAILED'; Select-String -Path "$R\tools_build.log" -Pattern 'error' | Select-Object -First 10; exit 1 }
    & $Git -C $RT checkout -q --detach (& $Git -C $RecompSrc rev-parse HEAD)
    "runtime worktree at $(& $Git -C $RT rev-parse --short HEAD)"

    Stage 'Ghidra cleanup'
    Run-Ghidra "$Root\game\ghidra\headless_cleanup.log" @('-scriptPath', "$Root\tools\ghidra", '-postScript', 'PoPMarkDataRanges.java')
    Show-ScriptLines "$Root\game\ghidra\headless_cleanup.log" 'PoPMarkDataRanges.java'

    Stage 'SCE SDK signature names'
    New-Item -ItemType Directory -Force -Path "$R\analyzer" | Out-Null
    & "$RecompSrc\build-win\ps2xAnalyzer\ps2_analyzer.exe" "$Root\game\ps2\disc\SLUS_207.43" "$R\analyzer\analyzer.toml" *> "$R\analyzer\analyzer_run.log"
    $names = Select-String -Path "$R\analyzer\analyzer.toml" -Pattern '"([A-Za-z0-9_]+)@(0x[0-9A-Fa-f]+)"' -AllMatches |
             ForEach-Object { $_.Matches } | ForEach-Object { '{0},{1}' -f $_.Groups[1].Value, $_.Groups[2].Value } | Sort-Object -Unique
    $names | Set-Content -Encoding ascii "$R\analyzer\sce_names.csv"
    "$($names.Count) names from ps2_analyzer"
    Run-Ghidra "$Root\game\ghidra\headless_names.log" @('-scriptPath', "$Root\tools\ghidra", '-postScript', 'PoPApplyNames.java', "$R\analyzer\sce_names.csv")
    Show-ScriptLines "$Root\game\ghidra\headless_names.log" 'PoPApplyNames.java'

    Stage 'Export'
    Run-Ghidra "$Root\game\ghidra\headless_export.log" @('-readOnly', '-scriptPath', "$RecompSrc\ps2xRecomp\tools\ghidra",
        '-postScript', 'ExportPS2Functions.java', "$R\config.toml", "$R\functions.csv")
    Show-ScriptLines "$Root\game\ghidra\headless_export.log" 'ExportPS2Functions.java' | Select-String 'Exported \d'

    Stage 'Recompile'
    if (Test-Path "$R\output") { Remove-Item -Recurse -Force "$R\output" }
    & "$RecompSrc\build-win\ps2xRecomp\ps2_recomp.exe" "$R\config.toml" *> "$R\recomp_run.log"
    $errors = (Select-String -Path "$R\recomp_run.log" -Pattern '\[error\]').Count
    $warnings = (Select-String -Path "$R\recomp_run.log" -Pattern '\[warning\]').Count
    "errors=$errors warnings=$warnings"

    Stage 'Sync generated code into runtime worktree'
    $overrides = Get-ChildItem "$Root\tools\overrides\*.cpp" | ForEach-Object Name
    & $Python -I "$Root\tools\sync_generated.py" "$R\output" "$RT\ps2xRuntime\src\runner" '*.cpp' --delete --keep @overrides
    & $Python -I "$Root\tools\sync_generated.py" "$R\output" "$RT\ps2xRuntime\include" '*.h'
}

Stage 'Sync game overrides'
# Overrides must be in the executable's sources, not the runtime library (upstream #264).
& $Python -I "$Root\tools\sync_generated.py" "$Root\tools\overrides" "$RT\ps2xRuntime\src\runner" '*.cpp'

Stage 'Build runtime'
cmake --build "$RT\build-win" *> "$R\runtime_build.log"
if ($LASTEXITCODE) { 'BUILD FAILED'; Select-String -Path "$R\runtime_build.log" -Pattern 'error' | Select-Object -First 10; exit 1 }
"build ok, errors=$((Select-String -Path "$R\runtime_build.log" -Pattern ': error').Count)"
Stage 'Done'
