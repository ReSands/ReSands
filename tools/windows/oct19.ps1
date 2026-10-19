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
# Run through start-job.ps1 (the window needs the desktop session).
param([switch]$NoBuild, [switch]$NoMeasure, [string]$Pattern = 'play_*.state')
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$Rt = 'C:\tools\PS2Recomp-sotc'
if (-not (git -C $Rt log --oneline -20 | Select-String -SimpleMatch 'vu1_recomp: pairs past the last non-zero')) {
    "== applying runtime patch 35"
    git -C $Rt -c user.name=Jon -c user.email=resands@proton.me am "$Root\tools\patches\pop-sotc\35-vu1-recomp-padding-to-interpreter.patch"
    if ($LASTEXITCODE -ne 0) { git -C $Rt am --abort; "PATCH 35 FAILED"; exit 1 }
}
git -C "$Root\game\recomp-sotc\runtime-src" checkout -q --detach (git -C $Rt rev-parse HEAD)

$progs = "$Root\game\recomp-sotc\vu_progs"
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') VU programs from the ELF"
Remove-Item -Recurse -Force $progs -ErrorAction SilentlyContinue
python -I "$Root\tools\vu1progs.py" "$Root\game\ps2\disc\SLUS_207.43" $progs --check "$Root\game\recomp-sotc\vu1_capture" |
    Where-Object { $_ -notlike '*MPG-like word*' }
if ($LASTEXITCODE -ne 0) { "VU PROGRAMS FAILED"; exit 1 }
"$((Get-ChildItem "$progs\vu1_*.bin").Count) VU1 and $((Get-ChildItem "$progs\vu0_*.bin").Count) VU0 images"

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
