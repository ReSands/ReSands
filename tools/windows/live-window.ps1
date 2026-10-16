# Live native window test on this PC: builds the native tools, starts the live viewer
# (pop_native_view --live, with a screenshot of the presented frame every 10 s), boots a save state
# with POP_NATIVE_WINDOW so the game streams every frame to it, plus the runtime's own screenshots
# for comparison. Needs the desktop session (run through start-job.ps1, or from oct16.ps1).
#   tools\windows\live-window.ps1 [-State gameplay1] [-Seconds 60]
# Output: game\analysis\native\live\<state>_live_<n>.png, viewer log <state>_viewer.log,
# boot log game\recomp-sotc\live_<state>.log, screenshots in game\recomp-sotc\screens.
param([string]$State = 'gameplay1', [int]$Seconds = 60, [string]$Shots = '15000,15300,15600,15900')
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
"== native tools build"
& "$Root\tools\windows\native.ps1"
$out = "$Root\game\analysis\native\live"
New-Item -ItemType Directory -Force $out | Out-Null
$viewer = "$Root\game\native-build\pop_native_view.exe"
if (-not (Test-Path $viewer)) { "no viewer ($viewer)"; exit 1 }
"== live viewer"
$v = Start-Process -FilePath $viewer -ArgumentList @('live', '-', "$out\$State.png", '--live', '47800', '--live-shots', '10') `
    -RedirectStandardOutput "$out\${State}_viewer.log" -RedirectStandardError "$out\${State}_viewer.err" -PassThru
Start-Sleep 3
"== boot $State streaming to it"
& "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds $Seconds -Name "live_$State" -Shots $Shots `
    -Env "PS2X_STATE_LOAD_AT=120:states/$State.state;POP_NATIVE_LIVE=1;POP_NATIVE_WINDOW=47800"
Start-Sleep 2
if (-not $v.HasExited) { Stop-Process -Id $v.Id -Force }
Get-Content "$out\${State}_viewer.log" -ErrorAction SilentlyContinue | Select-Object -Last 8
Get-Content "$out\${State}_viewer.err" -ErrorAction SilentlyContinue | Select-Object -Last 5
"== live window test done"
