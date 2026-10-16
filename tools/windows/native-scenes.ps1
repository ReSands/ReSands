# Exports native frames of more scenes on this PC (next step "test other scenes natively"): a fast
# play build with the current overrides, then save-state boots with the live conversion and a frame
# export (POP_NATIVE_LIVE / POP_NATIVE_FRAME), plus the SotC runtime's own screenshots around the
# export for comparison. Run through start-job.ps1 (needs the desktop session for the window):
#   tools\windows\start-job.ps1 -Script tools\windows\native-scenes.ps1
# Output: game\analysis\native\scenes\<state>.pnf (+ .sub.txt), screenshots in game\recomp-sotc\screens,
# boot logs game\recomp-sotc\scene_<state>.log. Copy back to the Mac with scp.
param([switch]$NoBuild)
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$out = "$Root\game\analysis\native\scenes"
New-Item -ItemType Directory -Force $out | Out-Null
if (-not $NoBuild) {
    "== fast play build (overrides)"
    & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc
    if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
}
# state file, seconds after start for the export, screenshot fields (the state's own field counter
# continues from its save: gameplay1 14377, enemies 22625; ~60 fields per second; the export
# second is counted from process start, the load happens at field 120 of the boot, ~2 s in)
$scenes = @(
    @{ State = 'gameplay1'; At = 20; Shots = '15380,15440,15500' },
    @{ State = 'enemies';   At = 5;  Shots = '22780,22800,22820' }
)
foreach ($s in $scenes) {
    $name = $s.State
    "== $name"
    $envs = "PS2X_STATE_LOAD_AT=120:states/$name.state;POP_NATIVE_LIVE=1;POP_NATIVE_FRAME=$out\$name.pnf:$($s.At)"
    & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 45 -Name "scene_$name" -Shots $s.Shots -Env $envs
    if (Test-Path "$out\$name.pnf") { "$name.pnf: $((Get-Item "$out\$name.pnf").Length) bytes" } else { "$name.pnf: NOT WRITTEN" }
}
"== done"
