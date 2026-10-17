# Boots each kept quick-save briefly and takes the SotC runtime's own screenshots, plus the [pop:fps]
# lines of that boot (a save-state boot has no sound, so this measures picture and speed only).
# For surveying a playthrough's saves. Run through start-job.ps1 (the window needs the desktop session):
#   tools\windows\start-job.ps1 -Script tools\windows\state-shots.ps1
# -Pattern picks the states (default: today's kept saves), -Seconds the boot length.
# Output: game\analysis\stateshots\<state>\ (screenshots + boot log), summary lines in the job log.
param([string]$Pattern = "play_$(Get-Date -Format 'yyyyMMdd')_*.state", [int]$Seconds = 20, [string]$Fps60 = '1')
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$out = "$Root\game\analysis\stateshots"
$screens = "$Root\game\recomp-sotc\screens"
New-Item -ItemType Directory -Force $out | Out-Null
$states = Get-ChildItem "$Root\game\ps2\disc\states\$Pattern" | Sort-Object Name
"== $($states.Count) states"
foreach ($st in $states) {
    $name = $st.BaseName
    $dir = "$out\$name"
    if (Test-Path "$dir\boot.log") { "${name}: done before, skipped"; continue }
    New-Item -ItemType Directory -Force $dir | Out-Null
    # The field counter continues from the save; find the saved field in the play log line
    # "[savestate] State saved (field N)" just before "quick-save kept as states/<name>.state".
    $field = $null
    foreach ($log in Get-ChildItem "$Root\game\recomp-sotc\play_*.log") {
        $lines = Get-Content $log
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -like "*kept as states/$name.state*") {
                for ($j = $i; $j -ge [Math]::Max(0, $i - 5); $j--) {
                    if ($lines[$j] -match 'State saved \(field (\d+)\)') { $field = [int]$Matches[1]; break }
                }
            }
        }
        if ($field) { break }
    }
    if (-not $field) { "${name}: saved field not found, skipped"; continue }
    # 0.5 s, 4 s and 10 s after the load
    $shots = "$($field + 30),$($field + 240),$($field + 600)"
    Remove-Item "$screens\*" -ErrorAction SilentlyContinue
    & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds $Seconds -Name "shots_$name" -Shots $shots `
        -Env "PS2X_STATE_LOAD_AT=120:states/$($st.Name);POP_60FPS=$Fps60" | Out-Null
    Move-Item "$Root\game\recomp-sotc\shots_$name.log" "$dir\boot.log" -Force
    Get-ChildItem "$screens\*" -ErrorAction SilentlyContinue | Move-Item -Destination $dir -Force
    $fps = (Select-String -Path "$dir\boot.log" -Pattern '\[pop:fps\] ([\d.]+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ' '
    $n = (Get-ChildItem $dir -Exclude boot.log).Count
    "${name} field ${field}: $n shots, fps $fps"
}
"== done"
