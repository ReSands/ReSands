#!/bin/zsh
# Double-click before bed: sends the latest commits to the gaming PC, asks which local-model job
# to run, and starts it THERE (tools/windows/llm-job.ps1 via start-job.ps1), then exits. The Mac
# can be switched off afterwards; the PC must stay on. Morning: tools/overnight-pc.command --status
# (or ask Claude). Stop a running job: tools/overnight-pc.command --stop (or double-click
# tools/stop-pc-job.command). The PC must not be running the game or a build when you press this.
cd "${0:A:h}/.."
PC=10.0.0.202
REMOTE='C:\dev\PoP_Decomp'
ps() { ssh -o BatchMode=yes -o ConnectTimeout=10 $PC "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $(iconv -f UTF-8 -t UTF-16LE <<<"$1" | base64)" 2>&1 | grep -v 'CLIXML\|<Objs\|post-quantum\|store now\|upgraded'; }

if [[ "$1" == "--stop" ]]; then
    # Ending the scheduled task stops only its PowerShell wrapper; the job's Python processes
    # (naming, mesh workers) would keep running, so stop them too, and a game a boot had started
    # (an orphan keeps the exe locked, so the next build fails to link: 19 Oct).
    ps "Stop-ScheduledTask -TaskName PoP_Decomp_Job -ErrorAction SilentlyContinue
        Stop-Process -Name ps2EntryRunner -Force -ErrorAction SilentlyContinue
        Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { \$_.CommandLine -match 'PoP_Decomp|multiprocessing' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep 2
        'job: ' + (Get-ScheduledTask -TaskName PoP_Decomp_Job).State + '; python processes left: ' + @(Get-Process python -ErrorAction SilentlyContinue).Count"
    echo "(Press any key to close.)"; read -k1
    exit 0
fi
if [[ "$1" == "--status" ]]; then
    ps "& '$REMOTE\\tools\\windows\\start-job.ps1' -Status | Select-Object -Last 25"
    exit 0
fi

echo "== Syncing commits to the PC"
pc_head=$(ps "git -C '$REMOTE' rev-parse HEAD" | tr -d '\r' | tail -1)
if ! git cat-file -e "$pc_head" 2>/dev/null; then
    echo "The PC is at $pc_head, which this Mac does not know. Stopping."; read -k1; exit 1
fi
if [[ "$pc_head" != "$(git rev-parse HEAD)" ]]; then
    git bundle create /tmp/pop-overnight.bundle "$pc_head..main" >/dev/null 2>&1 &&
    scp -q -o BatchMode=yes /tmp/pop-overnight.bundle "$PC:C:/dev/PoP_Decomp/game/jobs/pop.bundle" &&
    ps "git -C '$REMOTE' pull --ff-only game\\jobs\\pop.bundle main 2>&1 | Select-Object -Last 1; git -C '$REMOTE' log --oneline -1"
fi

echo
echo "Which job should the PC run overnight?"
echo "  1) MMI instruction fixes, with documented behaviour (judged by the instruction test)"
echo "  2) MMI instruction fixes, from the instruction name only"
echo "  3) Function naming suggestions"
echo "  4) Nightly checks + bug report (instruction test, all world packs, boot test; Gemma writes docs/nightly)"
echo "  5) Nightly checks, then function naming (v2: thinking + context; resumes each night)"
read -k1 "choice?Choice [1-5]: "; echo
case $choice in
    1) args='-Job mmi -Spec -Tries 5' ;;
    2) args='-Job mmi -Tries 5' ;;
    3) args='-Job naming' ;;
    4) args='-Job nightly' ;;
    5) args='-Job nightly -ThenNaming' ;;
    *) echo "No job started."; read -k1; exit 1 ;;
esac
echo "== Starting the overnight job on the PC ($args)"
ps "Set-Location '$REMOTE'; & tools\\windows\\start-job.ps1 -Script tools\\windows\\llm-job.ps1 -Arguments '$args'"
echo
echo "Started. You can switch this Mac off now; the PC must stay on."
echo "(Press any key to close.)"
read -k1
