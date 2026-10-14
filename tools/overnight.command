#!/bin/zsh
# Double-click (Finder) or run: starts the overnight local-model job on the gaming PC.
# Safe to stop at any time (close this window, Ctrl+C, or turn the PC off): finished work is
# kept, and running this again continues where it stopped.
cd "${0:A:h}/.."
PC=10.0.0.202
LOG=game/analysis/naming/overnight.log
mkdir -p game/analysis/naming
echo "== $(date) overnight job starting" | tee -a $LOG

# 1. Ollama on the PC (a scheduled task, so it survives SSH ending)
ssh -o BatchMode=yes -o ConnectTimeout=10 $PC "schtasks /run /tn OllamaServe" >/dev/null 2>&1 \
    || { echo "PC not reachable over SSH; is it on?" | tee -a $LOG; read -k1 "?Press a key to close."; exit 1; }
sleep 8

# 2. Tunnel to Ollama (it only listens on the PC's 127.0.0.1)
ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -N -L 11434:127.0.0.1:11434 $PC &
TUNNEL=$!
trap 'kill $TUNNEL 2>/dev/null' EXIT
sleep 3

# 3. The job, with the Mac kept awake while it runs
caffeinate -i python3 -I tools/llm_port/name_functions.py 2>&1 | tee -a $LOG
echo "== $(date) overnight job ended" | tee -a $LOG
python3 -I tools/llm_port/name_functions.py --score 2>&1 | head -3 | tee -a $LOG
