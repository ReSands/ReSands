# Windows version of tools/boot.sh: boot the recompiled game from the disc folder with a time limit.
# Usage: tools\windows\boot.ps1 [-Seconds 90] [-Name boot]
# Log: game\recomp\<Name>.log (stdout then stderr). "timeout" means the time limit was hit.
# Run it from a desktop session (Claude Code on the PC): over SSH there is no desktop for the window.
param([int]$Seconds = 90, [string]$Name = 'boot')
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Exe  = "$Root\game\recomp\runtime-src\build-win\ps2xRuntime\ps2EntryRunner.exe"
$Log  = "$Root\game\recomp\$Name.log"
$Out  = "$Log.stdout"
$Err  = "$Log.stderr"

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
