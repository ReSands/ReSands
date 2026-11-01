# 1 Nov (i): 0.2 step 4, native vs GS. Each GS-only gate save (+ step 2's HUD / save menu / loading saves, cold boot):
# A = every native switch off (play.ps1 -Mode gs), B = play.ps1 defaults + POP_GS_OFF=1 (-GsOff). No build.
# Shots 1 s, 4 s, 10 s after the load (field = the save's own vsync counter + N).
Set-Location C:\dev\PoP_Decomp
"DISPLAY $(nvidia-smi --query-gpu=display_active --format=csv,noheader)"
$R = 'C:\dev\PoP_Decomp\game\recomp-sotc'
$D = 'C:\dev\PoP_Decomp\game\ps2\disc\states'
Get-ChildItem game\jobs\gsref\*.gs.state | ForEach-Object { Copy-Item $_.FullName "$D\gsref_$($_.Name -replace '\.gs\.state$', '').state" }
$nat = 'POP_60FPS=1;PS2X_FULLSCREEN=1;PS2X_DEBUGGER=0;POP_NATIVE_WORLD={0};POP_NATIVE_CHARACTERS={0};POP_NATIVE_TRANSLUCENT={0};POP_NATIVE_SHADOWS={0};POP_NATIVE_LATE={0};POP_NATIVE_POST={0};POP_NATIVE_2D={0}'
$modes = @(@('gs', ($nat -f '0')), @('off', (($nat -f '1') + ';POP_GS_OFF=1')))
$out = "$R\shots1101i"
New-Item -ItemType Directory -Force $out | Out-Null
function Run($tag, $secs, $shots, $envs) {
    "== $tag $(Get-Date)"
    Remove-Item "$R\screens\*" -ErrorAction SilentlyContinue
    $name = "gsoff_$tag"
    & tools\windows\boot.ps1 -Play -Out game\recomp-sotc -Seconds $secs -Name $name -Shots $shots -Env $envs | Out-Null
    Get-ChildItem "$R\screens\field-*.png" -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName "$out\$($tag)_$($_.Name)"; "shot $($tag)_$($_.Name)" }
    $fps = (Select-String -Path "$R\$name.log" -Pattern '\[pop:fps\]' | ForEach-Object { [math]::Round([double]($_.Line -split ' ')[1]) }) -join ' '
    "fps $fps"
    foreach ($p in 'GS off', 'no native picture', 'unexpected buffers', 'frames injected', 'late lists', 'Exception', 'crash') {
        Select-String -Path "$R\$name.log" -Pattern $p | Select-Object -Last 2 | ForEach-Object { $_.Line }
    }
    Copy-Item "$R\$name.log" $out
}
$saves = @(
    @('gsref_20261024_184926', 0x7074), @('gsref_20261024_201540', 0x13cee), @('gsref_20261024_202314', 0x19be8),
    @('gsref_20261025_115837', 0x4d97), @('gsref_20261025_115853', 0x5176), @('gsref_20261025_125052', 0x4089),
    @('gsref_20261025_134636', 0x19b3), @('gsref_20261025_145139', 0x33ca), @('gsref_20261025_155110', 0x7b66),
    @('gsref_20261025_162246', 0x224fb), @('gsref_20261025_162621', 0x25764), @('gsref_20261029_202926', 0x8b91),
    @('gsref_20261030_174131', 0x3cdf),
    @('play_20261019_185602', 36792), @('play_20261019_192114', 126310), @('play_20261030_192103', 3635))
foreach ($m in $modes) {
    Run "cold_$($m[0])" 40 '600,900,1200,1500,1800,2100' $m[1]
    foreach ($t in $saves) {
        Run "$($t[0])_$($m[0])" 18 "$($t[1] + 60),$($t[1] + 240),$($t[1] + 600)" "$($m[1]);POP_INPUT=r1@1:3600;PS2X_STATE_LOAD_AT=120:states/$($t[0]).state"
    }
}
Remove-Item "$D\gsref_*.state"
"JOB_DONE gsoff1101i"
