#!/bin/zsh
# Double-click before bed: sends the latest commits to the gaming PC and starts the overnight
# local-model job THERE (tools/windows/llm-mmifix.ps1 via start-job.ps1), then exits. The Mac
# can be switched off afterwards; the PC must stay on. Morning: tools/overnight-pc.command --status
# (or ask Claude). The PC must not be running the game or a build when you press this.
cd "${0:A:h}/.."
PC=10.0.0.202
REMOTE='C:\dev\PoP_Decomp'
ps() { ssh -o BatchMode=yes -o ConnectTimeout=10 $PC "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $(iconv -f UTF-8 -t UTF-16LE <<<"$1" | base64)" 2>&1 | grep -v 'CLIXML\|<Objs\|post-quantum\|store now\|upgraded'; }

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
    git bundle create /tmp/pop-overnight.bundle "$pc_head..HEAD" >/dev/null 2>&1 &&
    scp -q -o BatchMode=yes /tmp/pop-overnight.bundle "$PC:C:/dev/PoP_Decomp/game/jobs/pop.bundle" &&
    ps "git -C '$REMOTE' pull --ff-only game\\jobs\\pop.bundle main 2>&1 | Select-Object -Last 1"
fi

echo "== Starting the overnight job on the PC"
ps "Set-Location '$REMOTE'; & tools\\windows\\start-job.ps1 -Script tools\\windows\\llm-mmifix.ps1 -Arguments '-Tries 3'"
echo
echo "Started. You can switch this Mac off now; the PC must stay on."
echo "(Press any key to close.)"
read -k1
