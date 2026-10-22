# VU1 trace of one save (PS2X_VU1_TRACE, built into the runtime): boots the play build from the save
# (states\<State>.state), records `Fields` GS fields of VU1 runs starting `After` fields past the save's own
# field (popstate.py prints it as 'tick'; the trace counts GS fields, which the load restores to the save's
# and which advance only with presented frames), writes game\analysis\vu1trace\<State>.trace plus the code
# images (vu1_<hash>.bin). Boots the game: the TV must be on (checked).
# Usage: tools\windows\vu1trace.ps1 -State play_20261022_192521 -Tick 1500 [-After 400] [-Fields 6]
param([Parameter(Mandatory)][string]$State, [Parameter(Mandatory)][int]$Tick, [int]$After = 400, [int]$Fields = 6)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
if ((nvidia-smi --query-gpu=display_active --format=csv,noheader) -notmatch 'Enabled') { 'no active display (TV off): not booting'; exit 1 }
$trace = "$Root\game\analysis\vu1trace"
New-Item -ItemType Directory -Force $trace | Out-Null
$file = "$trace\$State.trace"
Remove-Item $file -ErrorAction SilentlyContinue
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') VU1 trace of $State from field $($Tick + $After), $Fields fields"
& "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 30 -Name "vu1trace" `
    -Env "PS2X_STATE_LOAD_AT=120:states/$State.state;POP_60FPS=1;PS2X_VU1_TRACE=$($file):$($Tick + $After):$Fields" | Out-Null
Select-String -Path "$Root\game\recomp-sotc\vu1trace.log" -Pattern '\[vu1-trace\]' | ForEach-Object Line
"$State.trace: $(if (Test-Path $file) { (Get-Item $file).Length } else { 0 }) bytes"
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') done"
