# Play the game on the PC: the pop-sotc play build at 60 fps (POP_60FPS=1), no time limit.
# Usage: tools\windows\play.ps1 [-Fps30] [-Profile] [-Env 'A=1;B=2']
#        -Fps30 plays at the original 30 fps (to recheck something seen at 60).
#        -Profile: every kept quick-save also gets a profile of the seconds before it (see below); costs a
#        little speed itself, so it is a separate shortcut.
# Desktop shortcuts (made by -Shortcuts): "PoP 60 fps", "PoP 60 fps (profiling)" and "PoP 30 fps (recheck)".
# Starts in borderless fullscreen with the debug panel hidden (PS2X_FULLSCREEN=1, PS2X_DEBUGGER=0; runtime
# patch 27): F11 toggles fullscreen, F1 shows the panel.
# While playing: Select (or F5) quick-saves; L3 + R3 (or F3) shows/hides the frame-time graph (patch 28); every quick-save is also kept as
# game\ps2\disc\states\play_<date>_<time>.state; with -Profile also play_<date>_<time>.prof: a profile of the ~12 s
# before the save (POP_PROFILE_LAST, rolling profiler), so a dip can be saved just after it happened. When the
# game is closed, each new .prof is symbolized to <name>.report.txt with this build's linker map.
# Alt+F4 or closing the window quits. Avoid F9 (quick-load).
# Log: game\recomp-sotc\play_<date>_<time>.log, written when the game window is closed.
param([switch]$Fps30, [switch]$Profile, [string]$Env = '', [switch]$Shortcuts)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path

if ($Shortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    $desk = [Environment]::GetFolderPath('Desktop')
    foreach ($s in @(@{ Name = 'PoP 60 fps'; Args = '' }, @{ Name = 'PoP 60 fps (profiling)'; Args = ' -Profile' }, @{ Name = 'PoP 30 fps (recheck)'; Args = ' -Fps30' })) {
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
& "$PSScriptRoot\boot.ps1" -Play -Out 'game\recomp-sotc' -Seconds 86400 -Name $name -Env "POP_60FPS=$fps;PS2X_FULLSCREEN=1;PS2X_DEBUGGER=0;$(if ($Profile) { 'POP_PROFILE_LAST=12;' })$Env"

$map = "$Root\game\recomp-sotc\runtime-src\build-win-play\ps2xRuntime\ps2EntryRunner.map"
foreach ($prof in Get-ChildItem "$Root\game\ps2\disc\states\play_*.prof" -ErrorAction SilentlyContinue) {
    $report = [IO.Path]::ChangeExtension($prof.FullName, '.report.txt')
    if (-not (Test-Path $report) -and (Test-Path $map)) {
        python -I "$Root\tools\profsym.py" $prof.FullName $map --top 30 | Set-Content -Encoding utf8 $report
    }
}
