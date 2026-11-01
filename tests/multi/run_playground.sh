#!/usr/bin/env bash
# The playground end to end on this machine: two pmds on loopback and the server. Compiles and
# runs every example on one and two nodes, then the failures and attacks. Reports PASS or FAIL.
#
#   tests/multi/run_playground.sh [build directory, default build/dev]
#
# The library comes from the build directory, so it tests this tree, not /opt/paramesh.

set -u
build="$(cd "${1:-build/dev}" && pwd)"
work="$(mktemp -d /tmp/pmplay-XXXXXX)"
daemons=()
cleanup() {
    for pid in "${daemons[@]}"; do { kill "$pid" && wait "$pid"; } 2>/dev/null; done
    rm -rf "$work"
}
trap cleanup EXIT
mkdir -p "$work/1" "$work/2" "$work/3" "$work/prefix/lib" "$work/prefix/include"
cp include/paramesh.h "$work/prefix/include/"
{
    echo "CREATE $work/prefix/lib/libparamesh.a"
    for module in lib coh mem store rt net wire platform; do
        echo "ADDLIB $build/src/$module/libparamesh_$module.a"
    done
    echo SAVE
    echo END
} | ar -M
"$build/src/pmd/pmd" --node-id 2 --no-multicast --control-port 47211 --discovery-port 47210 \
    --http-port 47212 --state-dir "$work/2" --cap-cores 2 --cap-ram 512M \
    --peers 127.0.0.1:47201:47200 >"$work/pmd2.log" 2>&1 &
daemons+=($!)
"$build/src/pmd/pmd" --node-id 3 --no-multicast --control-port 47221 --discovery-port 47220 \
    --http-port 47222 --state-dir "$work/3" --standby >"$work/pmd3.log" 2>&1 &
daemons+=($!)
"$build/src/pmd/pmd" --node-id 1 --no-multicast --control-port 47201 --discovery-port 47200 \
    --http-port 47202 --state-dir "$work/1" --cap-cores 2 --cap-ram 512M \
    --peers 127.0.0.1:47211:47210 >"$work/pmd1.log" 2>&1 &
daemons+=($!)
sleep 3
python3 tools/playground/server.py --port 47081 --pmd-http 47202 --state-dir "$work/1" \
    --bin "$build/src/tools" --prefix "$work/prefix" --timeout 4 --code letmein --public \
    --advertise 127.0.0.1 --pmd-ports 47201:47200 >"$work/site.log" 2>&1 &
daemons+=($!)
python3 tools/playground/server.py --port 47082 --pmd-http 47222 --state-dir "$work/3" \
    --bin "$build/src/tools" --prefix "$work/prefix" >"$work/site3.log" 2>&1 &
daemons+=($!)
sleep 2
export PMRUN="$build/src/tools/pmrun" STATE="$work/1"
python3 tests/multi/playground_checks.py
status=$?
[ "$status" -eq 0 ] && echo PASS || { echo FAIL; tail -5 "$work/pmd1.log"; }
exit "$status"
