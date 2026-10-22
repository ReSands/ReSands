# PC job (night of 22-23 Oct): the local model (Qwen3.6 via llama-server) ports the unfinished stages of the
# VU1 water program P6 (tools/vu1port/port.py; docs/llm_jobs.md "VU1 water program P6"), entirely on this PC,
# then shuts the PC down. Started from the desktop shortcut "Qwen overnight (then shuts down)"; the window
# shows progress, the log is game\jobs\qwen-night.log.
#   1. MSVC environment (the judge is built with cl, AddressSanitizer), llama-server (task LlamaServe);
#   2. pass 1: every unfinished stage, 6 rounds, a fresh conversation every 3 rounds; the inner-loop stages
#      that failed on 19-20 Oct were split again on 22 Oct (9-20 pairs each);
#   3. pass 2 (time left): 6 more rounds for the stages still failing; the two clip-check stages last;
#   4. port.py --chain (whole program on the 600 whole runs) when every stage has passed;
#   5. stops llama-server and shuts the PC down (5 minutes' notice: `shutdown /a` in a console cancels).
# No new round starts after -Until (default 07:30). -NoShutdown keeps the PC on.
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

try {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC not found' }
    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    $env:VSCMD_SKIP_SENDTELEMETRY = '1'
    $env:PYTHONUTF8 = '1'
    Set-Location $Root

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
    $stages = @('p6_a_g1_in1a', 'p6_a_g1_in1b', 'p6_a_g1_in2a', 'p6_a_g1_in2b', 'p6_a_g1_in2c',
                'p6_a_g2_in1a', 'p6_a_g2_in1b', 'p6_a_g2_in2a', 'p6_a_g2_in2b', 'p6_a_g2_in3a', 'p6_a_g2_in3b',
                'p6_b_g1_in1a', 'p6_b_g1_in1b',
                'p6_b_g2_head_a', 'p6_b_g2_head_b', 'p6_b_g2_in1a', 'p6_b_g2_in1b', 'p6_b_g2_in2a', 'p6_b_g2_in2b',
                'p6_a_check', 'p6_b_check')
    function Passed($s) { Test-Path "$Root\game\analysis\qwen\vu1_$s\PASS" }
    $ErrorActionPreference = 'Continue'
    foreach ($pass in 1, 2) {
        foreach ($s in $stages) {
            if (Passed $s) { continue }
            if ((Get-Date) -gt $end) { break }
            # port.py resumes after the last round on disk: pass 2 adds $Rounds more
            $n = $Rounds * $pass
            Say "pass $pass $s (up to round $n)"
            & $Python -I "$Root\tools\vu1port\port.py" $s --rounds $n --fresh 3 --until $untilUnix 2>&1 | ForEach-Object { "  $_" }
            Say ("  $s " + $(if (Passed $s) { 'PASSED' } else { 'not passed' }))
        }
    }
    $left = @($stages | Where-Object { -not (Passed $_) })
    Say "stages passed tonight's list: $($stages.Count - $left.Count) of $($stages.Count); left: $($left -join ' ')"
    if ($left.Count -eq 0) {
        Say 'all stages pass: whole-program judge (port.py --chain)'
        & $Python -I "$Root\tools\vu1port\port.py" --chain 2>&1 | Select-Object -First 40 | ForEach-Object { "  $_"; Add-Content -Path $Log -Value "  $_" }
    }
} catch {
    Say "ERROR: $_"
} finally {
    Say 'stopping llama-server'
    & "$Root\tools\windows\llama-serve.ps1" -Stop | Out-Null
    if (-not $NoShutdown) {
        Say 'shutting the PC down in 5 minutes (shutdown /a cancels)'
        shutdown /s /t 300 /c "Qwen night job finished: shutting down in 5 minutes (shutdown /a cancels)."
    }
    Say 'QWEN_NIGHT_DONE'
}
