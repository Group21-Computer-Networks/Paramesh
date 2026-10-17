#!/usr/bin/env bash
# The M5 fairness experiments (M5-5), on daemons on this machine:
#
#   tools/bench/fairness.sh [-s size] [-c cores] [-r rounds] [-b build directory] [experiment...]
#
#   jain       three nodes each start a matrix multiply on all three at once; prints each job's
#              time and Jain's index over their throughputs (1 is perfectly even)
#   freerider  three contributors and a fourth node that never runs anyone else's work; in each
#              round every node starts a job at once; prints the free-rider's weight at node 1
#              and its share of the round's throughput, round by round
#   latency    faultlat on three nodes, first alone, then beside two matrix multiplies;
#              prints the two distributions of remote page-fault times
#
#   -s   matrix size (default 1024)
#   -t   threads each job asks for on each node (default 1): with --cap-cores 2 a node's own
#        job leaves one slot, which the guests share by weight
#   -c   --cap-cores of every daemon (default 2)
#   -r   rounds of the free-rider experiment (default 10)
#   -b   build directory (default build/dev) with src/pmd/pmd, src/tools/pmrun and the apps
#
# With no experiment, all three run. The free-rider experiment sets ledger.scale to 10
# core-seconds (PARAMESH_CFG_LEDGER_SCALE) so that weights move within minutes; the default is
# 3,600. Exit status 0, or 1 if a run failed.

set -u
size=1024
threads=1
cores=2
rounds=10
build=build/dev
while getopts "s:t:c:r:b:h" opt; do
    case "$opt" in
    s) size="$OPTARG" ;;
    t) threads="$OPTARG" ;;
    c) cores="$OPTARG" ;;
    r) rounds="$OPTARG" ;;
    b) build="$OPTARG" ;;
    h)
        sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
experiments=("$@")
[ ${#experiments[@]} -eq 0 ] && experiments=(jain freerider latency)
build="$(readlink -f "$build")"
pmd="$build/src/pmd/pmd"
pmrun="$build/src/tools/pmrun"
matmul="$build/apps/matmul/matmul"
faultlat="$build/apps/faultlat/faultlat"
for tool in "$pmd" "$pmrun" "$matmul" "$faultlat"; do
    if [ ! -x "$tool" ]; then
        echo "no $tool: build first" >&2
        exit 2
    fi
done

work="$(mktemp -d /tmp/pm-fair-XXXXXX)"
daemons=()
stop_daemons() {
    for pid in "${daemons[@]}"; do
        { kill "$pid" && wait "$pid"; } 2>/dev/null
    done
    daemons=()
}
trap 'stop_daemons; rm -rf "$work"' EXIT

# Free ports: a control and a discovery port for each of up to four nodes.
mapfile -t ports < <(python3 - <<'PY'
import socket
held = []
for kind in [socket.SOCK_STREAM] * 4 + [socket.SOCK_DGRAM] * 4:
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    held.append(s)
    print(s.getsockname()[1])
PY
)
control() { echo "${ports[$(($1 - 1))]}"; }
discovery() { echo "${ports[$(($1 + 3))]}"; }

# start_daemons count [free-rider]: nodes 1 to count, each knowing the others as seeds. With
# "free-rider", node `count` gives the others a discovery port nobody listens on, so they never
# hear it and never ask it for a worker, while it can still ask them.
start_daemons() {
    local count=$1 rider=${2:-}
    rm -rf "$work"/[0-9]*
    for i in $(seq 1 "$count"); do
        local peers=""
        for j in $(seq 1 "$count"); do
            [ "$j" -eq "$i" ] && continue
            [ -n "$rider" ] && [ "$j" -eq "$count" ] && continue  # nobody seeds the free-rider
            local port
            port="$(discovery "$j")"
            [ -n "$rider" ] && [ "$i" -eq "$count" ] && port=1
            peers="$peers${peers:+,}127.0.0.1:$(control "$j"):$port"
        done
        "$pmd" --node-id "$i" --cap-cores "$cores" --no-multicast --http-port 0 \
            --control-port "$(control "$i")" --discovery-port "$(discovery "$i")" \
            --state-dir "$work/$i" ${peers:+--peers "$peers"} 2>"$work/pmd-$i.log" &
        daemons+=($!)
    done
    for i in $(seq 1 "$count"); do
        for _ in $(seq 1 100); do
            [ -S "$work/$i/pmd.sock" ] && break
            sleep 0.1
        done
    done
    sleep 2  # discovery: a HELLO a second
}

# run_job node nodes out program arguments...: pmrun on `node`, in the background.
run_job() {
    local node=$1 nodes=$2 out=$3
    shift 3
    "$pmrun" -n "$nodes" --state-dir "$work/$node" "$@" >"$out" 2>&1 &
}

seconds_of() { sed -n 's/.*: \([0-9.]*\) s, checksum.*/\1/p' "$1"; }

failed=0
experiment_jain() {
    start_daemons 3
    for i in 1 2 3; do run_job "$i" 3 "$work/jain-$i" "$matmul" "$size" "$threads"; done
    wait_jobs || return 1
    local times=()
    for i in 1 2 3; do
        times+=("$(seconds_of "$work/jain-$i")")
        echo "  job from node $i: ${times[-1]} s"
    done
    python3 -c '
import sys
x = [1 / float(t) for t in sys.argv[1:]]
print("  Jain\x27s index over the three throughputs: %.3f" % (sum(x) ** 2 / (len(x) * sum(v * v for v in x))))
' "${times[@]}"
    stop_daemons
}

experiment_freerider() {
    export PARAMESH_CFG_LEDGER_SCALE=10
    echo "  ledger.scale is 10 core-seconds for this experiment"
    start_daemons 4 free-rider
    echo "  round  free-rider s  contributors' mean s  free-rider weight at node 1  its share"
    for round in $(seq 1 "$rounds"); do
        for i in 1 2 3; do run_job "$i" 3 "$work/c-$i" "$matmul" "$size" "$threads"; done
        run_job 4 4 "$work/fr" "$matmul" "$size" "$threads"
        wait_jobs || { unset PARAMESH_CFG_LEDGER_SCALE; return 1; }
        local weight
        weight="$(curl -s "http://127.0.0.1:$(sed -n 's/.*"http_port":\([0-9]*\).*/\1/p' "$work/pmd-1.log" | head -1)/v1/ledger" |
            python3 -c 'import json, sys
for p in json.load(sys.stdin).get("peers", []):
    if p["peer"] == 4: print("%.2f" % p["weight"]); break
else: print("1.00")')"
        python3 -c '
import sys
fr, *others = (float(t) for t in sys.argv[2:])
throughput = [1 / fr] + [1 / t for t in others]
print("  %5s  %12.2f  %20.2f  %27s  %.3f" % (sys.argv[1], fr, sum(others) / len(others),
      sys.argv[6], throughput[0] / sum(throughput)))
' "$round" "$(seconds_of "$work/fr")" "$(seconds_of "$work/c-1")" "$(seconds_of "$work/c-2")" \
            "$(seconds_of "$work/c-3")" "$weight"
    done
    unset PARAMESH_CFG_LEDGER_SCALE
    stop_daemons
}

experiment_latency() {
    start_daemons 3
    run_job 1 3 "$work/alone" "$faultlat" 4096
    wait_jobs || return 1
    echo "  alone:          $(sed -n 's/^faultlat: //p' "$work/alone")"
    run_job 2 3 "$work/busy-2" "$matmul" "$size" "$threads"
    run_job 3 3 "$work/busy-3" "$matmul" "$size" "$threads"
    sleep 1
    run_job 1 3 "$work/busy" "$faultlat" 4096
    wait_jobs || return 1
    echo "  beside 2 jobs:  $(sed -n 's/^faultlat: //p' "$work/busy")"
    stop_daemons
}

# Waits for every job started; 1 if one failed, printing what it said.
wait_jobs() {
    local bad=0
    for pid in $(jobs -p); do
        if ! printf '%s\n' "${daemons[@]}" | grep -qx "$pid"; then
            wait "$pid" || bad=1
        fi
    done
    if [ "$bad" -ne 0 ]; then
        echo "  a job failed:"
        tail -n 3 "$work"/jain-* "$work"/c-* "$work"/fr "$work"/alone "$work"/busy* 2>/dev/null |
            sed 's/^/    | /'
    fi
    return "$bad"
}

for experiment in "${experiments[@]}"; do
    if ! declare -f "experiment_$experiment" >/dev/null; then
        echo "no experiment $experiment" >&2
        exit 2
    fi
    echo "$experiment (matmul $size with $threads thread(s) a node, --cap-cores $cores):"
    "experiment_$experiment" || failed=1
done
exit "$failed"
