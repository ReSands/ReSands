# PC job (night of 24-25 Oct, and on until cancelled): the local model (Qwen3.6 via llama-server) ports the
# last 9 P6 stages, then the sprite program P9 (tools/vu1port/port.py; docs/llm_jobs.md "VU1 water program P6",
# "VU1 sprite program P9"), entirely on this PC. Changes from oct23.ps1 (docs/llm_jobs.md "24 Oct morning"): the 6
# stages that failed on VU denormal handling and plain bugs were fixed by hand (tools/vu1port/known_good/, which
# counts as passed); the two clip checks (0 of 174 cases in two nights) are split into 12 pieces, 3 of them
# hand-ported as the end-to-end check, so the 9 left are those pieces; the primer now states the VU's float
# normalisation. Added 24 Oct evening: P9 after the water passes, and no deadline (the user may leave it running).
# Started from the desktop shortcut "Qwen overnight (then shuts down)"; the window shows progress, the log
# is game\jobs\qwen-night.log.
#   1. MSVC environment (the judge is built with cl, AddressSanitizer), llama-server (task LlamaServe; checked
#      before every stage and restarted if down: the task has a 24-hour limit);
#   2. water: 4 passes of $Rounds rounds per unfinished P6 piece (a fresh conversation every 3 rounds);
#   3. then passes without end: each unfinished P9 stage in program order ($Rounds9 rounds; a passing stage is
#      the example for the next), then each unfinished P6 piece; port.py --chain p6 / p9 once a program passes;
#   4. only when every stage of both programs has passed: stops llama-server and shuts the PC down (5 minutes'
#      notice: `shutdown /a` in a console cancels).
# Cancelling, at any time: the desktop shortcut "Stop Qwen" (tools\windows\qwen-stop.ps1: ends this job and
# llama-server, frees the GPU, never shuts down), or Ctrl+C / closing the window (then run "Stop Qwen" too, or
# llama-server keeps the GPU). Nothing done is lost: every round is on disk as it finishes (at most the round
# in progress), and the next run of this script, or any later one, resumes where it stopped.
# -Until HH:mm: no new round after that time (then it ends by itself and shuts down). -NoShutdown keeps the PC on.
param([string]$Until = '', [switch]$NoShutdown, [int]$Rounds = 6, [int]$Rounds9 = 3)
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
    Say "== Qwen night: P6, then P9, until $($end.ToString('ddd HH:mm'))"
} else {
    Say '== Qwen: P6, then P9, until cancelled ("Stop Qwen") or everything passes'
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
    # in program order: a passing piece becomes the example for the next one
    $p6 = @('p6_a_ck_head', 'p6_a_ck_loop', 'p6_a_ck_cull', 'p6_a_ck_sub',
            'p6_b_ck_head', 'p6_b_ck_loop', 'p6_b_ck_next', 'p6_b_ck_cull', 'p6_b_ck_sub')
    $p9 = @(& $Python -I -c "import sys; sys.path.insert(0, r'$Root\tools\vu1port'); import port; print('\n'.join(port.STAGES_P9))")
    if ($p9.Count -lt 30) { throw "P9 stage list from port.py: $($p9 -join ' ')" }
    function Passed($s) { (Test-Path "$Root\game\analysis\qwen\vu1_$s\PASS") -or (Test-Path "$Root\tools\vu1port\known_good\$s.cpp") }
    function CaseFile($s) { if ($s.StartsWith('p6_')) { $s.Substring(3) } else { $s } }
    # the cases are copied from the Mac (game\ is not in git): stop now, while the user is still here
    $missing = @(($p6 + $p9) | Where-Object { -not (Test-Path "$Root\game\analysis\vu1port\$(CaseFile $_).cases") })
    if ($missing.Count) { throw "no cases for: $($missing -join ' ')" }
    $ErrorActionPreference = 'Continue'
    $chained = @{}
    function Work($s, $n, $label) {
        if ((Passed $s) -or (Get-Date) -gt $end) { return }
        Ensure-Llama
        Say "$label $s ($n more rounds)"
        & $Python -I "$Root\tools\vu1port\port.py" $s --rounds $n --fresh 3 --until $untilUnix 2>&1 | ForEach-Object { "  $_" }
        Say ("  $s " + $(if (Passed $s) { 'PASSED' } else { 'not passed' }))
    }
    function Chain($prog, $stages) {
        if ($chained[$prog] -or @($stages | Where-Object { -not (Passed $_) }).Count) { return }
        $chained[$prog] = $true
        Say "all $prog stages pass: whole-program judge (port.py --chain $prog)"
        & $Python -I "$Root\tools\vu1port\port.py" --chain $prog 2>&1 | Select-Object -First 40 | ForEach-Object { "  $_"; Add-Content -Path $Log -Value "  $_" }
    }
    function Left($stages) { @($stages | Where-Object { -not (Passed $_) }) }
    # water first, as planned for tonight
    foreach ($pass in 1..4) { foreach ($s in $p6) { Work $s $Rounds "water pass $pass" } }
    Say "P6 pieces passed: $($p6.Count - (Left $p6).Count) of $($p6.Count); left: $((Left $p6) -join ' ')"
    Chain 'p6' $p6
    # then the sprites, and the water leftovers after each sprite pass, until everything passes (or -Until)
    $pass = 0
    while (((Left $p9).Count -or (Left $p6).Count) -and (Get-Date) -lt $end) {
        $pass++
        Say "== pass ${pass}: P9 left $((Left $p9).Count) of $($p9.Count), P6 left $((Left $p6).Count)"
        foreach ($s in $p9) { Work $s $Rounds9 "P9 pass $pass" }
        Chain 'p9' $p9
        foreach ($s in $p6) { Work $s $Rounds "P6 pass $pass" }
        Chain 'p6' $p6
    }
    Say "stages left: P9 $((Left $p9) -join ' '); P6 $((Left $p6) -join ' ')"
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
