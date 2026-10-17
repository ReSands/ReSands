# Cold-boot check before the 60 fps playthrough (17 Oct): the same scripted run to the balcony
# (title, opening movie, first steps) at 60 fps and at 30 fps, each with the sound dumped
# (PS2X_AUDIO_DUMP, raw 48 kHz 16-bit stereo after a 44-byte header that stays empty when the
# time limit kills the game) and the runtime's screenshots. Compare: [pop:fps] lines, sound
# timeline (movie and music length must match between the two), screenshots.
# Started through start-job.ps1 (window needs the desktop). Output: game\analysis\fps60\.
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
$out = "$Root\game\analysis\fps60"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$pad = 'start@55,start@65,cross@85,cross@95,cross@105,lup@345:4,cross@352,lleft@356:3,lup@360:5'
$shots = '6000,12000,18000,20400,21300,22200'
foreach ($run in @(@{ Name = 'fps60'; Fps = '1' }, @{ Name = 'fps30'; Fps = '0' })) {
    "== $(Get-Date -Format 'HH:mm') $($run.Name)"
    & "$Root\tools\windows\boot.ps1" -Play -Out 'game\recomp-sotc' -Seconds 400 -Name $run.Name -PadInput $pad -Shots $shots `
        -Env "POP_60FPS=$($run.Fps);POP_KEEP_SAVES=0;PS2X_AUDIO_DUMP=$out\$($run.Name).pcm"
    Copy-Item "$Root\game\recomp-sotc\$($run.Name).log" $out -Force
    New-Item -ItemType Directory -Force -Path "$out\$($run.Name)_screens" | Out-Null
    Get-ChildItem "$Root\game\recomp-sotc\screens" -File | Move-Item -Destination "$out\$($run.Name)_screens" -Force
    Select-String -Path "$out\$($run.Name).log" -Pattern '\[pop:fps\]' | Select-Object -Last 3 | ForEach-Object { $_.Line }
}
"== $(Get-Date -Format 'HH:mm') done"
