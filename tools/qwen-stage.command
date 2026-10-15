#!/bin/bash
# Runs one stage of the Qwen mesh-decoder port (tools/llm_port/meshdecode): starts llama-server on
# the PC (task LlamaServe), tunnels to it, waits until the model is loaded, keeps the Mac awake and
# runs port.py. Usage: double-click (asks for the stage), or tools/qwen-stage.command <stage> [rounds]
# Results: game/analysis/meshdecode/stage<N>/ (port.log, attempts/, meshdecode.cpp = best so far).
cd "$(dirname "$0")/.."
STAGE=${1:-}
if [ -z "$STAGE" ]; then read -p "Stage(s), e.g. 3 or 4,5 or 4-5 (each runs only if the previous passed): " STAGE; fi
case "$STAGE" in *-*) STAGES=$(seq ${STAGE%-*} ${STAGE#*-});; *) STAGES=${STAGE//,/ };; esac
ROUNDS=${2:-15}
PC=10.0.0.202
echo "Starting llama-server on the PC..."
ssh $PC "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\dev\\PoP_Decomp\\tools\\windows\\llama-serve.ps1" 2>/dev/null | grep -v -i "quantum\|store now\|upgraded"
ssh -N -L 8033:127.0.0.1:8033 $PC 2>/dev/null &
TUNNEL=$!
trap 'kill $TUNNEL 2>/dev/null' EXIT
echo "Waiting for the model to load (a minute or two)..."
for i in $(seq 1 120); do
  curl -s http://127.0.0.1:8033/health | grep -q '"ok"' && break
  sleep 5
done
curl -s http://127.0.0.1:8033/health; echo
for S in $STAGES; do
  echo "=== stage $S"
  if ! caffeinate -i python3 -I tools/llm_port/meshdecode/port.py --stage "$S" --rounds "$ROUNDS"; then
    echo "Stage $S did not pass; stopping. Log: game/analysis/meshdecode/stage$S/port.log"
    break
  fi
  echo "Stage $S passed. Log: game/analysis/meshdecode/stage$S/port.log"
done
echo "To stop the model on the PC: ssh $PC powershell -File C:\\dev\\PoP_Decomp\\tools\\windows\\llama-serve.ps1 -Stop"
read -p "Press Enter to close."
