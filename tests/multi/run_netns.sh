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

# shellcheck source=tests/multi/netns.sh
. "$(dirname "$(readlink -f "$0")")/netns.sh"
netns_enter "$0" "$@"

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

netns_nodes "$nodes" "$delay" "$loss"

PM_NODE_ADDRS="${NS_ADDRS[*]}" PM_NODE_NS="${NS_HOLDERS[*]}" PM_NETEM="delay ${delay} ms, loss ${loss}%" \
    "$(dirname "$(readlink -f "$0")")/run_local.sh" -n "$nodes" -t "$limit" "$@"
