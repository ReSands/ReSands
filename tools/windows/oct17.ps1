# PC job (17 Oct evening): play build with today's changes, a smoke boot, then the profiles of the
# slow saves.
#   0. runtime patches 27 (fullscreen / hidden panel start options) and 28 (frame-time graph) applied if missing;
#   1. fast play build (overrides: Select quick-save, profiler, live save-state/lightmap paths;
#      runtime patch 27; the play build now links with /MAP, reconfigured once);
#   2. smoke boot: cold boot 90 s at 60 fps with the start options (fullscreen, panel hidden) and the
#      frame-time graph shown, screenshots at 50 s and 80 s (game\recomp-sotc\screens);
#   3. tools\windows\profile-states.ps1 -NoBuild: the sand-monster fight, the pool, a 60 fps scene.
# Started through start-job.ps1 (log: game\jobs\job.log).
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
# runtime patch 27 (start options) into the pop-sotc runtime, then the build's runtime worktree to it
$Rt = 'C:\tools\PS2Recomp-sotc'
foreach ($p in @(@{ N = 27; File = '27-fullscreen-debugger-start-options.patch'; Mark = 'Start options: PS2X_FULLSCREEN' },
                 @{ N = 28; File = '28-frame-time-graph.patch'; Mark = 'Frame-time graph overlay' })) {
    if (-not (git -C $Rt log --oneline -20 | Select-String $p.Mark)) {
        "== applying runtime patch $($p.N)"
        git -C $Rt -c user.name=Jon -c user.email=resands@proton.me am "$Root\tools\patches\pop-sotc\$($p.File)"
        if ($LASTEXITCODE -ne 0) { git -C $Rt am --abort; "PATCH $($p.N) FAILED"; exit 1 }
    }
}
git -C "$Root\game\recomp-sotc\runtime-src" checkout -q --detach (git -C $Rt rev-parse HEAD)
"runtime at $(git -C "$Root\game\recomp-sotc\runtime-src" log --oneline -1)"
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') fast play build"
& "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc
if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($LASTEXITCODE)"; exit 1 }
$map = "$Root\game\recomp-sotc\runtime-src\build-win-play\ps2xRuntime\ps2EntryRunner.map"
"linker map: $(if (Test-Path $map) { '{0:N0} bytes' -f (Get-Item $map).Length } else { 'MISSING' })"
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') smoke boot (90 s, 60 fps, fullscreen, panel hidden)"
& "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 90 -Name smoke_oct17 -Shots '3000,4800' -Env 'POP_60FPS=1;PS2X_FULLSCREEN=1;PS2X_DEBUGGER=0;PS2X_FRAME_GRAPH=1' | Out-Null
$log = "$Root\game\recomp-sotc\smoke_oct17.log"
"fps: $((Select-String -Path $log -Pattern '\[pop:fps\] ([\d.]+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ' ')"
"movies: $((Select-String -Path $log -Pattern '\[pop:movie\] \S+ ->').Count)"
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') profiles"
& "$Root\tools\windows\profile-states.ps1" -NoBuild
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') job done"
