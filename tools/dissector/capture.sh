#!/usr/bin/env bash
# Captures a three-node ParaMesh run and checks that the dissector decodes every frame (M6-4).
#
#   tools/dissector/capture.sh [-b build directory] [-o capture file] [program arguments...]
#
#   -b   build directory with src/pmd/pmd, src/tools/pmrun and apps/ (default build/dev)
#   -o   where to keep the capture (default: a new /tmp/paramesh-XXXXXX.pcap)
#
# The three daemons run in network namespaces on one bridge (tests/multi/netns.sh), so the
# capture needs no root where unprivileged namespaces are allowed. The job is matrix multiply
# (default `matmul 256 1`). With tshark installed, the capture is then read with
# tools/dissector/paramesh.lua: it prints how many frames were decoded, how many of each opcode,
# and how many were malformed. Exit status 0 when none was, 1 otherwise, 2 for bad usage.

set -u
here="$(dirname "$(readlink -f "$0")")"
# shellcheck source=tests/multi/netns.sh
. "$here/../../tests/multi/netns.sh"

build=build/dev
capture=""
while getopts "b:o:h" opt; do
    case "$opt" in
    b) build="$OPTARG" ;;
    o) capture="$OPTARG" ;;
    h)
        sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] && set -- matmul 256 1
[ -z "$capture" ] && capture="$(mktemp /tmp/paramesh-XXXXXX.pcap)"
# The run and its capture happen in a namespace, as its root; the capture is read out here,
# as the user, because Wireshark will not run a Lua dissector as root.
if [ -z "${PM_IN_NAMESPACE:-}" ]; then
    self="$(readlink -f "$0")"
    if unshare --user --map-root-user --net --mount true 2>/dev/null; then
        unshare --user --map-root-user --net --mount env PM_IN_NAMESPACE=1 \
            "$self" -b "$build" -o "$capture" "$@"
    elif sudo -n true 2>/dev/null; then
        sudo -n unshare --net --mount env PM_IN_NAMESPACE=1 "$self" -b "$build" -o "$capture" "$@"
    else
        echo "cannot create network namespaces here; see tools/check_env.sh" >&2
        exit 2
    fi
    status=$?
    if ! command -v tshark >/dev/null; then
        echo "tshark is not installed: open $capture in Wireshark with $here/paramesh.lua"
        exit "$status"
    fi
    read_with() { tshark -n -X "lua_script:$here/paramesh.lua" -r "$capture" "$@" 2>/dev/null; }
    frames=$(read_with -Y pmsh -T fields -e pmsh.opcode | tr ',' '\n' | grep -c .)
    echo "ParaMesh frames decoded: $frames, of these opcodes:"
    read_with -Y pmsh -T fields -e pmsh.opcode | tr ',' '\n' | sort | uniq -c | sort -rn |
        sed 's/^/  /'
    bad=$(read_with -Y "_ws.malformed || pmsh.malformed || _ws.expert.severity == error" | grep -c .)
    echo "malformed or in error: $bad"
    [ "$status" -eq 0 ] && [ "$frames" -gt 0 ] && [ "$bad" -eq 0 ]
    exit $?
fi

build="$(readlink -f "$build")"
program="$build/apps/$1/$1"
shift

netns_nodes 3 0 0
work="$(mktemp -d /tmp/pm-cap-XXXXXX)"
trap 'pkill -P $$ 2>/dev/null; for pid in "${NS_HOLDERS[@]}"; do kill "$pid" 2>/dev/null; done; rm -rf "$work"' EXIT

tcpdump -Z root -i pmbr0 -w "$capture" -U -s 0 2>"$work/tcpdump.log" &
dump=$!
sleep 1
for i in 3 2 1; do
    peers=""
    for j in 1 2 3; do [ "$j" -ne "$i" ] && peers="$peers${peers:+,}${NS_ADDRS[$((j - 1))]}"; done
    nsenter -t "${NS_HOLDERS[$((i - 1))]}" -n "$build/src/pmd/pmd" --node-id "$i" --no-multicast \
        --http-port 0 --state-dir "$work/$i" --peers "$peers" 2>"$work/pmd-$i.log" &
done
for i in 1 2 3; do
    for _ in $(seq 1 100); do [ -S "$work/$i/pmd.sock" ] && break; sleep 0.1; done
done
sleep 2
nsenter -t "${NS_HOLDERS[0]}" -n "$build/src/tools/pmrun" -n 3 --state-dir "$work/1" "$program" "$@"
status=$?
sleep 2
# tcpdump cannot be signalled from inside the namespace; taking its interface away ends it,
# and with -U every packet is already in the file.
ip link del pmbr0
wait "$dump" 2>/dev/null
echo "pmrun: exit $status; capture in $capture"
exit "$status"
