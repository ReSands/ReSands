# Play the game on the PC: the pop-sotc play build at 60 fps (POP_60FPS=1), no time limit.
# Usage: tools\windows\play.ps1 [-Fps30] [-Profile] [-NoNativeWorld] [-NoNativeCharacters] [-NoNativeTranslucent] [-NoNativeShadows] [-NoNativeLate] [-NoNativePost] [-Env 'A=1;B=2']
#        tools\windows\play.ps1 -Mode hybrid|gs|native [-Save <name>]   (bug checks, user 29 Oct)
#        -Mode hybrid = the default play build (as bugs were first seen); gs = every native switch off (the emulated
#        GS only: is it in the original?); native = every native part on (the default since 30 Oct, as hybrid).
#        -Save play_20261025_162246 loads game\ps2\disc\states\<name>.state 2 s after boot (no sound after a load).
#        The static world is drawn by the native Vulkan renderer (POP_NATIVE_WORLD=1, renderer step 1, in game
#        since 22 Oct); -NoNativeWorld leaves it all to the emulated GS. The opaque characters are native too
#        (POP_NATIVE_CHARACTERS=1, renderer step 2, judged in game by the user 22 Oct); -NoNativeCharacters
#        leaves them to the GS. The translucent lists before the shadows (+0x3C, +0x4C) are native too
#        (POP_NATIVE_TRANSLUCENT=1, offline A/B over 201 saves 23 Oct, 19.24 -> 18.23; in-game check pending);
#        -NoNativeTranslucent leaves them to the GS.
#        The projected shadows are native too (POP_NATIVE_SHADOWS=1; user check in game 24 Oct: floors right, lever
#        flicker gone); -NoNativeShadows leaves them to the GS.
#        The lists after the shadows (+0x88, +0x98, +0xA8), GFX particles, halos and the Pinch warp are native too
#        (POP_NATIVE_LATE=1, only while no earlier draw of the frame went to the GS; user check in game 30 Oct night:
#        sun halo, fountain glow, dagger glow, rewind; closes 0.13); -NoNativeLate leaves them to the GS.
#        The after-effects (glow, SmartBlur, MotionSmooth, FixBlend, ZoomSmooth) too, while the frame is pure
#        (POP_NATIVE_POST=1; user check 30 Oct evening: lamp hall and dagger halos right, N24 / N25); -NoNativePost leaves
#        them to the GS.
#        The 2D layer (HUD, menus, text, fades, letterbox, loading screen, movie picture) is native too
#        (POP_NATIVE_2D=1; user check on the TV 1 Nov, closes 0.14); -NoNative2D leaves it to the GS.
#        -Fps30 plays at the original 30 fps (to recheck something seen at 60).
#        -Profile: every kept quick-save also gets a profile of the seconds before it (see below); costs a
#        little speed itself, so it is a separate shortcut.
# Desktop shortcut (made by -Shortcuts): "PoP 60 fps" only (user, 22 Oct); -Shortcuts also removes the old
# "PoP 60 fps (profiling)" and "PoP 30 fps (recheck)" ones.
# Starts in borderless fullscreen with the debug panel hidden (PS2X_FULLSCREEN=1, PS2X_DEBUGGER=0; runtime
# patch 27): F11 toggles fullscreen, F1 shows the panel.
# While playing: Select (or F5) quick-saves; L3 + R3 (or F3) shows/hides the frame-time graph (patch 28); every quick-save is also kept as
# game\ps2\disc\states\play_<date>_<time>.state; with -Profile also play_<date>_<time>.prof: a profile of the ~12 s
# before the save (POP_PROFILE_LAST, rolling profiler), so a dip can be saved just after it happened. When the
# game is closed, each new .prof is symbolized to <name>.report.txt with this build's linker map.
# Esc, Alt+F4 or closing the window quits (Esc: runtime patch 39; PS2X_ESC_QUITS=0 turns it off). Avoid F9 (quick-load).
# Log: game\recomp-sotc\play_<date>_<time>.log, written when the game window is closed.
param([ValidateSet('', 'hybrid', 'gs', 'native')][string]$Mode = '', [string]$Save = '', [switch]$Fps30, [switch]$Profile, [switch]$NoNativeWorld, [switch]$NoNativeCharacters, [switch]$NoNativeTranslucent, [switch]$NoNativeShadows, [switch]$NoNativeLate, [switch]$NoNativePost, [switch]$NoNative2D, [string]$Env = '', [switch]$Shortcuts)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path

if ($Shortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    $desk = [Environment]::GetFolderPath('Desktop')
    foreach ($old in @('PoP 60 fps (profiling)', 'PoP 30 fps (recheck)')) { Remove-Item "$desk\$old.lnk" -ErrorAction SilentlyContinue }
    foreach ($s in @(@{ Name = 'PoP 60 fps'; Args = '' })) {
        $lnk = $shell.CreateShortcut("$desk\$($s.Name).lnk")
        $lnk.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
        $lnk.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File `"$PSCommandPath`"$($s.Args)"
        $lnk.WorkingDirectory = $Root
        $lnk.Save()
        "shortcut: $desk\$($s.Name).lnk"
    }
    return
}

if ($Mode -eq 'gs') { $NoNativeWorld = $NoNativeCharacters = $NoNativeTranslucent = $NoNativeShadows = $NoNativeLate = $NoNativePost = $NoNative2D = [switch]$true }
if ($Save) {
    if (-not (Test-Path "$Root\game\ps2\disc\states\$Save.state")) { throw "no save game\ps2\disc\states\$Save.state" }
    $Env = "PS2X_STATE_LOAD_AT=120:states/$Save.state;$Env"
}
$fps = $(if ($Fps30) { '0' } else { '1' })
$name = "play_$(Get-Date -Format 'yyyyMMdd_HHmmss')$(if ($Mode) { "_$Mode" })" # seconds: two boots in one minute overwrote a log (23 Oct)
& "$PSScriptRoot\boot.ps1" -Play -Out 'game\recomp-sotc' -Seconds 86400 -Name $name -Env "POP_60FPS=$fps;PS2X_FULLSCREEN=1;PS2X_DEBUGGER=0;POP_NATIVE_WORLD=$(if ($NoNativeWorld) { '0' } else { '1' });POP_NATIVE_CHARACTERS=$(if ($NoNativeCharacters) { '0' } else { '1' });POP_NATIVE_TRANSLUCENT=$(if ($NoNativeTranslucent) { '0' } else { '1' });POP_NATIVE_SHADOWS=$(if ($NoNativeShadows) { '0' } else { '1' });POP_NATIVE_LATE=$(if ($NoNativeLate) { '0' } else { '1' });POP_NATIVE_POST=$(if ($NoNativePost) { '0' } else { '1' });POP_NATIVE_2D=$(if ($NoNative2D) { '0' } else { '1' });$(if ($Profile) { 'POP_PROFILE_LAST=12;' })$Env"

$map = "$Root\game\recomp-sotc\runtime-src\build-win-play\ps2xRuntime\ps2EntryRunner.map"
foreach ($prof in Get-ChildItem "$Root\game\ps2\disc\states\play_*.prof" -ErrorAction SilentlyContinue) {
    $report = [IO.Path]::ChangeExtension($prof.FullName, '.report.txt')
    if (-not (Test-Path $report) -and (Test-Path $map)) {
        python -I "$Root\tools\profsym.py" $prof.FullName $map --top 30 | Set-Content -Encoding utf8 $report
    }
}
