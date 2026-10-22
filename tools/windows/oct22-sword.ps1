# PC job (22 Oct night): which draw group carries the Prince's sword (missing from the native frame of save
# 192521). Boots the save with the plain GS (no native world) and POP_NATIVE_SKIP per group (measurement only:
# the skipped group is simply not drawn), screenshots 0.5/4/10 s after the load. The group whose shots lose
# the sword is the path to decode. Output: game\analysis\native\sword\<group>\ (shots + boot log).
# First run (none/gfx/skinned/static/lightmap): the sword stayed in every one, but skinned/static chains that
# would switch the VU1 driver were kept. -Replay (POP_NATIVE_SKIP_REPLAY=1) skips those too, replaying the switches,
# and -Build first makes a fast play build (the new sprite/water/list18 groups). Output dir: <group>[_replay].
# Then: -Groups callers -ExtraEnv POP_NATIVE_SKIP_CALLERS=1 (Display callers by $ra in the log; "callers" contains
# "all", so that boot skipped everything too) and -Groups static -ExtraEnv POP_NATIVE_SKIP_RA=<hex> -Tag _ra<hex>
# (static calls skipped from one caller only).
# Run through start-job.ps1 (the window needs the desktop session); the TV must be on.
param([string]$State = 'play_20261022_192521', [string]$Groups = 'none,gfx,skinned,static,lightmap', [switch]$Replay, [switch]$Build, [string]$ExtraEnv = '', [string]$Tag = '')
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
if ((nvidia-smi --query-gpu=display_active --format=csv,noheader) -notmatch 'Enabled') { "NO ACTIVE DISPLAY (switch the TV on): FAILED"; exit 1 }
if ($Build) {
    "== $(Get-Date -Format 'yyyy-MM-dd HH:mm') fast play build (override: POP_NATIVE_SKIP groups, replay)"
    & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc
    if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
}
$screens = "$Root\game\recomp-sotc\screens"
$out = "$Root\game\analysis\native\sword"
# the saved field (the field counter continues from it), as state-shots.ps1 finds it
$field = $null
foreach ($log in Get-ChildItem "$Root\game\recomp-sotc\play_*.log") {
    $lines = Get-Content $log
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -like "*kept as states/$State.state*") {
            for ($j = $i; $j -ge [Math]::Max(0, $i - 5); $j--) {
                if ($lines[$j] -match 'State saved \(field (\d+)\)') { $field = [int]$Matches[1]; break }
            }
        }
    }
    if ($field) { break }
}
if (-not $field) { "saved field of $State not found: FAILED"; exit 1 }
"== $State saved at field $field"
foreach ($group in $Groups -split ',') {
    $dir = "$out\$group$(if ($Replay) { '_replay' })$Tag"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Remove-Item "$screens\*", "$dir\*" -ErrorAction SilentlyContinue
    $skip = $(if ($group -eq 'none') { '' } else { ";POP_NATIVE_SKIP=$group;POP_NATIVE_SKIP_REPLAY=$(if ($Replay) { 1 } else { 0 })" })
    & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 30 -Name "sword" -Shots "$($field + 30),$($field + 240),$($field + 600)" `
        -Env "PS2X_STATE_LOAD_AT=120:states/$State.state;POP_60FPS=1;POP_NATIVE_WORLD=0;POP_NATIVE_CHARACTERS=0$skip$(if ($ExtraEnv) { ";$ExtraEnv" })" | Out-Null
    Move-Item "$Root\game\recomp-sotc\sword.log" "$dir\boot.log" -Force
    Get-ChildItem "$screens\*" -ErrorAction SilentlyContinue | Move-Item -Destination $dir -Force
    $counts = Select-String -Path "$dir\boot.log" -Pattern 'skipped/kept' | Select-Object -Last 1 | ForEach-Object { $_.Line -replace '.*kept: ', '' }
    "${group}: $((Get-ChildItem $dir -Exclude boot.log).Count) shots | $counts"
}
"== done"
