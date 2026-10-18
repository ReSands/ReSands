# PC job (18 Oct): compile the VU1 microcode the slow scenes run on the interpreter.
# The profiles of 18 Oct (game\analysis\prof\*.report.txt) show the VU1 thread busy in the
# cycle-modelling interpreter in the slow saves: compiled VU1 programs are looked up by the whole
# VU1 code image, and the compiled set came from a capture of the opening (9 images, 12 Oct), so
# scenes with other program combinations fall back to the interpreter.
#   0. runtime patches 29 (frame-rate overlay layout) and 30 (no pointer in fullscreen) if missing;
#   1. capture: every kept save loaded for a short boot with PS2X_VU1_CAPTURE into the existing
#      capture folder (game\recomp-sotc\vu1_capture; images are added, not replaced);
#   2. rebuild.ps1 -Fast -Play -Vu1 <capture>: vu1_recomp over all images, play build;
#   3. measure: the fps of a 20 s boot of every save (game\analysis\stateshots\fps_after_vu1.txt,
#      to compare with the job log of state-shots.ps1, 17 Oct), then profile-states.ps1 -NoBuild;
#   4. the nightly checks (llm-job.ps1 -Job nightly) on the new build (-Sleep: then the PC sleeps).
# -Vu0 (second run, same night): compiled VU0 microprograms (runtime patch 31) plus the upstream SotC
# runtime's 17 Oct hot-path commits (patches 32-34). No capture run: the VU
# code images come from the saves themselves (tools/vuimages.py; PoP uploads one VU0 image at boot,
# the same in all 84 saves), then the -Vu1 build, verify boots (PS2X_VU0_VERIFY=1: every compiled VU0
# run checked against the interpreter), and the measure/profile steps into *_after_vu0 files.
# Run through start-job.ps1 (the window needs the desktop session).
param([string]$Pattern = 'play_*.state', [int]$CaptureSeconds = 16, [switch]$NoCapture, [switch]$NoBuild,
      [switch]$NoMeasure, [switch]$NoNightly, [switch]$Sleep, [switch]$Vu0, [string[]]$ProfileStates = @())
# Quick run before more play: -NoMeasure -NoNightly. Measure/nightly only: -NoCapture -NoBuild.
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
# start-job.ps1 passes arguments as one string: 'a,b' arrives as a single element
$ProfileStates = @($ProfileStates | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
# runtime patches not applied yet (29: the frame-rate overlay in Digital Foundry's layout; 30: pointer)
$Rt = 'C:\tools\PS2Recomp-sotc'
foreach ($p in @(@{ N = 29; File = '29-frame-graph-df-layout.patch'; Mark = "Frame-rate overlay in Digital Foundry's layout" },
                 @{ N = 30; File = '30-hide-pointer-fullscreen.patch'; Mark = 'Hide the mouse pointer in fullscreen' },
                 @{ N = 31; File = '31-vu0-compiled-microprograms.patch'; Mark = 'Compiled VU0 microprograms'; Vu0 = $true },
                 # upstream sotc-port of 17 Oct (TaylorNAlbarnaz/PS2Recomp 29d26f9, 1729ab6, b4b8ea3)
                 @{ N = 32; File = '32-upstream-headless-skip-identical-output.patch'; Mark = 'Add headless mode and skip-identical'; Vu0 = $true },
                 @{ N = 33; File = '33-upstream-vu1-zero-padded-prefix-match.patch'; Mark = 'Match zero-padded VU1'; Vu0 = $true },
                 @{ N = 34; File = '34-upstream-vu1-vif1-scheduler-hot-paths.patch'; Mark = 'Optimize VU1/VIF1 hot paths'; Vu0 = $true })) {
    if ($p.Vu0 -and -not $Vu0) { continue }
    if (-not (git -C $Rt log --oneline -20 | Select-String -SimpleMatch $p.Mark)) {
        "== applying runtime patch $($p.N)"
        git -C $Rt -c user.name=Jon -c user.email=resands@proton.me am "$Root\tools\patches\pop-sotc\$($p.File)"
        if ($LASTEXITCODE -ne 0) { git -C $Rt am --abort; "PATCH $($p.N) FAILED"; exit 1 }
    }
}
git -C "$Root\game\recomp-sotc\runtime-src" checkout -q --detach (git -C $Rt rev-parse HEAD)
$capture = "$Root\game\recomp-sotc\vu1_capture"
$states = Get-ChildItem "$Root\game\ps2\disc\states\$Pattern" | Sort-Object Name
if ($Vu0) {
    $NoCapture = $true
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') VU code images from the saves"
    python -I "$Root\tools\vuimages.py" $capture (Get-ChildItem "$Root\game\ps2\disc\states\*.state" | ForEach-Object FullName)
}
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') capture: $((Get-ChildItem "$capture\vu1_*.bin").Count) images before"
if (-not $NoCapture) {
    foreach ($st in $states) {
        & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds $CaptureSeconds -Name "vu1cap" `
            -Env "PS2X_STATE_LOAD_AT=120:states/$($st.Name);POP_60FPS=1;PS2X_VU1_CAPTURE=$capture" | Out-Null
        "$($st.BaseName): $((Get-ChildItem "$capture\vu1_*.bin").Count) images"
    }
}
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') rebuild with the compiled VU1 microcode"
if (-not $NoBuild) {
    & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc -Vu1 $capture
    if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
}
if ($Vu0) {
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') verify: compiled VU0 runs against the interpreter"
    foreach ($st in @('enemies', 'play_20261017_093051', 'play_20261018_213904')) {
        & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 25 -Name "vu0verify" `
            -Env "PS2X_STATE_LOAD_AT=120:states/$st.state;POP_60FPS=1;PS2X_VU0_VERIFY=1" | Out-Null
        $v = Select-String -Path "$Root\game\recomp-sotc\vu0verify.log" -Pattern '\[vu0-verify\]' | ForEach-Object Line
        "$st : $(@($v | Where-Object { $_ -like '*mismatch #*' }).Count) mismatch lines; last: $(($v | Select-Object -Last 1))"
        $v | Where-Object { $_ -like '*mismatch #*' } | Select-Object -First 3
    }
}
if ($NoMeasure) { "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done (no measuring)"; exit 0 }
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') fps of every save"
$fpsOut = if ($Vu0) { "$Root\game\analysis\stateshots\fps_after_vu0.txt" } else { "$Root\game\analysis\stateshots\fps_after_vu1.txt" }
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
if ($Vu0) {
    # keep the VU1-only profiles for comparison
    New-Item -ItemType Directory -Force "$Root\game\analysis\prof\after_vu1" | Out-Null
    Copy-Item "$Root\game\analysis\prof\*.txt" "$Root\game\analysis\prof\after_vu1\" -ErrorAction SilentlyContinue
}
if ($ProfileStates.Count) { & "$Root\tools\windows\profile-states.ps1" -NoBuild -States $ProfileStates }
else { & "$Root\tools\windows\profile-states.ps1" -NoBuild }
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') nightly checks on the new build"
if (-not $NoNightly) { & "$Root\tools\windows\llm-job.ps1" -Job nightly }
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done"
# -Sleep puts the PC to sleep at the end (off by default: the user shuts it down or asks for it)
if ($Sleep) { "putting the PC to sleep"; Start-Process rundll32.exe -ArgumentList 'powrprof.dll,SetSuspendState 0,1,0' }
