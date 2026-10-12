# Windows version of tools/boot.sh: boot the recompiled game from the disc folder with a time limit.
# Usage: tools\windows\boot.ps1 [-Seconds 90] [-Name boot] [-PadInput 'start@55,cross@85,lleft@150:5'] [-Play]
#        -Play runs the log-free play build (rebuild.ps1 -Play), about 1.5x faster, with the GS
#        rasterizer on its own thread (POP_ASYNC_GS=1, another ~1.3x; -NoAsyncGs turns it off).
#        -PadInput sets POP_INPUT (scripted pad input, see tools/overrides/pop_slus20743.cpp).
#        Frame dumps go to game\recomp\screens.
# Log: game\recomp\<Name>.log (stdout then stderr). "timeout" means the time limit was hit.
# Run it from a desktop session (Claude Code on the PC): over SSH there is no desktop for the window.
param([int]$Seconds = 90, [string]$Name = 'boot', [string]$PadInput = '', [switch]$Play, [switch]$NoAsyncGs)
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Exe  = "$Root\game\recomp\runtime-src\$(if ($Play) { 'build-win-play' } else { 'build-win' })\ps2xRuntime\ps2EntryRunner.exe"
$Log  = "$Root\game\recomp\$Name.log"
$Out  = "$Log.stdout"
$Err  = "$Log.stderr"

New-Item -ItemType Directory -Force -Path "$Root\game\recomp\screens" | Out-Null
$env:POP_INPUT = $PadInput
$env:POP_ASYNC_GS = $(if ($Play -and -not $NoAsyncGs) { '1' } else { '0' })
$p = Start-Process -FilePath $Exe -ArgumentList '.\SLUS_207.43' -WorkingDirectory "$Root\game\ps2\disc" `
        -RedirectStandardOutput $Out -RedirectStandardError $Err -PassThru -NoNewWindow
$result = 'exited'
if (-not $p.WaitForExit($Seconds * 1000)) {
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit()
    $result = 'timeout'
}
Get-Content $Out, $Err | Set-Content $Log
Remove-Item $Out, $Err
$lines = (Get-Content $Log | Measure-Object -Line).Lines
"result=$result exit=$($p.ExitCode) lines=$lines log=$Log"
