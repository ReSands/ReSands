# Play the game on the PC: the pop-sotc play build at 60 fps (POP_60FPS=1), no time limit.
# Usage: tools\windows\play.ps1 [-Fps30] [-Env 'A=1;B=2']
#        -Fps30 plays at the original 30 fps (to recheck something seen at 60).
# Desktop shortcuts (made by -Shortcuts): "PoP 60 fps" and "PoP 30 fps (recheck)".
# Starts in borderless fullscreen with the debug panel hidden (PS2X_FULLSCREEN=1, PS2X_DEBUGGER=0; runtime
# patch 27): F11 toggles fullscreen, F1 shows the panel.
# While playing: Select (or F5) quick-saves; L3 + R3 (or F3) shows/hides the frame-time graph (patch 28); every quick-save is also kept as
# game\ps2\disc\states\play_<date>_<time>.state. Alt+F4 or closing the window quits. Avoid F9 (quick-load).
# Log: game\recomp-sotc\play_<date>_<time>.log, written when the game window is closed.
param([switch]$Fps30, [string]$Env = '', [switch]$Shortcuts)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path

if ($Shortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    $desk = [Environment]::GetFolderPath('Desktop')
    foreach ($s in @(@{ Name = 'PoP 60 fps'; Args = '' }, @{ Name = 'PoP 30 fps (recheck)'; Args = ' -Fps30' })) {
        $lnk = $shell.CreateShortcut("$desk\$($s.Name).lnk")
        $lnk.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
        $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File `"$PSCommandPath`"$($s.Args)"
        $lnk.WorkingDirectory = $Root
        $lnk.Save()
        "shortcut: $desk\$($s.Name).lnk"
    }
    return
}

$fps = $(if ($Fps30) { '0' } else { '1' })
$name = "play_$(Get-Date -Format 'yyyyMMdd_HHmm')"
& "$PSScriptRoot\boot.ps1" -Play -Out 'game\recomp-sotc' -Seconds 86400 -Name $name -Env "POP_60FPS=$fps;PS2X_FULLSCREEN=1;PS2X_DEBUGGER=0;$Env"
