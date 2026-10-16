#!/bin/zsh
# Double-click before bed (16 Oct): wakes the gaming PC if needed, sends it the latest commits and
# starts tonight's job there (tools/windows/oct16.ps1: native frames of the gameplay1 and enemies
# save states with the new build, then the nightly checks). Then it exits; the Mac can be switched
# off, the PC must stay on and logged in (the game window needs the desktop). Nothing must be
# running on the PC (game, build, another job).
# Morning: tools/oct16.command --status (or ask Claude). Stop: tools/stop-pc-job.command.
cd "${0:A:h}/.."
PC=10.0.0.202
MAC_ADDR=b42e99c03550
REMOTE='C:\dev\PoP_Decomp'
ps() { ssh -o BatchMode=yes -o ConnectTimeout=10 $PC "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $(iconv -f UTF-8 -t UTF-16LE <<<"$1" | base64)" 2>&1 | grep -v 'CLIXML\|<Objs\|post-quantum\|store now\|upgraded'; }
up() { ssh -o BatchMode=yes -o ConnectTimeout=8 $PC echo ok 2>/dev/null | grep -q ok; }

if [[ "$1" == "--status" ]]; then
    ps "& '$REMOTE\\tools\\windows\\start-job.ps1' -Status | Select-Object -Last 30"
    exit 0
fi

if ! up; then
    echo "== The PC is not answering; sending a wake-on-LAN packet and waiting up to 3 minutes"
    python3 -I -c "
import socket; p=b'\xff'*6+bytes.fromhex('$MAC_ADDR')*16
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET,socket.SO_BROADCAST,1)
[s.sendto(p,(a,9)) for a in ('255.255.255.255','10.0.0.255')]"
    for i in {1..36}; do up && break; sleep 5; done
    if ! up; then echo "The PC did not wake up. Switch it on, log in, and run this again."; read -k1; exit 1; fi
    echo "PC is up; giving it a minute to finish logging in"; sleep 60
fi

echo "== Syncing commits to the PC"
pc_head=$(ps "git -C '$REMOTE' rev-parse HEAD" | tr -d '\r' | tail -1)
if ! git cat-file -e "$pc_head" 2>/dev/null; then
    echo "The PC is at $pc_head, which this Mac does not know. Stopping."; read -k1; exit 1
fi
if [[ "$pc_head" != "$(git rev-parse main)" ]]; then
    git bundle create /tmp/pop-oct16.bundle "$pc_head..main" >/dev/null 2>&1 &&
    scp -q -o BatchMode=yes /tmp/pop-oct16.bundle "$PC:C:/dev/PoP_Decomp/game/jobs/pop.bundle" &&
    ps "git -C '$REMOTE' pull --ff-only game\\jobs\\pop.bundle main 2>&1 | Select-Object -Last 1; git -C '$REMOTE' log --oneline -1"
fi

echo "== Starting tonight's job on the PC (native scenes, then nightly checks)"
ps "Set-Location '$REMOTE'; & tools\\windows\\start-job.ps1 -Script tools\\windows\\oct16.ps1"
echo
echo "Started. You can switch this Mac off now; the PC must stay on."
echo "(Press any key to close.)"
read -k1
