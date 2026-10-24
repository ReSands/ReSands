# PC job (night of 24-25 Oct): the local model (Qwen3.6 via llama-server) ports the last 9 P6 stages
# (tools/vu1port/port.py; docs/llm_jobs.md "VU1 water program P6"), entirely on this PC, then shuts the PC
# down. Changes from oct23.ps1 (docs/llm_jobs.md "24 Oct morning"): the 6 stages that failed on VU denormal
# handling and plain bugs were fixed by hand (tools/vu1port/known_good/, which counts as passed); the two
# clip checks (0 of 174 cases in two nights) are split into 12 pieces, 3 of them hand-ported as the end-to-end
# check, so the 9 left are those pieces; the primer now states the VU's float normalisation.
# Started from the desktop shortcut "Qwen overnight (then shuts down)"; the window shows progress, the log
# is game\jobs\qwen-night.log.
#   1. MSVC environment (the judge is built with cl, AddressSanitizer), llama-server (task LlamaServe);
#   2. passes of $Rounds more rounds per unfinished stage (a fresh conversation every 3 rounds) until -Until;
#   3. port.py --chain (whole program on the 600 whole runs) when every stage has passed;
#   4. stops llama-server and shuts the PC down (5 minutes' notice: `shutdown /a` in a console cancels).
# No new round starts after -Until (default 07:30). -NoShutdown keeps the PC on.
# Interrupting in the morning (Ctrl+C in the window, or closing it) is safe: every round is on disk as it
# finishes (at most the round in progress is lost), the next run resumes, and the PC is NOT shut down then
# (only a run that ends by itself shuts down). After Ctrl+C llama-server is stopped too; after closing the
# window it keeps running (`tools\windows\llama-serve.ps1 -Stop` frees the GPU).
param([string]$Until = '07:30', [switch]$NoShutdown, [int]$Rounds = 6)
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$Log = "$Root\game\jobs\qwen-night.log"
New-Item -ItemType Directory -Force -Path "$Root\game\jobs" | Out-Null
function Say([string]$s) { $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm') $s"; $line; Add-Content -Path $Log -Value $line }

$end = [datetime]::Today.Add([timespan]::Parse($Until))
if ($end -lt (Get-Date)) { $end = $end.AddDays(1) }
$untilUnix = [int64]([DateTimeOffset]$end).ToUnixTimeSeconds()
Say "== Qwen night: P6 stages until $($end.ToString('ddd HH:mm'))"

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

    # the thinking budget is a server flag: the task runs game\jobs\llama-serve.cmd, so rewrite that (as
    # tools\windows\llama-serve.ps1 now writes it) and restart a server started without it
    $cmdFile = "$Root\game\jobs\llama-serve.cmd"
    $cmd = Get-Content -Raw $cmdFile
    if ($cmd -match '--reasoning-budget -1 ') {
        $msg = '--reasoning-budget 16000 --reasoning-budget-message "Thinking budget reached: stop reasoning now and write the complete file in one cpp code block." '
        # .NET, not Set-Content: in this script PS 5.1 rejected Set-Content -Encoding (23 Oct, first start)
        [IO.File]::WriteAllText($cmdFile, ($cmd -replace '--reasoning-budget -1 ', $msg), [Text.Encoding]::ASCII)
        & "$Root\tools\windows\llama-serve.ps1" -Stop | Out-Null
        Say 'llama-serve.cmd: thinking budget 16000 set'
    }
    function Test-Llama { try { (Invoke-WebRequest -UseBasicParsing -TimeoutSec 5 http://127.0.0.1:8033/health).StatusCode -eq 200 } catch { $false } }
    if (-not (Test-Llama)) {
        Say 'starting llama-server (LlamaServe)'
        # start the registered task: registering it again (llama-serve.ps1) needs an elevated shell
        if (Get-ScheduledTask -TaskName LlamaServe -ErrorAction SilentlyContinue) { Start-ScheduledTask -TaskName LlamaServe }
        else { & "$Root\tools\windows\llama-serve.ps1" | Out-Null }
        $t = 0
        while (-not (Test-Llama) -and $t -lt 900) { Start-Sleep 10; $t += 10 }
        if (-not (Test-Llama)) { throw 'llama-server did not come up within 15 minutes (game\jobs\llama.log)' }
    }
    Say 'llama-server up'

    $Python = (Get-Command python -ErrorAction Stop).Source
    # in program order: a passing piece becomes the example for the next one
    $stages = @('p6_a_ck_head', 'p6_a_ck_loop', 'p6_a_ck_cull', 'p6_a_ck_sub',
                'p6_b_ck_head', 'p6_b_ck_loop', 'p6_b_ck_next', 'p6_b_ck_cull', 'p6_b_ck_sub')
    function Passed($s) { (Test-Path "$Root\game\analysis\qwen\vu1_$s\PASS") -or (Test-Path "$Root\tools\vu1port\known_good\$s.cpp") }
    # the pieces' cases are copied from the Mac (game\ is not in git): stop now, while the user is still here
    $missing = @($stages | Where-Object { -not (Test-Path "$Root\game\analysis\vu1port\$($_.Substring(3)).cases") })
    if ($missing.Count) { throw "no cases for: $($missing -join ' ')" }
    $ErrorActionPreference = 'Continue'
    foreach ($pass in 1..4) {
        foreach ($s in $stages) {
            if (Passed $s) { continue }
            if ((Get-Date) -gt $end) { break }
            Say "pass $pass $s ($Rounds more rounds)"
            & $Python -I "$Root\tools\vu1port\port.py" $s --rounds $Rounds --fresh 3 --until $untilUnix 2>&1 | ForEach-Object { "  $_" }
            Say ("  $s " + $(if (Passed $s) { 'PASSED' } else { 'not passed' }))
        }
    }
    $left = @($stages | Where-Object { -not (Passed $_) })
    Say "stages passed tonight's list: $($stages.Count - $left.Count) of $($stages.Count); left: $($left -join ' ')"
    if ($left.Count -eq 0) {
        Say 'all stages pass: whole-program judge (port.py --chain)'
        & $Python -I "$Root\tools\vu1port\port.py" --chain 2>&1 | Select-Object -First 40 | ForEach-Object { "  $_"; Add-Content -Path $Log -Value "  $_" }
    }
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
        Say 'shutting the PC down in 5 minutes (shutdown /a cancels)'
        shutdown /s /t 300 /c "Qwen night job finished: shutting down in 5 minutes (shutdown /a cancels)."
    }
    Say 'QWEN_NIGHT_DONE'
}
