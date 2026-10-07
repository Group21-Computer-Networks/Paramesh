#!/usr/bin/env bash
# Runs one ParaMesh program as a job of N processes on this machine and reports PASS or FAIL.
#
#   tests/multi/run_local.sh [-n nodes] [-t seconds] program [arguments...]
#
#   -n   number of nodes, 1 to 8 (default 3). Node 1 is the launcher.
#   -t   give up after this many seconds (default 60)
#
# Every node runs the same command, told apart by the environment of docs/PROTOCOL.md,
# section 10. PASS means every process exited 0. Exit status: 0 PASS, 1 FAIL, 2 bad usage.
#
# tests/multi/run_netns.sh calls this script with PM_NODE_ADDRS (one address per node) and
# PM_NODE_NS (one PID per node, whose network namespace the node runs in).

set -u

nodes=3
limit=60
while getopts "n:t:h" opt; do
    case "$opt" in
    n) nodes="$OPTARG" ;;
    t) limit="$OPTARG" ;;
    h)
        sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ $# -lt 1 ] || ! [ "$nodes" -ge 1 ] 2>/dev/null || [ "$nodes" -gt 8 ]; then
    echo "usage: $0 [-n nodes] [-t seconds] program [arguments...]" >&2
    exit 2
fi

read -r -a addrs <<<"${PM_NODE_ADDRS:-}"
read -r -a spaces <<<"${PM_NODE_NS:-}"
where="localhost"
[ ${#spaces[@]} -gt 0 ] && where="network namespaces${PM_NETEM:+, $PM_NETEM}"

logs="$(mktemp -d "${TMPDIR:-/tmp}/paramesh-run.XXXXXX")" || exit 1
pids=()
cleanup() {
    for pid in "${pids[@]}"; do
        { kill -KILL "$pid" && wait "$pid"; } 2>/dev/null
    done
    rm -rf "$logs"
}
trap cleanup EXIT

# One address and port per node: 127.0.0.1 and a free port each, or the namespace's address and
# one fixed port.
peers=""
listen=()
for i in $(seq 1 "$nodes"); do
    if [ ${#addrs[@]} -ge "$i" ]; then
        at="${addrs[$((i - 1))]}:47100"
    else
        port="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')" || exit 1
        at="127.0.0.1:$port"
    fi
    listen+=("$at")
    peers="${peers:+$peers,}$i@$at"
done

for i in $(seq 1 "$nodes"); do
    role=worker
    [ "$i" -eq 1 ] && role=launcher
    enter=()
    [ ${#spaces[@]} -ge "$i" ] && enter=(nsenter -t "${spaces[$((i - 1))]}" -n)
    "${enter[@]}" env PARAMESH_ROLE="$role" PARAMESH_JOB_ID=1 PARAMESH_NODE_ID="$i" \
        PARAMESH_LISTEN="${listen[$((i - 1))]}" PARAMESH_PEERS="$peers" "$@" >"$logs/$i.log" 2>&1 &
    pids+=($!)
done

# Wait for every process, or for the time limit.
deadline=$((SECONDS + limit))
codes=()
for i in $(seq 1 "$nodes"); do
    pid="${pids[$((i - 1))]}"
    while kill -0 "$pid" 2>/dev/null && [ "$SECONDS" -lt "$deadline" ]; do sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then
        codes+=("still running after ${limit} s")
    else
        wait "$pid"
        codes+=("$?")
    fi
done

failed=0
for i in $(seq 1 "$nodes"); do
    code="${codes[$((i - 1))]}"
    name="node $i"
    [ "$i" -eq 1 ] && name="node 1 (launcher)"
    echo "$name: exit $code"
    sed 's/^/    | /' "$logs/$i.log"
    [ "$code" = "0" ] || failed=1
done

if [ "$failed" -eq 0 ]; then
    echo "PASS: $(basename "$1") on $nodes nodes ($where)"
    exit 0
fi
echo "FAIL: $(basename "$1") on $nodes nodes ($where)"
exit 1
