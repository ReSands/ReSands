# PC job (night of 26-27 Oct, until done or cancelled): the local model (Qwen3.6 via llama-server) ports the
# particle program P5 (tools/vu1port/port.py STAGES_P5, 87 stages; docs/llm_jobs.md "VU1 particle program P5"),
# entirely on this PC. Same shape as oct25.ps1 (P4); new since: the primer gives the exact norm() / fix() float
# helpers, the judge rejects the largest float written as an integer, every request has a fresh seed (retries
# came back byte-identical), and the whole-program check covers every recorded P5 run (930), not a sample.
# Started from the desktop shortcut "Qwen overnight (until Stop Qwen)"; the window shows progress, the log is
# game\jobs\qwen-night.log.
#   1. MSVC environment (the judge is built with cl, AddressSanitizer), llama-server (task LlamaServe; checked
#      before every stage and restarted if down: the task has a 24-hour limit);
#   2. passes over the unfinished P5 stages in program order, $Rounds rounds each (a fresh conversation every 3
#      rounds; a passing stage is the example for the next); port.py --chain p5 once every stage passes;
#   3. ends when every stage has passed, or after two passes in a row with no new pass: stops llama-server and
#      shuts the PC down (5 minutes' notice: `shutdown /a` in a console cancels).
# Cancelling, at any time: the desktop shortcut "Stop Qwen" (tools\windows\qwen-stop.ps1: ends this job and
# llama-server, frees the GPU, never shuts down), or Ctrl+C / closing the window (then run "Stop Qwen" too, or
# llama-server keeps the GPU). Nothing done is lost: every round is on disk as it finishes (at most the round
# in progress), and the next run of this script resumes where it stopped.
# -Until HH:mm: no new round after that time (then it ends by itself and shuts down). -NoShutdown keeps the PC on.
param([string]$Until = '', [switch]$NoShutdown, [int]$Rounds = 3)
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
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
    Say "== Qwen night: P5 until $($end.ToString('ddd HH:mm'))"
} else {
    Say '== Qwen: P5 until every stage passes, two passes bring nothing new, or "Stop Qwen"'
}

$finished = $false
$started = Get-Date
try {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC not found' }
    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    $env:VSCMD_SKIP_SENDTELEMETRY = '1'
    $env:PYTHONUTF8 = '1'
    Set-Location $Root

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
    Ensure-Llama
    Say 'llama-server up'

    $Python = (Get-Command python -ErrorAction Stop).Source
    $p5 = @(& $Python -I -c "import sys; sys.path.insert(0, r'$Root\tools\vu1port'); import port; print('\n'.join(port.STAGES_P5))")
    if ($p5.Count -lt 80) { throw "P5 stage list from port.py: $($p5 -join ' ')" }
    function Passed($s) { (Test-Path "$Root\game\analysis\qwen\vu1_$s\PASS") -or (Test-Path "$Root\tools\vu1port\known_good\$s.cpp") }
    # the cases are copied from the Mac (game\ is not in git): stop now, while the user is still here
    $missing = @(($p5 + 'p5_whole') | Where-Object { -not (Test-Path "$Root\game\analysis\vu1port\$_.cases") })
    if ($missing.Count) { throw "no cases for: $($missing -join ' ')" }
    $ErrorActionPreference = 'Continue'
    function Left { @($p5 | Where-Object { -not (Passed $_) }) }
    function Work($s, $n, $label) {
        if ((Passed $s) -or (Get-Date) -gt $end) { return }
        Ensure-Llama
        Say "$label $s ($n more rounds)"
        & $Python -I "$Root\tools\vu1port\port.py" $s --rounds $n --fresh 3 --until $untilUnix 2>&1 | ForEach-Object { "  $_" }
        Say ("  $s " + $(if (Passed $s) { 'PASSED' } else { 'not passed' }))
    }
    $pass = 0
    $idle = 0
    while ((Left).Count -and (Get-Date) -lt $end -and $idle -lt 2) {
        $pass++
        $before = (Left).Count
        Say "== pass ${pass}: P5 left $before of $($p5.Count)"
        foreach ($s in $p5) { Work $s $Rounds "P5 pass $pass" }
        if ((Left).Count -lt $before) { $idle = 0 } else { $idle++ }
    }
    if (-not (Left).Count) {
        Say 'all P5 stages pass: whole-program judge (port.py --chain p5)'
        & $Python -I "$Root\tools\vu1port\port.py" --chain p5 2>&1 | Select-Object -First 40 | ForEach-Object { "  $_"; Add-Content -Path $Log -Value "  $_" }
    } elseif ($idle -ge 2) {
        Say 'two passes without a new pass: stopping'
    }
    Say "P5 stages left: $((Left).Count) of $($p5.Count): $((Left) -join ' ')"
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
