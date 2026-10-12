# Windows version of tools/boot.sh: boot the recompiled game from the disc folder with a time limit.
# Usage: tools\windows\boot.ps1 [-Seconds 90] [-Name boot] [-PadInput 'start@55,cross@85,lleft@150:5'] [-Play]
#        -Play -Release runs the Release (LTCG) play build from rebuild.ps1 -Play -Release.
#        -Play runs the log-free play build (rebuild.ps1 -Play), about 1.5x faster, with the GS
#        rasterizer on its own thread (POP_ASYNC_GS=1, another ~1.3x; -NoAsyncGs turns it off).
#        -PadInput sets POP_INPUT (scripted pad input, see tools/overrides/pop_slus20743.cpp).
#        Frame dumps go to game\recomp\screens.
# Log: game\recomp\<Name>.log (stdout then stderr). "timeout" means the time limit was hit.
# Run it from a desktop session (Claude Code on the PC): over SSH there is no desktop for the window.
param([int]$Seconds = 90, [string]$Name = 'boot', [string]$PadInput = '', [switch]$Play, [switch]$NoAsyncGs, [switch]$Release, [string]$Out = 'game\recomp', [string]$Shots = '', [string]$Env = '')
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$dir  = if ($Play -and $Release) { 'build-win-play-rel' } elseif ($Play) { 'build-win-play' } else { 'build-win' }
$R    = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { "$Root\$Out" }
$Exe  = "$R\runtime-src\$dir\ps2xRuntime\ps2EntryRunner.exe"
$Log  = "$R\$Name.log"
$StdOut = "$Log.stdout"
$Err  = "$Log.stderr"

# -Env 'A=1;B=2' sets extra environment variables for the game (applied first, so they win).
foreach ($pair in ($Env -split ';')) { if ($pair -match '^\s*([^=]+)=(.*)$') { Set-Item "env:$($Matches[1].Trim())" $Matches[2] } }
# Runtime settings the SotC port documents (Docs/CONFIGURATION.md), for the pop-sotc setup:
# GPU GS renderer on its own thread, VU1 on its own thread, compiled VU1 microcode, SIMD paths.
# Values already set in the environment win.
if ($Out -ne 'game\recomp') {
    $sotc = [ordered]@{ PS2X_GS_GPU = '1'; PS2X_GS_THREAD = '1'; PS2X_GS_DIRECT_PRESENT = '1'; PS2X_GS_COMPACT_QUEUE = '1';
                        PS2X_GS_QUEUE_CHUNKS = '8'; PS2X_VIF_SIMD_UNPACK = '1'; PS2X_VU1_SIMD_CLIP = '1'; PS2X_MTVU = '1';
                        PS2X_VU1_RECOMP = '1';
                        # Camera stick: modern horizontal direction (PoP orbits the camera the other way
                        # and has no option for it). -Env 'PS2X_INVERT_RIGHT_STICK=0' restores the original.
                        PS2X_INVERT_RIGHT_STICK = 'x' }
    foreach ($k in $sotc.Keys) { if (-not [Environment]::GetEnvironmentVariable($k)) { Set-Item "env:$k" $sotc[$k] } }
}
New-Item -ItemType Directory -Force -Path "$Root\game\recomp\screens" | Out-Null
# -Shots '1800,3600': the SotC runtime's own window screenshots at those vsync counts (~60/s),
# saved to <Out>\screens (our override's frame dumper does not see the GPU renderer's output).
if ($Shots) {
    New-Item -ItemType Directory -Force -Path "$R\screens" | Out-Null
    $env:PS2X_SCREENSHOT_FIELDS = $Shots
    $env:PS2X_SCREENSHOT_DIR = "$R\screens"
}
$env:POP_INPUT = $PadInput
# The override's async GS needs its GIF tap, which would bypass a runtime's own GS pipeline
# (the SotC fork has one), so only the original setup (game\recomp) uses it.
$async = $Play -and -not $NoAsyncGs -and ($Out -eq 'game\recomp')
$env:POP_ASYNC_GS = $(if ($async) { '1' } else { '0' })
$env:POP_GIF_TAP  = $(if ($async) { '1' } else { '0' })
$p = Start-Process -FilePath $Exe -ArgumentList '.\SLUS_207.43' -WorkingDirectory "$Root\game\ps2\disc" `
        -RedirectStandardOutput $StdOut -RedirectStandardError $Err -PassThru -NoNewWindow
$result = 'exited'
if (-not $p.WaitForExit($Seconds * 1000)) {
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit()
    $result = 'timeout'
}
Get-Content $StdOut, $Err | Set-Content $Log
Remove-Item $StdOut, $Err
$lines = (Get-Content $Log | Measure-Object -Line).Lines
"result=$result exit=$($p.ExitCode) lines=$lines log=$Log"
