# PC job (19 Oct): compile PoP's VU microprograms straight from the ELF instead of recorded images.
# The death save of 19 Oct (play_20261019_082854, 39-43 fps) ran a VU1 image nobody had recorded: every
# image is one of 10 programs in the ELF plus the tail an earlier program left (tools/vu1progs.py), and
# compiled code was looked up by the whole image. The runtime also matches a program's zero-padded prefix
# (patch 33), so one compiled copy of each program covers every combination. Steps:
#   1. runtime patch 35 (vu1_recomp: pairs past a program's end go to the interpreter);
#   2. tools/vu1progs.py: the programs from the ELF -> game\recomp-sotc\vu_progs;
#   3. rebuild.ps1 -Fast -Play -Vu1 vu_progs;
#   4. verify boots (PS2X_VU1_VERIFY=1, PS2X_VU0_VERIFY=1) on the death save, the fight and gameplay1;
#   5. profile of the death save; fps of every save (fps_progs.txt, compare fps_final.txt).
# -Experiments: cost of the parts build-time scheduling would remove. Builds the measurement-only variants of
# runtime patch 36 (VU1R_EXPERIMENT=noflags / nostall / both: NOT exact, never for play), measures the slowest
# saves with each (game\analysis\stateshots\fps_exp_<variant>.txt, 'exact' = the normal build), then rebuilds
# the exact play build.
# -Sched: build-time scheduled VU1 programs (runtime patch 37, VU1R_SCHED=1): verify boots, then fps of the
# slowest saves with the scheduled code on and off in the same build (PS2X_VU1_SCHED=0), fps_sched_<on|off>.txt.
# -Native (night of 19 Oct, back to the native renderer): what drawing natively would save. Fast build with
# POP_NATIVE_SKIP (tools/overrides/pop_native_live.cpp: the covered draw paths return without building DMA;
# measurement only, the picture is wrong), fps of the fight and slow saves per skip group
# (game\analysis\native\prize\fps_<group>.txt), profiles of the worst fight with nothing and everything
# skipped (game\analysis\prof\prize_<group>.*), then VU1 traces (PS2X_VU1_TRACE) of two water saves for the
# water program P6 (game\analysis\vu1trace\<save>.trace + the vu1_<hash>.bin images): inputs for the port.
# -World (night of 19-20 Oct, renderer step 1: POP_NATIVE_WORLD): build and offline checks only, no boots
# (the TV is off). Runtime patch 38 (GS host-inject hook, runner extra libraries) into the sotc checkout
# and the runtime worktree, a fast play build that links the native renderer (rebuild.ps1 builds
# game\native-build's pop_native_render + volk), then on the fight save: pop_state_frame --world (the
# native draws), pop_world_test (the runtime side with a stand-in GS: Vulkan on this GPU, timings) and
# the viewer's --gsz for the comparison (game\analysis\native\world\).
# Run through start-job.ps1 (the window needs the desktop session).
param([switch]$NoBuild, [switch]$NoMeasure, [string]$Pattern = 'play_*.state', [switch]$Experiments, [switch]$Sched,
      [switch]$Native, [switch]$TracesOnly, [switch]$World)
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$Rt = 'C:\tools\PS2Recomp-sotc'

if ($World) {
    if (-not (git -C $Rt log --oneline -20 | Select-String -SimpleMatch 'host-inject hook')) {
        "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') runtime patch 38"
        git -C $Rt -c user.name=Jon -c user.email=resands@proton.me am "$Root\tools\patches\pop-sotc\38-gs-host-inject-hook-runner-extra-libs.patch"
        if ($LASTEXITCODE -ne 0) { git -C $Rt am --abort; "PATCH 38 FAILED"; exit 1 }
    }
    git -C "$Root\game\recomp-sotc\runtime-src" checkout -q --detach (git -C $Rt rev-parse HEAD)
    "runtime worktree at $(git -C "$Root\game\recomp-sotc\runtime-src" log --oneline -1)"
    if (-not $NoBuild) {
        "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') fast play build (POP_NATIVE_WORLD, native renderer linked)"
        & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc $Rt -Out game\recomp-sotc
        if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
    }
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') offline checks (pop_state_frame --world, pop_world_test)"
    # the compiler environment (rebuild.ps1 sets up its own)
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    $NB = "$Root\game\native-build"
    $out = "$Root\game\analysis\native\world"
    New-Item -ItemType Directory -Force $out | Out-Null
    cmake -S "$Root\tools\native" -B $NB "-DPOP_SOTC_SRC=$Rt" "-DPOP_RUNTIME_SRC=$Root\game\recomp-sotc\runtime-src" > "$out\configure.log" 2>&1
    cmake --build $NB --target pop_state_frame pop_world_test pop_native_view > "$out\build.log" 2>&1
    if ($LASTEXITCODE -ne 0) { "NATIVE TOOLS BUILD FAILED"; Select-String -Path "$out\build.log" -Pattern 'error' | Select-Object -First 10; exit 1 }
    Set-Location "$Root\game\ps2\disc"
    $st = 'states\play_20261019_185602.state'
    $env:POP_STATE_WORLD_SECOND = '1' # also the next frame (batches by key) for pop_world_test
    & "$NB\pop_state_frame.exe" $st "$out\fight.world.pnf" --world --bigfile DATA\PRINCE.BF --packindex "$Root\game\analysis\packindex.txt" > "$out\state_frame.log" 2>&1
    Select-String -Path "$out\state_frame.log" -Pattern 'world frame|packs loaded'
    '1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1' | Set-Content "$out\identity.clip"
    & "$NB\pop_native_view.exe" "$out\fight.world.pnf" "$out\identity.clip" "$out\fight.world.png" --frame --gsz "$out\fight.world.gsz" 2>&1 | Select-Object -Last 1
    & "$NB\pop_world_test.exe" "$out\fight.world.pnf" "$out\fight.test" 60 2>&1 | Tee-Object "$out\world_test.log" | Select-Object -Last 4
    $same = (Get-FileHash "$out\fight.test.gsz").Hash -eq (Get-FileHash "$out\fight.world.gsz").Hash
    "pop_world_test GS Z identical to the viewer's: $same"
    Set-Location $Root
    exit 0
}

# every other mode boots the game: with the TV off, OpenGL Present stalls every boot to ~4 fps (19 Oct)
if ((nvidia-smi --query-gpu=display_active --format=csv,noheader) -notmatch 'Enabled') { "NO ACTIVE DISPLAY (switch the TV on): FAILED"; exit 1 }

if ($Native) {
    if (-not $NoBuild) {
        "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') fast play build (override: POP_NATIVE_SKIP)"
        & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc $Rt -Out game\recomp-sotc
        if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
    }
    $prize = "$Root\game\analysis\native\prize"
    if ($TracesOnly) { $groupsToRun = @() } else { $groupsToRun = @('none', 'static', 'skinned', 'lightmap', 'gfx', 'all') }
    New-Item -ItemType Directory -Force $prize | Out-Null
    $states = @('play_20261019_185602', 'play_20261019_185837', 'play_20261019_082854', 'play_20261017_093051',
                'play_20261018_202311', 'play_20261017_093011', 'play_20261018_202222', 'play_20261018_214002')
    foreach ($group in $groupsToRun) {
        "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') fps, skipping: $group"
        $out = "$prize\fps_$group.txt"
        Remove-Item $out -ErrorAction SilentlyContinue
        $skip = $(if ($group -eq 'none') { '' } else { ";POP_NATIVE_SKIP=$group" })
        foreach ($st in $states) {
            & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 30 -Name "prize" `
                -Env "PS2X_STATE_LOAD_AT=120:states/$st.state;POP_60FPS=1$skip" | Out-Null
            $log = "$Root\game\recomp-sotc\prize.log"
            $fps = (Select-String -Path $log -Pattern '\[pop:fps\] ([\d.]+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ' '
            $counts = Select-String -Path $log -Pattern 'skipped/kept' | Select-Object -Last 1 | ForEach-Object { $_.Line -replace '.*kept: ', '' }
            $line = "$group $st fps $fps | $counts"
            $line
            Add-Content -Path $out -Value $line
        }
    }
    $map = "$Root\game\recomp-sotc\runtime-src\build-win-play\ps2xRuntime\ps2EntryRunner.map"
    foreach ($group in $(if ($TracesOnly) { @() } else { @('none', 'all') })) {
        "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') profile of the fight, skipping: $group"
        $prof = "$Root\game\analysis\prof\prize_$group.txt"
        Remove-Item $prof -ErrorAction SilentlyContinue
        $skip = $(if ($group -eq 'none') { '' } else { ";POP_NATIVE_SKIP=$group" })
        & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 35 -Name "prizeprof" `
            -Env "PS2X_STATE_LOAD_AT=120:states/play_20261019_185602.state;POP_60FPS=1;POP_PROFILE=$($prof):8:20$skip" | Out-Null
        if (Test-Path $prof) {
            python -I "$Root\tools\profsym.py" $prof $map --top 30 | Set-Content -Encoding utf8 "$Root\game\analysis\prof\prize_$group.report.txt"
            Get-Content "$Root\game\analysis\prof\prize_$group.report.txt" -TotalCount 30
        } else { "no profile written" }
    }
    # VU1 traces: the trace counts GS fields, which a save load restores to the save's own field (popstate.py
    # prints it as 'tick') and which advance only with presented frames; save field + 400 = ~7 s after the
    # load at 60 fps (a window counted from boot has passed by then: empty traces, 19 Oct)
    $trace = "$Root\game\analysis\vu1trace"
    New-Item -ItemType Directory -Force $trace | Out-Null
    foreach ($s in @(@{ State = 'play_20261017_091049'; Tick = 15746 }, @{ State = 'play_20261017_091734'; Tick = 37719 })) {
        foreach ($first in @(($s.Tick + 400))) {
            "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') VU1 trace of $($s.State) from field $first"
            $file = "$trace\$($s.State).trace"
            Remove-Item $file -ErrorAction SilentlyContinue
            & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 30 -Name "vu1trace" `
                -Env "PS2X_STATE_LOAD_AT=120:states/$($s.State).state;POP_60FPS=1;PS2X_VU1_TRACE=$($file):$($first):6" | Out-Null
            Select-String -Path "$Root\game\recomp-sotc\vu1trace.log" -Pattern '\[vu1-trace\]' | ForEach-Object Line
            $size = $(if (Test-Path $file) { (Get-Item $file).Length } else { 0 })
            "$($s.State).trace: $size bytes"
            if ($size -gt 100000) { break }
        }
    }
    Get-ChildItem "$trace\vu1_*.bin" | ForEach-Object Name
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done (native prize + water traces)"
    exit 0
}
if (-not (git -C $Rt log --oneline -20 | Select-String -SimpleMatch 'vu1_recomp: pairs past the last non-zero')) {
    "== applying runtime patch 35"
    git -C $Rt -c user.name=Jon -c user.email=resands@proton.me am "$Root\tools\patches\pop-sotc\35-vu1-recomp-padding-to-interpreter.patch"
    if ($LASTEXITCODE -ne 0) { git -C $Rt am --abort; "PATCH 35 FAILED"; exit 1 }
}
if (($Experiments -or $Sched) -and -not (git -C $Rt log --oneline -20 | Select-String -SimpleMatch 'measurement-only switches')) {
    "== applying runtime patch 36"
    git -C $Rt -c user.name=Jon -c user.email=resands@proton.me am "$Root\tools\patches\pop-sotc\36-vu1-recomp-measurement-switches.patch"
    if ($LASTEXITCODE -ne 0) { git -C $Rt am --abort; "PATCH 36 FAILED"; exit 1 }
}
if ($Sched -and -not (git -C $Rt log --oneline -20 | Select-String -SimpleMatch 'build-time scheduled VU1 programs')) {
    "== applying runtime patch 37"
    git -C $Rt -c user.name=Jon -c user.email=resands@proton.me am "$Root\tools\patches\pop-sotc\37-vu1-recomp-build-time-scheduling.patch"
    if ($LASTEXITCODE -ne 0) { git -C $Rt am --abort; "PATCH 37 FAILED"; exit 1 }
}
git -C "$Root\game\recomp-sotc\runtime-src" checkout -q --detach (git -C $Rt rev-parse HEAD)

$progs = "$Root\game\recomp-sotc\vu_progs"
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') VU programs from the ELF"
Remove-Item -Recurse -Force $progs -ErrorAction SilentlyContinue
python -I "$Root\tools\vu1progs.py" "$Root\game\ps2\disc\SLUS_207.43" $progs --check "$Root\game\recomp-sotc\vu1_capture" |
    Where-Object { $_ -notlike '*MPG-like word*' }
if ($LASTEXITCODE -ne 0) { "VU PROGRAMS FAILED"; exit 1 }
"$((Get-ChildItem "$progs\vu1_*.bin").Count) VU1 and $((Get-ChildItem "$progs\vu0_*.bin").Count) VU0 images"

if ($Sched) {
    $slow = @('play_20261017_093051', 'play_20261018_202311', 'play_20261017_093011', 'play_20261018_202222', 'play_20261018_214002',
              'play_20261017_093100', 'play_20261018_213954', 'play_20261017_092959', 'play_20261019_082854')
    if (-not $NoBuild) {
        "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') rebuild with the scheduled programs"
        $env:VU1R_SCHED = '1'
        & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc $Rt -Out game\recomp-sotc -Vu1 $progs
        $code = $LASTEXITCODE
        Remove-Item Env:VU1R_SCHED -ErrorAction SilentlyContinue
        if ($code -ne 0) { "BUILD FAILED ($code)"; exit 1 }
        Select-String -Path "$Root\game\recomp-sotc\vu1_recomp.log" -Pattern 'sched' | ForEach-Object Line
    }
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') verify: scheduled VU1 runs against the interpreter"
    foreach ($st in @('play_20261019_082854', 'play_20261017_093051', 'gameplay1')) {
        & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 25 -Name "vuverify" `
            -Env "PS2X_STATE_LOAD_AT=120:states/$st.state;POP_60FPS=1;PS2X_VU1_VERIFY=1;PS2X_VU0_VERIFY=1" | Out-Null
        $v = Select-String -Path "$Root\game\recomp-sotc\vuverify.log" -Pattern '\[vu[01]-verify\]' | ForEach-Object Line
        "$st : $(@($v | Where-Object { $_ -like '*mismatch #*' }).Count) mismatch lines; last lines:"
        $v | Select-Object -Last 2
        $v | Where-Object { $_ -like '*mismatch #*' } | Select-Object -First 12
    }
    if ($NoMeasure) { "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done (no measuring)"; exit 0 }
    foreach ($mode in @('on', 'off')) {
        $out = "$Root\game\analysis\stateshots\fps_sched_$mode.txt"
        Remove-Item $out -ErrorAction SilentlyContinue
        $flag = $(if ($mode -eq 'off') { ';PS2X_VU1_SCHED=0' } else { '' })
        foreach ($st in $slow) {
            & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 30 -Name "fpssched" `
                -Env "PS2X_STATE_LOAD_AT=120:states/$st.state;POP_60FPS=1$flag" | Out-Null
            $fps = (Select-String -Path "$Root\game\recomp-sotc\fpssched.log" -Pattern '\[pop:fps\] ([\d.]+)' |
                    ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ' '
            $line = "$mode $st fps $fps"
            $line
            Add-Content -Path $out -Value $line
        }
    }
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done (scheduled build in place)"
    exit 0
}

if ($Experiments) {
    $slow = @('play_20261017_093051', 'play_20261018_202311', 'play_20261017_093011', 'play_20261018_202222', 'play_20261018_214002',
              'play_20261017_093100', 'play_20261018_213954', 'play_20261017_092959', 'play_20261019_082854')
    foreach ($variant in @('exact', 'noflags', 'nostall', 'noflags,nostall', 'final')) {
        if ($variant -ne 'exact') {
            "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') build variant $variant"
            $env:VU1R_EXPERIMENT = $(if ($variant -eq 'final') { '' } else { $variant })
            & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc $Rt -Out game\recomp-sotc -Vu1 $progs
            $code = $LASTEXITCODE
            Remove-Item Env:VU1R_EXPERIMENT -ErrorAction SilentlyContinue
            if ($code -ne 0) { "BUILD FAILED ($code)"; exit 1 }
            Select-String -Path "$Root\game\recomp-sotc\vu1_recomp.log" -Pattern 'EXPERIMENT' | ForEach-Object Line
        }
        if ($variant -eq 'final') { break }
        $out = "$Root\game\analysis\stateshots\fps_exp_$($variant -replace ',', '_').txt"
        Remove-Item $out -ErrorAction SilentlyContinue
        foreach ($st in $slow) {
            & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 30 -Name "fpsexp" `
                -Env "PS2X_STATE_LOAD_AT=120:states/$st.state;POP_60FPS=1" | Out-Null
            $fps = (Select-String -Path "$Root\game\recomp-sotc\fpsexp.log" -Pattern '\[pop:fps\] ([\d.]+)' |
                    ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ' '
            $line = "$variant $st fps $fps"
            $line
            Add-Content -Path $out -Value $line
        }
    }
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done (experiments; the exact build is back)"
    exit 0
}

if (-not $NoBuild) {
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') rebuild with the programs compiled"
    & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc $Rt -Out game\recomp-sotc -Vu1 $progs
    if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
    Get-Content "$Root\game\recomp-sotc\vu1_recomp.log" -Tail 8
}

"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') verify: compiled VU1/VU0 runs against the interpreter"
foreach ($st in @('play_20261019_082854', 'play_20261017_093051', 'gameplay1')) {
    & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 25 -Name "vuverify" `
        -Env "PS2X_STATE_LOAD_AT=120:states/$st.state;POP_60FPS=1;PS2X_VU1_VERIFY=1;PS2X_VU0_VERIFY=1" | Out-Null
    $v = Select-String -Path "$Root\game\recomp-sotc\vuverify.log" -Pattern '\[vu[01]-verify\]' | ForEach-Object Line
    "$st : $(@($v | Where-Object { $_ -like '*mismatch #*' }).Count) mismatch lines; last lines:"
    $v | Select-Object -Last 2
    $v | Where-Object { $_ -like '*mismatch #*' } | Select-Object -First 3
}
if ($NoMeasure) { "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done (no measuring)"; exit 0 }

"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') profile of the death save"
& "$Root\tools\windows\profile-states.ps1" -NoBuild -States play_20261019_082854 -Start 3 -Duration 20

"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') fps of every save"
$fpsOut = "$Root\game\analysis\stateshots\fps_progs.txt"
Remove-Item $fpsOut -ErrorAction SilentlyContinue
foreach ($st in Get-ChildItem "$Root\game\ps2\disc\states\$Pattern" | Sort-Object Name) {
    & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 20 -Name "fpscheck" `
        -Env "PS2X_STATE_LOAD_AT=120:states/$($st.Name);POP_60FPS=1" | Out-Null
    $fps = (Select-String -Path "$Root\game\recomp-sotc\fpscheck.log" -Pattern '\[pop:fps\] ([\d.]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ' '
    $line = "$($st.BaseName) fps $fps"
    $line
    Add-Content -Path $fpsOut -Value $line
}
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done"
