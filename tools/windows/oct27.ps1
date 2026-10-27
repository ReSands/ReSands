# PC job (27 Oct, TV on): live A/B of the renderer audit's step 1 (docs/native_bugs.md, docs/renderer_audit.md):
# shader alpha test on the GS scale (texel x vertex alpha), AFAIL failure pass, dedupe key, stats lock.
# The changes are uncommitted in the working tree. "before" = those code files stashed, "after" = unstashed;
# each: -Fast build, then the three stnative saves for 45 s with R1 held and screenshots at fields 1200 / 2400.
# Logs and screens: game\recomp-sotc\oct27\<variant>_<save>.log and <variant>_<save>\.
# Run: tools\windows\start-job.ps1 -Script tools\windows\oct27.ps1
$Root = 'C:\dev\PoP_Decomp'
$R = "$Root\game\recomp-sotc"
$code = 'tools/native', 'tools/overrides'
Set-Location $Root
New-Item -ItemType Directory -Force -Path "$R\oct27" | Out-Null
$base = 'POP_60FPS=1;PS2X_FULLSCREEN=1;PS2X_DEBUGGER=0;POP_NATIVE_WORLD=1;POP_NATIVE_CHARACTERS=1;POP_NATIVE_TRANSLUCENT=1;POP_NATIVE_SHADOWS=1;POP_NATIVE_LATE=1;POP_INPUT=r1@1:3600'
foreach ($variant in 'before', 'after') {
    "== $variant"
    if ($variant -eq 'before') { git -c user.name=pop -c user.email=pop@local stash push -- @code; if ($LASTEXITCODE) { "STASH FAILED"; exit 1 } }
    else { git -c user.name=pop -c user.email=pop@local stash pop; if ($LASTEXITCODE) { "STASH POP FAILED (git stash list)"; exit 1 } }
    & "$Root\tools\windows\rebuild.ps1" -Fast -Play -RecompSrc C:\tools\PS2Recomp-sotc -Out game\recomp-sotc
    if ($LASTEXITCODE) {
        "BUILD FAILED"
        if ($variant -eq 'before') { git -c user.name=pop -c user.email=pop@local stash pop }
        exit 1
    }
    foreach ($state in 'play_20261024_184926', 'play_20261025_115837', 'play_20261025_115853') {
        "== $variant $state"
        Remove-Item "$R\screens\*" -ErrorAction SilentlyContinue
        & "$Root\tools\windows\boot.ps1" -Play -Out game\recomp-sotc -Seconds 45 -Name stnative -Shots '1200,2400' -Env "$base;PS2X_STATE_LOAD_AT=120:states/$state.state" | Out-Null
        $log = "$R\stnative.log"
        "fps: " + ((Select-String -Path $log -Pattern '\[pop:fps\] ([\d.]+)' | ForEach-Object { '{0:N0}' -f [double]$_.Matches[0].Groups[1].Value }) -join ' ')
        foreach ($p in '\[pop:world\] frames', 'late lists', 'shadows:', 'sprites \(P9', 'frames injected', 'unknown draws', 'not drawn') {
            Select-String -Path $log -Pattern $p | Select-Object -Last 1 | ForEach-Object { $_.Line }
        }
        Copy-Item $log "$R\oct27\${variant}_$state.log" -Force
        New-Item -ItemType Directory -Force -Path "$R\oct27\${variant}_$state" | Out-Null
        Move-Item "$R\screens\*" "$R\oct27\${variant}_$state\" -Force
    }
}
"JOB_DONE oct27"
