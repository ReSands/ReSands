# PC job (18 Oct): compile the VU1 microcode the slow scenes run on the interpreter.
# The profiles of 18 Oct (game\analysis\prof\*.report.txt) show the VU1 thread busy in the
# cycle-modelling interpreter in the slow saves: compiled VU1 programs are looked up by the whole
# VU1 code image, and the compiled set came from a capture of the opening (9 images, 12 Oct), so
# scenes with other program combinations fall back to the interpreter.
#   0. runtime patch 29 (frame-rate overlay layout) if missing;
#   1. capture: every kept save loaded for a short boot with PS2X_VU1_CAPTURE into the existing
#      capture folder (game\recomp-sotc\vu1_capture; images are added, not replaced);
#   2. rebuild.ps1 -Fast -Play -Vu1 <capture>: vu1_recomp over all images, play build;
#   3. measure: the fps of a 20 s boot of every save (game\analysis\stateshots\fps_after_vu1.txt,
#      to compare with the job log of state-shots.ps1, 17 Oct), then profile-states.ps1 -NoBuild.
# Run through start-job.ps1 (the window needs the desktop session).
param([string]$Pattern = 'play_*.state', [int]$CaptureSeconds = 16, [switch]$NoCapture)
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
# runtime patches not applied yet (29: the frame-rate overlay in Digital Foundry's layout)
$Rt = 'C:\tools\PS2Recomp-sotc'
foreach ($p in @(@{ N = 29; File = '29-frame-graph-df-layout.patch'; Mark = "Frame-rate overlay in Digital Foundry's layout" })) {
    if (-not (git -C $Rt log --oneline -20 | Select-String -SimpleMatch $p.Mark)) {
        "== applying runtime patch $($p.N)"
        git -C $Rt -c user.name=Jon -c user.email=resands@proton.me am "$Root\tools\patches\pop-sotc\$($p.File)"
        if ($LASTEXITCODE -ne 0) { git -C $Rt am --abort; "PATCH $($p.N) FAILED"; exit 1 }
    }
}
git -C "$Root\game\recomp-sotc\runtime-src" checkout -q --detach (git -C $Rt rev-parse HEAD)
$capture = "$Root\game\recomp-sotc\vu1_capture"
$states = Get-ChildItem "$Root\game\ps2\disc\states\$Pattern" | Sort-Object Name
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') capture: $((Get-ChildItem "$capture\vu1_*.bin").Count) images before"
if (-not $NoCapture) {
    foreach ($st in $states) {
        & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds $CaptureSeconds -Name "vu1cap" `
            -Env "PS2X_STATE_LOAD_AT=120:states/$($st.Name);POP_60FPS=1;PS2X_VU1_CAPTURE=$capture" | Out-Null
        "$($st.BaseName): $((Get-ChildItem "$capture\vu1_*.bin").Count) images"
    }
}
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') rebuild with the compiled VU1 microcode"
& "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc -Vu1 $capture
if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') fps of every save"
$fpsOut = "$Root\game\analysis\stateshots\fps_after_vu1.txt"
Remove-Item $fpsOut -ErrorAction SilentlyContinue
foreach ($st in $states) {
    & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 20 -Name "fpscheck" `
        -Env "PS2X_STATE_LOAD_AT=120:states/$($st.Name);POP_60FPS=1" | Out-Null
    $fps = (Select-String -Path "$Root\game\recomp-sotc\fpscheck.log" -Pattern '\[pop:fps\] ([\d.]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ' '
    $line = "$($st.BaseName) fps $fps"
    $line
    Add-Content -Path $fpsOut -Value $line
}
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') profiles"
& "$Root\tools\windows\profile-states.ps1" -NoBuild
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done"
