# Overnight job (16 Oct): native frames of more scenes, then the nightly checks.
#   1. tools\windows\native-scenes.ps1: fast play build with the current overrides (haze export,
#      point-light sets, per-draw side file), then save-state boots (gameplay1, enemies) that export
#      a native frame each plus the runtime's own screenshots around it;
#   2. tools\windows\live-window.ps1: the live native window (viewer + gameplay1 streaming to it),
#      with the viewer's screenshots every 10 s and the runtime's own;
#   3. tools\windows\llm-job.ps1 -Job nightly: instruction test, boot smoke test with the new build,
#      world-pack coverage (resumes), Gemma's report in docs\nightly.
# Started by tools/oct16.command through start-job.ps1 (log: game\jobs\job.log).
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $Root
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') native scenes"
& "$Root\tools\windows\native-scenes.ps1"
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') live native window"
& "$Root\tools\windows\live-window.ps1" -State gameplay1
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') nightly checks"
& "$Root\tools\windows\llm-job.ps1" -Job nightly
"== $(Get-Date -Format 'yyyy-MM-dd HH:mm') tonight done"
