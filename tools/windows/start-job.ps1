# Runs one of our scripts as a scheduled task in the logged-in user's desktop session, so it
# survives the SSH connection dropping (Windows kills an SSH session's processes when it ends)
# and a game window has a desktop to open on. One job at a time.
#
# Usage (e.g. over SSH from the Mac):
#   tools\windows\start-job.ps1 -Script tools\windows\rebuild.ps1 -Arguments '-Fast'
#   tools\windows\start-job.ps1 -Status     (prints the job log; last line is JOB_DONE ... when finished)
param([string]$Script, [string]$Arguments = '', [switch]$Status)
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Jobs = "$Root\game\jobs"
$Log  = "$Jobs\job.log"
$Task = 'PoP_Decomp_Job'
New-Item -ItemType Directory -Force -Path $Jobs | Out-Null

if ($Status) {
    $state = (Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue).State
    "task state: $state"
    if (Test-Path $Log) { Get-Content $Log }
    return
}

if ((Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue).State -eq 'Running') {
    throw "A job is already running (see $Log)"
}

$target = (Resolve-Path (Join-Path $Root $Script)).Path
# The wrapper always ends the log with JOB_DONE, even if the script dies with an error.
@"
`$started = Get-Date
`$code = 'error'
try {
    & '$target' $Arguments *>&1 | Tee-Object -FilePath '$Log'
    `$code = `$LASTEXITCODE
} catch {
    "JOB ERROR: `$_" | Add-Content '$Log'
} finally {
    "JOB_DONE exit=`$code minutes={0:N1}" -f ((Get-Date) - `$started).TotalMinutes | Add-Content '$Log'
}
"@ | Set-Content -Encoding utf8 "$Jobs\job.ps1"

Remove-Item $Log -ErrorAction SilentlyContinue
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
            -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File `"$Jobs\job.ps1`"" `
            -WorkingDirectory $Root
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 4) -AllowStartIfOnBatteries
Register-ScheduledTask -TaskName $Task -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $Task
"started $Script $Arguments; log: $Log"
