#!/usr/bin/env bash
# Runs one ParaMesh program as a job of N nodes, each in its own network namespace, joined by a
# bridge, with a chosen delay and loss on every node's link. Reports PASS or FAIL.
#
#   tests/multi/run_netns.sh [-n nodes] [-d delay_ms] [-l loss_percent] [-t seconds] program [arguments...]
#
#   -n   number of nodes, 1 to 8 (default 3)
#   -d   delay added to every packet a node sends, in ms (default 0)
#   -l   share of packets a node sends that are dropped, in percent (default 0)
#   -t   give up after this many seconds (default 120)
#
# Needs no root where unprivileged user namespaces are allowed (tools/check_env.sh tells);
# otherwise passwordless sudo. Exit status: 0 PASS, 1 FAIL, 2 bad usage or no namespaces.

set -u

if [ -z "${PM_IN_NAMESPACE:-}" ]; then
    self="$(readlink -f "$0")"
    if unshare --user --map-root-user --net --mount true 2>/dev/null; then
        exec unshare --user --map-root-user --net --mount env PM_IN_NAMESPACE=1 "$self" "$@"
    elif sudo -n true 2>/dev/null; then
        exec sudo -n unshare --net --mount env PM_IN_NAMESPACE=1 "$self" "$@"
    fi
    echo "cannot create network namespaces here; see tools/check_env.sh" >&2
    exit 2
fi

nodes=3
delay=0
loss=0
limit=120
while getopts "n:d:l:t:h" opt; do
    case "$opt" in
    n) nodes="$OPTARG" ;;
    d) delay="$OPTARG" ;;
    l) loss="$OPTARG" ;;
    t) limit="$OPTARG" ;;
    h)
        sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ $# -lt 1 ] || ! [ "$nodes" -ge 1 ] 2>/dev/null || [ "$nodes" -gt 8 ]; then
    echo "usage: $0 [-n nodes] [-d delay_ms] [-l loss_percent] [-t seconds] program [arguments...]" >&2
    exit 2
fi

# This shell is the "switch": a bridge, and one veth cable to each node's namespace.
holders=()
trap 'for pid in "${holders[@]}"; do kill "$pid" 2>/dev/null; done' EXIT
ip link set lo up
ip link add pmbr0 type bridge && ip link set pmbr0 up || {
    echo "cannot create a bridge in the namespace" >&2
    exit 2
}

addrs=""
for i in $(seq 1 "$nodes"); do
    unshare --net sleep infinity &
    holder=$!
    holders+=("$holder")
    # The holder has its own namespace once its link differs from this shell's.
    for _ in $(seq 1 50); do
        [ "$(readlink "/proc/$holder/ns/net" 2>/dev/null)" != "$(readlink /proc/$$/ns/net)" ] && break
        sleep 0.05
    done
    ip link add "pmveth$i" type veth peer name eth0 netns "$holder" &&
        ip link set "pmveth$i" master pmbr0 up &&
        nsenter -t "$holder" -n sh -c "ip link set lo up && ip addr add 10.77.0.$i/24 dev eth0 && ip link set eth0 up &&
            tc qdisc add dev eth0 root netem delay ${delay}ms loss ${loss}%" || {
        echo "cannot set up the namespace of node $i" >&2
        exit 2
    }
    addrs="$addrs 10.77.0.$i"
done

PM_NODE_ADDRS="$addrs" PM_NODE_NS="${holders[*]}" PM_NETEM="delay ${delay} ms, loss ${loss}%" \
    "$(dirname "$(readlink -f "$0")")/run_local.sh" -n "$nodes" -t "$limit" "$@"
