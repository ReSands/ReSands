#!/bin/zsh
# Watches the PC job started by tools/windows/start-job.ps1 and prints one line per event worth
# acting on (meant for Claude Code's Monitor tool): stage changes, build progress every few
# minutes, error lines, stalls, and the end of the job. Exits when the job ends or stops.
#
# Stall = neither the job log nor the build log changed for STALL_MIN minutes while the PC's
# CPU is under 15% (a hang, not a slow single-threaded step).
PC=${PC:-10.0.0.202}
POLL=30
STALL_MIN=${STALL_MIN:-5}
PROGRESS_MIN=5

probe=$(iconv -f UTF-8 -t UTF-16LE <<'EOF' | base64
$ProgressPreference = 'SilentlyContinue'
$job = 'C:\dev\PoP_Decomp\game\jobs\job.log'
$lines = if (Test-Path $job) { @(Get-Content $job) } else { @() }
$stage = ($lines | Where-Object { $_ -like '== *' } | Select-Object -Last 1)
$last  = ($lines | Where-Object { $_ -notmatch '^\s*$' } | Select-Object -Last 1)
$done  = ($lines | Where-Object { $_ -like 'JOB_DONE*' } | Select-Object -Last 1)
$errl  = @($lines | Where-Object { $_ -cmatch 'FAILED|TIMEOUT|JOB ERROR|Press any key|Exception' })
$errs  = $errl.Count
$errline = ($errl | Select-Object -Last 1)
$b = 'C:\dev\PoP_Decomp\game\recomp\runtime_build.log.out'
$build = if (Test-Path $b) { Get-Content $b -Tail 1 } else { '' }
$state = (Get-ScheduledTask -TaskName PoP_Decomp_Job -ErrorAction SilentlyContinue).State
$cpu = (Get-CimInstance Win32_Processor).LoadPercentage
"STATE=$state`tCPU=$cpu`tERRS=$errs`tERRLINE=$errline`tSTAGE=$stage`tLAST=$last`tDONE=$done`tBUILD=$build"
EOF
)

prev_stage="" prev_sig="" same=0 fails=0 last_progress=$SECONDS seen_errs=0
while true; do
    out=$(ssh -o BatchMode=yes -o ConnectTimeout=10 $PC "powershell -NoProfile -EncodedCommand $probe" 2>/dev/null \
          | grep '^STATE=' | tail -1)
    if [[ -z $out ]]; then
        (( ++fails >= 3 )) && { echo "SSH: PC unreachable for $((fails * POLL))s"; fails=0; }
        sleep $POLL; continue
    fi
    fails=0
    field() { print -r -- ${${(M)${(s:	:)out}:#$1=*}#$1=} }
    state=$(field STATE) cpu=$(field CPU) errs=$(field ERRS) errline=$(field ERRLINE) stage=$(field STAGE)
    last=$(field LAST) done=$(field DONE) build=$(field BUILD)

    if [[ $stage != $prev_stage && -n $stage ]]; then echo "stage: $stage"; prev_stage=$stage; fi
    if (( errs > seen_errs )); then echo "ERROR line in job log: $errline"; seen_errs=$errs; fi
    if [[ -n $done ]]; then echo "DONE: $done"; exit 0; fi
    if [[ $state != Running ]]; then echo "STOPPED: task state '$state' without JOB_DONE; last line: $last"; exit 1; fi

    sig="$last|$build"
    if [[ $sig == $prev_sig ]]; then (( same++ )); else same=0; prev_sig=$sig; fi
    if (( same * POLL >= STALL_MIN * 60 && cpu < 15 )); then
        echo "STALL: no log change for ${STALL_MIN} min, CPU ${cpu}%, stage '$stage', last: $last"
        same=0
    fi
    if [[ -n $build && $stage == *'Build runtime'* ]] && (( SECONDS - last_progress >= PROGRESS_MIN * 60 )); then
        echo "progress: ${build[1,60]} (CPU ${cpu}%)"; last_progress=$SECONDS
    fi
    sleep $POLL
done
