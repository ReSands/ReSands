# Starts llama.cpp's server with Qwen3.6-35B-A3B (settings from a 5070 Ti + 32 GB tuning write-up:
# experts split CPU/GPU automatically by --fit with 128K context) as the scheduled task LlamaServe,
# so it survives the SSH session. Listens on 127.0.0.1:8033 (tunnel from the Mac:
# ssh -N -L 8033:127.0.0.1:8033 10.0.0.202). -Stop ends it. Log: game\jobs\llama.log
param([switch]$Stop, [string]$Model = 'D:\models\Qwen3.6-35B-A3B-UD-Q4_K_M.gguf', [string]$Exe = 'D:\llama.cpp\llama-server.exe')
$Task = 'LlamaServe'
if ($Stop) { Stop-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue; Get-Process llama-server -ErrorAction SilentlyContinue | Stop-Process -Force; 'stopped'; return }
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$cmdFile = "$Root\game\jobs\llama-serve.cmd"
@"
@echo off
"$Exe" -m "$Model" --fit on --fit-ctx 128000 --fit-target 256 -np 1 -fa on --load-mode mlock -b 2048 -ub 2048 -ctk q8_0 -ctv q8_0 --temp 0.6 --top-p 0.95 --top-k 20 --min-p 0.0 --presence-penalty 0.0 --repeat-penalty 1.0 --reasoning-budget 16000 --reasoning-budget-message "Thinking budget reached: stop reasoning now and write the complete file in one cpp code block." --chat-template-kwargs "{\"preserve_thinking\": true}" --host 127.0.0.1 --port 8033 > "$Root\game\jobs\llama.log" 2>&1
"@ | Set-Content -Encoding ascii $cmdFile
$action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$cmdFile`""
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 24) -AllowStartIfOnBatteries -Priority 4
Register-ScheduledTask -TaskName $Task -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $Task
"started $Task; log $Root\game\jobs\llama.log"
