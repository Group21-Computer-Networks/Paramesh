#!/usr/bin/env bash
# Sourced by the scripts that run nodes in network namespaces (run_netns.sh, run_m4.sh).
#
#   netns_enter "$0" "$@"            re-runs the calling script inside a new user and network
#                                    namespace, or exits 2 if namespaces cannot be made here
#   netns_nodes count delay_ms loss  makes `count` namespaces on one bridge, node i at
#                                    10.77.0.i, each sending with that delay and loss; fills
#                                    the arrays NS_HOLDERS (one PID per node) and NS_ADDRS
#   netns_add delay_ms loss          adds one more node the same way
#   netns_run i command...           runs a command in node i's namespace
#
# Needs no root where unprivileged user namespaces are allowed (tools/check_env.sh tells);
# otherwise passwordless sudo.

netns_enter() {
    [ -n "${PM_IN_NAMESPACE:-}" ] && return 0
    local self
    self="$(readlink -f "$1")"
    shift
    if unshare --user --map-root-user --net --mount true 2>/dev/null; then
        exec unshare --user --map-root-user --net --mount env PM_IN_NAMESPACE=1 "$self" "$@"
    elif sudo -n true 2>/dev/null; then
        exec sudo -n unshare --net --mount env PM_IN_NAMESPACE=1 "$self" "$@"
    fi
    echo "cannot create network namespaces here; see tools/check_env.sh" >&2
    exit 2
}

NS_HOLDERS=()
NS_ADDRS=()
trap 'for pid in "${NS_HOLDERS[@]}"; do kill "$pid" 2>/dev/null; done' EXIT

# This shell is the "switch": a bridge, and one veth cable to each node's namespace.
netns_add() {
    local i=$((${#NS_HOLDERS[@]} + 1)) holder
    unshare --net sleep infinity &
    holder=$!
    NS_HOLDERS+=("$holder")
    # The holder has its own namespace once its link differs from this shell's.
    for _ in $(seq 1 50); do
        [ "$(readlink "/proc/$holder/ns/net" 2>/dev/null)" != "$(readlink /proc/$$/ns/net)" ] && break
        sleep 0.05
    done
    ip link add "pmveth$i" type veth peer name eth0 netns "$holder" &&
        ip link set "pmveth$i" master pmbr0 up &&
        nsenter -t "$holder" -n sh -c "ip link set lo up && ip addr add 10.77.0.$i/24 dev eth0 && ip link set eth0 up &&
            tc qdisc add dev eth0 root netem delay ${1}ms loss ${2}%" || {
        echo "cannot set up the namespace of node $i" >&2
        exit 2
    }
    NS_ADDRS+=("10.77.0.$i")
}

netns_nodes() {
    ip link set lo up
    ip link add pmbr0 type bridge && ip link set pmbr0 up || {
        echo "cannot create a bridge in the namespace" >&2
        exit 2
    }
    for _ in $(seq 1 "$1"); do
        netns_add "$2" "$3"
    done
}

netns_run() {
    local i=$1
    shift
    nsenter -t "${NS_HOLDERS[$((i - 1))]}" -n "$@"
}
