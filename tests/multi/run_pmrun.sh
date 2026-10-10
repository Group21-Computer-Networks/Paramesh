#!/usr/bin/env bash
# Runs one ParaMesh program as a job of N nodes on this machine the way a user would: one pmd
# for each node, and one pmrun. Reports PASS or FAIL.
#
#   tests/multi/run_pmrun.sh [-n nodes] [-c cores] [-t seconds] [-b build directory] program [arguments...]
#
#   -n   number of nodes, 1 to 8 (default 3)
#   -c   --cap-cores of every daemon: the most worker threads a job gets on a node (default 2)
#   -t   give up after this many seconds (default 120)
#   -b   build directory with src/pmd/pmd and src/tools/pmrun (default build/dev)
#
# The daemons listen on free ports of 127.0.0.1 and keep their sockets in a temporary
# directory. Node 1 is where pmrun runs; its daemon knows the others as peers.
# Exit status: 0 PASS, 1 FAIL, 2 bad usage.

set -u
nodes=3
cores=2
limit=120
build=build/dev
while getopts "n:c:t:b:h" opt; do
    case "$opt" in
    n) nodes="$OPTARG" ;;
    c) cores="$OPTARG" ;;
    t) limit="$OPTARG" ;;
    b) build="$OPTARG" ;;
    h)
        sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
pmd="$build/src/pmd/pmd"
pmrun="$build/src/tools/pmrun"
if [ $# -lt 1 ] || ! [ "$nodes" -ge 1 ] 2>/dev/null || [ "$nodes" -gt 8 ] ||
    [ ! -x "$pmd" ] || [ ! -x "$pmrun" ]; then
    echo "usage: $0 [-n nodes] [-c cores] [-t seconds] [-b build directory] program [arguments...]" >&2
    echo "       (needs $pmd and $pmrun)" >&2
    exit 2
fi
program="$1"

state="$(mktemp -d /tmp/pmrun-XXXXXX)" # short: a socket's path is at most 107 bytes
daemons=()
cleanup() {
    for pid in "${daemons[@]}"; do
        { kill "$pid" && wait "$pid"; } 2>/dev/null
    done
    rm -rf "$state"
}
trap cleanup EXIT

# One free port for each daemon's control channel.
mapfile -t ports < <(python3 - "$nodes" <<'PY'
import socket, sys
held = [socket.socket() for _ in range(int(sys.argv[1]))]
for s in held:
    s.bind(("127.0.0.1", 0))
    print(s.getsockname()[1])
PY
)
peers=""
for i in $(seq 2 "$nodes"); do
    peers="$peers${peers:+,}127.0.0.1:${ports[$((i - 1))]}"
done
for i in $(seq "$nodes" -1 1); do # node 1 last, so its peers are already listening
    args=(--node-id "$i" --cap-cores "$cores" --control-port "${ports[$((i - 1))]}" --state-dir "$state/$i")
    if [ "$i" -eq 1 ] && [ -n "$peers" ]; then
        args+=(--peers "$peers")
    fi
    "$pmd" "${args[@]}" 2>"$state/pmd-$i.log" &
    daemons+=("$!")
done
for i in $(seq 1 "$nodes"); do
    for _ in $(seq 1 50); do
        [ -S "$state/$i/pmd.sock" ] && break
        sleep 0.1
    done
    if [ ! -S "$state/$i/pmd.sock" ]; then
        echo "the pmd of node $i did not start:" >&2
        cat "$state/pmd-$i.log" >&2
        echo "FAIL: $(basename "$program") on $nodes nodes (pmrun)"
        exit 1
    fi
done

timeout "$limit" "$pmrun" -n "$nodes" --state-dir "$state/1" "$@" >"$state/out" 2>&1
status=$?
sed 's/^/    | /' "$state/out"
if [ "$status" -eq 124 ]; then
    echo "pmrun: still running after $limit s"
else
    echo "pmrun: exit $status"
fi
sleep 0.5 # the workers end when the launcher tells them; their daemons collect them
left="$(pgrep -f "^$(readlink -f "$program")" | wc -l)"
if [ "$left" -ne 0 ]; then
    echo "$left process(es) of the job are still running"
    pkill -KILL -f "^$(readlink -f "$program")"
    status=1
fi
if [ "$status" -eq 0 ]; then
    echo "PASS: $(basename "$program") on $nodes nodes (pmrun)"
    exit 0
fi
for i in $(seq 1 "$nodes"); do
    grep -h '"level":"warn"\|"level":"error"' "$state/pmd-$i.log" | sed "s/^/    pmd $i: /"
done
echo "FAIL: $(basename "$program") on $nodes nodes (pmrun)"
exit 1
