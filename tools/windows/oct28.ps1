# PC job (from the night of 28-29 Oct, until done or cancelled): the local model (Qwen3.6 via llama-server)
# byte-matches DARE sound-engine functions (tools/decomp/qwen_match.py; docs/decomp_dare.md), entirely on this PC.
# Started from the desktop shortcut "Qwen DARE audio decomp"; the window shows progress, the log is
# game\jobs\qwen-night.log (one line per function; per-round detail in game\analysis\qwen\dare_<addr>\match.log).
#   1. llama-server (task LlamaServe; checked before every function and restarted if down: 24-hour task limit);
#   2. one pass over qwen_match.py --queue (untried first; same-size functions first, smallest first), $Rounds
#      rounds each; a function with PASS or HEADER is never queued again;
#   3. ends after the pass, at -Until, or on "Stop Qwen"; writes qwen_match.py --summary to the log, stops
#      llama-server and shuts the PC down (5 minutes' notice: `shutdown /a` in a console cancels).
# Cancelling, at any time: the desktop shortcut "Stop Qwen" (tools\windows\qwen-stop.ps1: ends this job and
# llama-server, frees the GPU, never shuts down). Nothing done is lost: every round is on disk as it finishes, and
# the next run resumes. Needs game\decomp\dare\ on this PC (copied from the Mac; see docs/decomp_dare.md).
# -Until HH:mm: no new round after that time. -NoShutdown keeps the PC on. -Shortcut: (re)make the desktop shortcut.
param([string]$Until = '', [switch]$NoShutdown, [int]$Rounds = 4, [switch]$Shortcut)
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root

if ($Shortcut) {
    $desk = [Environment]::GetFolderPath('Desktop')
    $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut("$desk\Qwen DARE audio decomp.lnk")
    $lnk.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    $lnk.WorkingDirectory = $Root
    $lnk.Save()
    "shortcut: $desk\Qwen DARE audio decomp.lnk"
    return
}

$Log = "$Root\game\jobs\qwen-night.log"
New-Item -ItemType Directory -Force -Path "$Root\game\jobs" | Out-Null
function Say([string]$s) { $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm') $s"; $line; Add-Content -Path $Log -Value $line }
# qwen-stop.ps1 ends this process (and its children) by this id
Set-Content -Path "$Root\game\jobs\qwen-night.pid" -Value $PID

$untilUnix = 0
$end = [datetime]::MaxValue
if ($Until) {
    $end = [datetime]::Today.Add([timespan]::Parse($Until))
    if ($end -lt (Get-Date)) { $end = $end.AddDays(1) }
    $untilUnix = [int64]([DateTimeOffset]$end).ToUnixTimeSeconds()
    Say "== Qwen DARE decomp until $($end.ToString('ddd HH:mm')), $Rounds rounds per function"
} else {
    Say "== Qwen DARE decomp: one pass over the queue, $Rounds rounds per function, or until `"Stop Qwen`""
}

$finished = $false
$started = Get-Date
try {
    $env:PYTHONUTF8 = '1'
    function Test-Llama { try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 http://127.0.0.1:8033/health).StatusCode -eq 200 } catch { $false } }
    function Ensure-Llama {
        if (Test-Llama) { return }
        Say 'starting llama-server (LlamaServe)'
        # start the registered task: registering it again (llama-serve.ps1) needs an elevated shell
        if (Get-ScheduledTask -TaskName LlamaServe -ErrorAction SilentlyContinue) { Start-ScheduledTask -TaskName LlamaServe }
        else { & "$Root\tools\windows\llama-serve.ps1" | Out-Null }
        $t = 0
        while (-not (Test-Llama) -and $t -lt 900) { Start-Sleep 10; $t += 10 }
        if (-not (Test-Llama)) {
            # once more from scratch (a task still marked running ignores Start-ScheduledTask)
            Say 'llama-server not up after 15 minutes: restarting it'
            & "$Root\tools\windows\llama-serve.ps1" -Stop | Out-Null
            Start-ScheduledTask -TaskName LlamaServe
            $t = 0
            while (-not (Test-Llama) -and $t -lt 900) { Start-Sleep 10; $t += 10 }
        }
        if (-not (Test-Llama)) { throw 'llama-server did not come up within 15 minutes (game\jobs\llama.log)' }
    }

    $Python = (Get-Command python -ErrorAction Stop).Source
    $Match = "$Root\tools\decomp\qwen_match.py"
    # the work files come from the Mac (game\ is not in git): stop now, while the user is still here
    foreach ($f in 'dare.h', 'dare_fix.h', 'units.tsv', 'protos.tsv', 'callees.tsv', 'asm', 'ghidra_c') {
        if (-not (Test-Path "$Root\game\decomp\dare\$f")) { throw "missing game\decomp\dare\$f (copy it from the Mac)" }
    }
    $queue = @(& $Python -I $Match --queue)
    if ($LASTEXITCODE -ne 0) { throw "qwen_match.py --queue failed: $($queue -join ' ')" }
    Ensure-Llama
    Say "llama-server up; $($queue.Count) functions queued"
    $ErrorActionPreference = 'Continue'
    $i = 0
    foreach ($a in $queue) {
        if ((Get-Date) -gt $end) { Say 'past -Until: stopping'; break }
        $i++
        Ensure-Llama
        # output to the log through PowerShell, not straight over ssh (a 28 Oct run died that way)
        $lines = @(& $Python -I $Match $a --rounds $Rounds --until $untilUnix 2>&1 | ForEach-Object { "$_" })
        Say "[$i/$($queue.Count)] $a $(if ($lines) { $lines[-1] } else { '(no output)' })"
    }
    & $Python -I $Match --summary 2>&1 | ForEach-Object { Say "$_" }
    $finished = $true
} catch {
    Say "ERROR: $_"
    # an error in the first 15 minutes means the user is probably still at the PC: leave it on
    $finished = ((Get-Date) - $started).TotalMinutes -gt 15
    if (-not $finished) { Say 'error soon after the start: no shutdown' }
} finally {
    Say 'stopping llama-server'
    & "$Root\tools\windows\llama-serve.ps1" -Stop | Out-Null
    if (-not $finished) { Say 'interrupted: no shutdown' }
    elseif (-not $NoShutdown) {
        Remove-Item -Force -ErrorAction SilentlyContinue "$Root\game\jobs\qwen-night.pid"
        Say 'shutting the PC down in 5 minutes (shutdown /a cancels)'
        shutdown /s /t 300 /c "Qwen night job finished: shutting down in 5 minutes (shutdown /a cancels)."
    }
    Say 'QWEN_NIGHT_DONE'
}
