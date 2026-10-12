# Windows version of tools/rebuild.sh: Ghidra cleanup -> SDK names -> export ->
# recompile -> sync into runtime worktree -> MSVC build. Logs in game\ghidra and game\recomp.
#
# Usage: tools\windows\rebuild.ps1          full pipeline
#        tools\windows\rebuild.ps1 -Fast    overrides/runtime only (skips Ghidra and the recompile)
#        tools\windows\rebuild.ps1 -Setup   one-time: runtime worktree + CMake configure, then full
#        add -Play to (also) build the log-free play build in build-win-play (configured on first
#        use; PS2Recomp's verbose runtime logging costs about a third of the frame rate)
#        add -Release with -Play for a Release build (/GL + LTCG, long link) in build-win-play-rel
#        -RecompSrc / -Out select another PS2Recomp checkout and output folder, e.g.
#        -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc (the pop-sotc setup)
param([switch]$Fast, [switch]$Setup, [switch]$Play, [switch]$Release,
      [string]$RecompSrc = 'C:\tools\PS2Recomp', [string]$Out = 'game\recomp', [string]$Vu1 = '')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root      = (Resolve-Path "$PSScriptRoot\..\..").Path
$Tools     = 'C:\tools'
$Ghidra    = "$Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat"
$R         = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { "$Root\$Out" }
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
# Stops the MSVC tools starting vctip.exe (telemetry), which lingers after builds holding
# inherited handles to our log files.
$env:VSCMD_SKIP_SENDTELEMETRY = '1'

Set-Location $Root
$sw = [Diagnostics.Stopwatch]::StartNew()
function Stage($name) {
    if ($sw.Elapsed.TotalSeconds -ge 1) { '   ({0:N0}s)' -f $sw.Elapsed.TotalSeconds }
    $sw.Restart()
    "== $name"
}
# Runs a program with stdout then stderr in a log and returns its exit code.
# Not via PowerShell redirection (in 5.1, under 'Stop', any stderr line from a native
# program is a terminating error) and not via cmd.exe redirection (Ghidra's .bat
# launchers parse %CMDCMDLINE% and break on '>' or '2>&1' in it).
# Every call has a time limit, so a hung program (e.g. Ghidra's launcher waiting at
# "Press any key" after an error) fails the run instead of stalling it silently.
function Invoke-Logged($log, $exe, [string[]]$argList, [int]$TimeoutMinutes = 30) {
    $quoted = $argList | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $p = Start-Process -FilePath $exe -ArgumentList ($quoted -join ' ') -NoNewWindow -PassThru `
            -RedirectStandardOutput "$log.out" -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutMinutes * 60 * 1000)) {
        taskkill /T /F /PID $p.Id 2>&1 | Out-Null
        "TIMEOUT after $TimeoutMinutes min: $exe (see $log.out)"
        exit 1
    }
    # No untimed WaitForExit() here: it also waits for the redirected files to be released,
    # and a lingering child that inherited them (MSVC's vctip.exe) made it hang.
    # A child the program started (Ghidra's launcher starts java) can still hold the
    # redirected files for a moment after it exits, so retry instead of failing the run.
    for ($i = 0; $i -lt 20; $i++) {
        try {
            Get-Content "$log.out", "$log.err" -ErrorAction Stop | Set-Content $log -ErrorAction Stop
            Remove-Item "$log.out", "$log.err" -ErrorAction Stop
            break
        } catch { Start-Sleep -Milliseconds 500 }
    }
    return $p.ExitCode
}
function Run-Ghidra($log, [string[]]$scriptArgs) {
    $null = Invoke-Logged $log $Ghidra (@("$Root\game\ghidra", 'PoP_SLUS20743', '-process', 'SLUS_207.43', '-noanalysis') + $scriptArgs) -TimeoutMinutes 10
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
    if (Invoke-Logged "$R\tools_configure.log" cmake @('-S', $RecompSrc, '-B', "$RecompSrc\build-win", '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DPS2X_BUILD_RUNTIME=OFF', '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF')) {
        'TOOLS CONFIGURE FAILED'; Get-Content "$R\tools_configure.log" -Tail 20; exit 1
    }
    # Build type "Dev": /O2 per file, but none of PS2Recomp's Release extras (/GL + /LTCG
    # whole-program optimisation, applied only to Release/RelWithDebInfo). With ~73k generated
    # functions the LTCG link alone ran for over 20 minutes on the 5800X. Use Release for
    # playable builds.
    $devFlags = '/O2 /Ob2 /DNDEBUG'
    if (Invoke-Logged "$R\runtime_configure.log" cmake @('-S', $RT, '-B', "$RT\build-win", '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Dev', "-DCMAKE_CXX_FLAGS_DEV=$devFlags", "-DCMAKE_C_FLAGS_DEV=$devFlags",
            '-DCMAKE_EXE_LINKER_FLAGS_DEV=/INCREMENTAL:NO', '-DCMAKE_SHARED_LINKER_FLAGS_DEV=',
            '-DCMAKE_MODULE_LINKER_FLAGS_DEV=', '-DCMAKE_STATIC_LINKER_FLAGS_DEV=',
            '-DPS2X_BUILD_RECOMP=OFF', '-DPS2X_BUILD_ANALYZER=OFF',
            '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF')) {
        'RUNTIME CONFIGURE FAILED'; Get-Content "$R\runtime_configure.log" -Tail 20; exit 1
    }
}

if (-not $Fast) {
    $branch = & $Git -C $RecompSrc rev-parse --abbrev-ref HEAD
    $commit = & $Git -C $RecompSrc rev-parse --short HEAD
    Stage "PS2Recomp tools ($branch $commit)"
    if (Invoke-Logged "$R\tools_build.log" cmake @('--build', "$RecompSrc\build-win", '--target', 'ps2_recomp', 'ps2_analyzer')) { 'TOOLS BUILD FAILED'; Select-String -Path "$R\tools_build.log" -Pattern 'error' | Select-Object -First 10; exit 1 }
    & $Git -C $RT checkout -q --detach (& $Git -C $RecompSrc rev-parse HEAD)
    "runtime worktree at $(& $Git -C $RT rev-parse --short HEAD)"

    Stage 'Ghidra cleanup'
    Run-Ghidra "$Root\game\ghidra\headless_cleanup.log" @('-scriptPath', "$Root\tools\ghidra", '-postScript', 'PoPMarkDataRanges.java')
    Show-ScriptLines "$Root\game\ghidra\headless_cleanup.log" 'PoPMarkDataRanges.java'

    Stage 'SCE SDK signature names'
    New-Item -ItemType Directory -Force -Path "$R\analyzer" | Out-Null
    $null = Invoke-Logged "$R\analyzer\analyzer_run.log" "$RecompSrc\build-win\ps2xAnalyzer\ps2_analyzer.exe" @("$Root\game\ps2\disc\SLUS_207.43", "$R\analyzer\analyzer.toml") -TimeoutMinutes 10
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
    # The Ghidra project was imported on the Mac, so the export writes the Mac path to the ELF.
    $elf = ("$Root\game\ps2\disc\SLUS_207.43" -replace '\\', '/')
    (Get-Content "$R\config.toml") -replace '^input = .*$', "input = `"$elf`"" | Set-Content -Encoding ascii "$R\config.toml"
    # Recompile the game's own libvu0 instead of PS2Recomp's C++ stand-ins (several are wrong;
    # see rebuild.sh). The recompiler also stubs by name, so rename the functions as well.
    (Get-Content "$R\config.toml") | Where-Object { $_ -notmatch '^  "sceVu0[A-Za-z0-9]*@0x[0-9A-F]*",$' } |
        ForEach-Object { $_ -replace '\b_sceVu0', 'pop__sceVu0' -replace '\bsceVu0', 'pop_sceVu0' } |
        Set-Content -Encoding ascii "$R\config.toml"
    (Get-Content "$R\functions.csv") | ForEach-Object { $_ -replace '\b_sceVu0', 'pop__sceVu0' -replace '\bsceVu0', 'pop_sceVu0' } |
        Set-Content -Encoding ascii "$R\functions.csv"
    "libvu0: $((Select-String -Path "$R\functions.csv" -Pattern '^pop_+sceVu0').Count) functions renamed"

    Stage 'Recompile'
    if (Test-Path "$R\output") { Remove-Item -Recurse -Force "$R\output" }
    $null = Invoke-Logged "$R\recomp_run.log" "$RecompSrc\build-win\ps2xRecomp\ps2_recomp.exe" @("$R\config.toml") -TimeoutMinutes 20
    $errors = (Select-String -Path "$R\recomp_run.log" -Pattern '\[error\]').Count
    $warnings = (Select-String -Path "$R\recomp_run.log" -Pattern '\[warning\]').Count
    "errors=$errors warnings=$warnings"
    if ($errors -or -not (Test-Path "$R\output\register_functions.cpp")) {
        "RECOMPILE FAILED, see $R\recomp_run.log"; Select-String -Path "$R\recomp_run.log" -Pattern '\[error\]' | Select-Object -First 5
        exit 1
    }

    Stage 'Sync generated code into runtime worktree'
    $overrides = Get-ChildItem "$Root\tools\overrides\*.cpp" | ForEach-Object Name
    & $Python -I "$Root\tools\sync_generated.py" "$R\output" "$RT\ps2xRuntime\src\runner" '*.cpp' --delete --keep @overrides
    & $Python -I "$Root\tools\sync_generated.py" "$R\output" "$RT\ps2xRuntime\include" '*.h'
}

Stage 'Sync game overrides'
# Overrides must be in the executable's sources, not the runtime library (upstream #264).
& $Python -I "$Root\tools\sync_generated.py" "$Root\tools\overrides" "$RT\ps2xRuntime\src\runner" '*.cpp'

$buildDir = "$RT\build-win"
if ($Play) {
    $buildDir = if ($Release) { "$RT\build-win-play-rel" } else { "$RT\build-win-play" }
    if (-not (Test-Path "$buildDir\build.ninja")) {
        Stage 'Configure play build'
        $devFlags = '/O2 /Ob2 /DNDEBUG'
        $typeArgs = if ($Release) { @('-DCMAKE_BUILD_TYPE=Release') } else {
            @('-DCMAKE_BUILD_TYPE=Dev', "-DCMAKE_CXX_FLAGS_DEV=$devFlags", "-DCMAKE_C_FLAGS_DEV=$devFlags",
              '-DCMAKE_EXE_LINKER_FLAGS_DEV=/INCREMENTAL:NO', '-DCMAKE_SHARED_LINKER_FLAGS_DEV=',
              '-DCMAKE_MODULE_LINKER_FLAGS_DEV=', '-DCMAKE_STATIC_LINKER_FLAGS_DEV=') }
        if (Invoke-Logged "$R\runtime_configure_play.log" cmake (@('-S', $RT, '-B', $buildDir, '-G', 'Ninja') + $typeArgs + @(
                '-DPS2X_BUILD_RECOMP=OFF', '-DPS2X_BUILD_ANALYZER=OFF', '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF',
                '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF', '-DPS2X_ENABLE_RUNTIME_LOGS=OFF', '-DPS2X_ENABLE_IOP_RPC_TRACE=OFF'))) {
            'PLAY CONFIGURE FAILED'; Get-Content "$R\runtime_configure_play.log" -Tail 20; exit 1
        }
    }
}
# -Vu1 <dir>: compile the VU1 microcode captured in <dir> (PS2X_VU1_CAPTURE, see boot.ps1 -Env) to C++
# with the runtime's vu1_recomp tool, into src\vu1_generated (the runner builds it if present; needs a
# PS2Recomp with that CMake hook, e.g. pop-sotc 77706cd+).
if ($Vu1) {
    Stage 'Compile captured VU1 microcode'
    & $Git -C $RT checkout -q --detach (& $Git -C $RecompSrc rev-parse HEAD)
    if (Invoke-Logged "$R\vu1_configure.log" cmake @('-S', $RT, '-B', $buildDir, '-DPS2X_BUILD_TEST=ON', '-DBUILD_TESTING=OFF')) { 'VU1 CONFIGURE FAILED'; exit 1 }
    if (Invoke-Logged "$R\vu1_tool_build.log" cmake @('--build', $buildDir, '--target', 'vu1_recomp')) { 'VU1 TOOL BUILD FAILED'; Select-String -Path "$R\vu1_tool_build.log.out" -Pattern 'error' | Select-Object -First 10; exit 1 }
    $tool = Get-ChildItem $buildDir -Recurse -Filter vu1_recomp.exe | Select-Object -First 1
    if (Invoke-Logged "$R\vu1_recomp.log" $tool.FullName @($Vu1, "$RT\ps2xRuntime\src\vu1_generated")) { 'VU1 RECOMP FAILED'; Get-Content "$R\vu1_recomp.log.err" -Tail 10; exit 1 }
    Get-Content "$R\vu1_recomp.log.out" -Tail 12
    "images: $((Get-ChildItem "$Vu1\vu1_*.bin").Count), generated files: $((Get-ChildItem "$RT\ps2xRuntime\src\vu1_generated\*.cpp").Count)"
    if (Invoke-Logged "$R\vu1_configure.log" cmake @('-S', $RT, '-B', $buildDir, '-DPS2X_BUILD_TEST=OFF')) { 'VU1 RECONFIGURE FAILED'; exit 1 }
}
Stage "Build runtime ($(Split-Path $buildDir -Leaf))"
if (Invoke-Logged "$R\runtime_build.log" cmake @('--build', $buildDir) -TimeoutMinutes 90) { 'BUILD FAILED'; Select-String -Path "$R\runtime_build.log" -Pattern 'error' | Select-Object -First 10; exit 1 }
"build ok, errors=$((Select-String -Path "$R\runtime_build.log" -Pattern ': error').Count)"
Stage 'Done'
