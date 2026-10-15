#!/bin/bash
# Overnight queue for the local model (Qwen on the PC): runs every task in order, up to ROUNDS rounds
# each, moving on when a task fails; then a second pass over the tasks that have not passed. The PC
# must be on. Double-click, or: tools/qwen-queue.command [rounds per task, default 15]
# Results: game/analysis/qwen/<task>/ (port.log, attempts/, best.cpp, PASS) and
# game/analysis/movie/stage1/ for m1. Summary at the end and in game/analysis/qwen/queue.log.
cd "$(dirname "$0")/.."
ROUNDS=${1:-15}
PC=10.0.0.202
TASKS="m1 materials gao tex8 lightmap chain"
LOG=game/analysis/qwen/queue.log
mkdir -p game/analysis/qwen
say() { echo "$(date '+%H:%M:%S') $*" | tee -a $LOG; }
if ! ssh -o ConnectTimeout=8 $PC echo ok >/dev/null 2>&1; then echo "The PC ($PC) is not reachable: switch it on first."; read -p "Press Enter."; exit 1; fi
ssh $PC "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dev\\PoP_Decomp\\tools\\windows\\llama-serve.ps1" 2>/dev/null | grep -v -i "quantum\|store now\|upgraded"
ssh -N -o ServerAliveInterval=30 -L 8033:127.0.0.1:8033 $PC 2>/dev/null &
TUNNEL=$!
trap 'kill $TUNNEL 2>/dev/null' EXIT
say "queue start: $TASKS ($ROUNDS rounds each)"
for i in $(seq 1 120); do curl -s http://127.0.0.1:8033/health | grep -q '"ok"' && break; sleep 5; done
passed() { if [ "$1" = m1 ]; then [ -f game/analysis/movie/stage1/movie.cpp ]; else [ -f game/analysis/qwen/$1/PASS ]; fi; }
for pass in 1 2; do
  for T in $TASKS; do
    passed $T && continue
    say "pass $pass: $T"
    if [ "$T" = m1 ]; then caffeinate -i python3 -I tools/llm_port/movie/port.py --rounds $((ROUNDS * pass))
    else caffeinate -i python3 -I tools/llm_port/generic/port.py $T --rounds $((ROUNDS * pass)); fi
    passed $T && say "$T PASSED" || say "$T not passed yet"
  done
done
say "queue done:"; for T in $TASKS; do passed $T && say "  $T: PASS" || say "  $T: not passed"; done
read -p "Press Enter to close."
