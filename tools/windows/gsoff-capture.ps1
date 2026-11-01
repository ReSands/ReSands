# 1 Nov: 0.2 step 4b (docs/gs_off.md "Step 4b plan"): GS-only saves mid-fight / mid-effect, scored offline on the Mac
# (tools/native/score_saves.sh: native frame vs the save's own picture, same frame by construction). No build.
# Each scene: its play_ save loaded with every native switch off (-Mode gs), Select quick-saves ~1, 4, 10 s after the
# load (load at field 120, ~3 s after boot), R1 held in between (presses are sequential, so R1 lets go for each Select).
# Kept copies (states\play_<time>.state) go to game\jobs\gsoffcap\<scene>_<n>.gs.state.
Set-Location C:\dev\PoP_Decomp
"DISPLAY $(nvidia-smi --query-gpu=display_active --format=csv,noheader)"
$R = 'C:\dev\PoP_Decomp\game\recomp-sotc'
$D = 'C:\dev\PoP_Decomp\game\ps2\disc\states'
$out = 'C:\dev\PoP_Decomp\game\jobs\gsoffcap'
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$D\quick.state" "$out\quick.state.bak" -Force   # the scripted saves overwrite the user's quick.state
$gs = 'POP_60FPS=1;PS2X_FULLSCREEN=1;PS2X_DEBUGGER=0;POP_NATIVE_WORLD=0;POP_NATIVE_CHARACTERS=0;POP_NATIVE_TRANSLUCENT=0;POP_NATIVE_SHADOWS=0;POP_NATIVE_LATE=0;POP_NATIVE_POST=0;POP_NATIVE_2D=0'
$pad = 'r1@1:2.9,select@4,r1@4.9:2,select@7,r1@7.9:5,select@13,r1@13.9:3600'
# fights 184926 134636 115837 202314 185602; sand haze 115853; rewind 155110; halos 162246; fountain water 202926
foreach ($s in @('20261024_184926', '20261025_134636', '20261025_115837', '20261024_202314', '20261019_185602',
                 '20261025_115853', '20261025_155110', '20261025_162246', '20261029_202926')) {
    "== $s $(Get-Date)"
    if (-not (Test-Path "$D\play_$s.state")) { "$s MISSING play_$s.state"; continue }
    $t0 = Get-Date
    $name = "gsoffcap_$s"
    & tools\windows\boot.ps1 -Play -Out game\recomp-sotc -Seconds 18 -Name $name -Env "$gs;POP_INPUT=$pad;PS2X_STATE_LOAD_AT=120:states/play_$s.state" | Out-Null
    $new = @(Get-ChildItem "$D\play_*.state" | Where-Object { $_.LastWriteTime -gt $t0 } | Sort-Object LastWriteTime)
    for ($i = 0; $i -lt $new.Count; $i++) { Move-Item $new[$i].FullName "$out\$($s)_$($i + 1).gs.state" -Force; "$s save $($i + 1) from $($new[$i].Name)" }
    if ($new.Count -ne 3) { "$s WARNING $($new.Count) saves (expected 3)" }
    Select-String -Path "$R\$name.log" -Pattern 'State saved|Exception|crash' | ForEach-Object { $_.Line }
    Copy-Item "$R\$name.log" $out
}
Copy-Item "$out\quick.state.bak" "$D\quick.state" -Force
"JOB_DONE gsoffcap"
