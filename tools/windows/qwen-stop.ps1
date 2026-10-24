# Stops the Qwen job at once (desktop shortcut "Stop Qwen"): the night script (its id in game\jobs\qwen-night.pid)
# with its children (port.py, a judge in progress), then llama-server, so the GPU is free. Never shuts the PC down.
# Nothing done is lost: every round is on disk as it finishes; the next run of the night script resumes.
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$pidFile = "$Root\game\jobs\qwen-night.pid"
if (Test-Path $pidFile) {
    $id = (Get-Content $pidFile | Select-Object -First 1).Trim()
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -match 'powershell|pwsh') {
        taskkill /T /F /PID $id | Out-Null
        "stopped the Qwen job (process $id)"
    } else { 'no Qwen job running' }
    Remove-Item -Force $pidFile
} else { 'no Qwen job running' }
# a port.py left from a job started some other way
Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" |
    Where-Object { $_.CommandLine -match 'vu1port\\port\.py' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "stopped port.py (process $($_.ProcessId))" }
& "$PSScriptRoot\llama-serve.ps1" -Stop | Out-Null
'llama-server stopped; the GPU is free'
Add-Content -Path "$Root\game\jobs\qwen-night.log" -Value "$(Get-Date -Format 'yyyy-MM-dd HH:mm') stopped by qwen-stop.ps1 (Stop Qwen): no shutdown"
Start-Sleep 3
