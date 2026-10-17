# Profiles slow scenes from kept quick-saves with the in-process sampler (POP_PROFILE,
# tools/overrides/pop_profiler.cpp) and symbolizes the result with the play build's linker map.
# Run through start-job.ps1 (the game window needs the desktop session):
#   tools\windows\start-job.ps1 -Script tools\windows\profile-states.ps1
# Default scenes (docs/saves_1017.md): the sand-monster fight (#50, 3-18 fps), the courtyard pool
# (#11, ~38 fps) and a 60 fps reference (#40, fire pits). Save-state boots have no sound.
# Output: game\analysis\prof\<state>.txt (raw) and <state>.report.txt (per thread, self + inclusive).
param([switch]$NoBuild, [string[]]$States = @('play_20261017_093051', 'play_20261017_091049', 'play_20261017_092346'),
      [int]$Start = 12, [int]$Duration = 30)
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$out = "$Root\game\analysis\prof"
New-Item -ItemType Directory -Force $out | Out-Null
if (-not $NoBuild) {
    "== fast play build (overrides, linker map)"
    & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc
    if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
}
$map = "$Root\game\recomp-sotc\runtime-src\build-win-play\ps2xRuntime\ps2EntryRunner.map"
if (-not (Test-Path $map)) { "no linker map at $map (rebuild.ps1 adds /MAP to the play build)"; exit 1 }
foreach ($st in $States) {
    "== $st"
    $prof = "$out\$st.txt"
    Remove-Item $prof -ErrorAction SilentlyContinue
    & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds ($Start + $Duration + 10) -Name "prof_$st" `
        -Env "PS2X_STATE_LOAD_AT=120:states/$st.state;POP_60FPS=1;POP_PROFILE=$($prof):$($Start):$($Duration)" | Out-Null
    $fps = (Select-String -Path "$Root\game\recomp-sotc\prof_$st.log" -Pattern '\[pop:fps\] ([\d.]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ' '
    "fps: $fps"
    if (Test-Path $prof) {
        python -I "$Root\tools\profsym.py" $prof $map --top 30 | Set-Content -Encoding utf8 "$out\$st.report.txt"
        Get-Content "$out\$st.report.txt" -TotalCount 40
    } else { "no profile written" }
}
"== done"
