# Runs a local-model job entirely on this PC: starts Ollama (scheduled task OllamaServe), enters
# the MSVC environment (the MMI job compiles the instruction test), and runs the job. Start it
# through start-job.ps1 so it survives the SSH connection ending, e.g. from the Mac's
# tools/overnight-pc.command; the Mac can then be switched off.
#   tools\windows\start-job.ps1 -Script tools\windows\llm-job.ps1 -Arguments '-Job mmi -Spec'
# Jobs (both resumable):
#   mmi     tools/llm_port/fix_mmi.py: implement wrongly translated MMI instructions, judged by the
#           instruction test; -Spec gives the model each instruction's documented behaviour.
#           Results: game\analysis\mmifix[_spec]\<NAME>.json (+ .cpp when it passes).
#   nightly tools/nightly/checks.py (instruction test, all world packs, boot smoke test) then
#           tools/nightly/report.py (Gemma writes docs\nightly\<date>.md from the results).
#   naming  tools/llm_port/name_functions.py: name/summary suggestions for anonymous functions
#           (inputs copied to game\analysis\naming*). Results: game\analysis\naming\results.
#           -V2: second configuration (thinking, context from PoPNamingContext.java) -> results_v2;
#           -Calib: only the 60 calibration functions.
#   -ThenNaming (with -Job nightly): afterwards run the naming job in the v2 configuration until the
#           queue ends or it is stopped (tools/overnight-pc.command --stop; resumes the next night).
param([ValidateSet('mmi', 'naming', 'nightly')][string]$Job = 'mmi', [switch]$Spec, [int]$Tries = 3,
      [string]$Model = 'gemma4:26b', [string[]]$Only = @(), [switch]$V2, [switch]$Calib, [switch]$ThenNaming)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
Set-Location $Root

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC not found (Visual Studio Build Tools with the C++ workload)' }
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:VSCMD_SKIP_SENDTELEMETRY = '1'
Set-Location $Root

function Test-Ollama { try { (New-Object Net.Sockets.TcpClient('127.0.0.1', 11434)).Close(); $true } catch { $false } }
if (-not (Test-Ollama)) {
    "starting Ollama (scheduled task OllamaServe)"
    schtasks /run /tn OllamaServe | Out-Null
    $t = 0
    while (-not (Test-Ollama) -and $t -lt 120) { Start-Sleep 2; $t += 2 }
    if (-not (Test-Ollama)) { throw 'Ollama did not start within 2 minutes' }
}
"Ollama up; $(Get-Date -Format 'yyyy-MM-dd HH:mm') starting job '$Job' ($Model)"

$Python = (Get-Command python -ErrorAction Stop).Source
if ($Job -eq 'mmi') {
    $argList = @('-I', "$Root\tools\llm_port\fix_mmi.py", '--model', $Model, '--tries', "$Tries")
    if ($Spec) { $argList += '--spec' }
    if ($Only) { $argList += @('--only', ($Only -join ',')) }
} elseif ($Job -eq 'nightly') {
    $argList = @('-I', "$Root\tools\nightly\checks.py")
    if ($Only) { $argList += @('--only', ($Only -join ',')) }
} else {
    $argList = @('-I', "$Root\tools\llm_port\name_functions.py", '--model', $Model)
    if ($V2) { $argList += '--v2' }
    if ($Calib) { $argList += '--calib' }
}
# Native stderr must not become a terminating error under 'Stop' (PowerShell 5.1).
$ErrorActionPreference = 'Continue'
& $Python @argList 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
if ($Job -eq 'nightly') {
    & $Python -I "$Root\tools\nightly\report.py" --model $Model 2>&1 | ForEach-Object { "$_" }
    if ($ThenNaming) {
        "$(Get-Date -Format 'yyyy-MM-dd HH:mm') nightly done; starting naming (v2)"
        & $Python -I "$Root\tools\llm_port\name_functions.py" --model $Model --v2 2>&1 | ForEach-Object { "$_" }
    }
}
"$(Get-Date -Format 'yyyy-MM-dd HH:mm') job '$Job' finished (exit $code)"
exit $code
