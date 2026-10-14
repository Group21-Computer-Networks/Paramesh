#!/usr/bin/env bash
# The M4 scenarios on real daemons, each node in its own network namespace, with a chosen delay
# and loss on every node's link (M4-8). Reports PASS or FAIL for each.
#
#   tests/multi/run_m4.sh [-d delay_ms] [-l loss_percent] [-b build directory] [scenario...]
#
#   leave  node 3 leaves a running job (pm leave); the job ends with every number right
#   join   then a fourth node comes up and joins the job, and runs chunks
#   abort  node 3's worker is killed; the job ends within 3 s, naming node 3
#   spill  RAM caps far below the job's data; the job ends with every number right, and a
#          spill file was written
#
#   -d   delay added to every packet a node sends, in ms (default 0)
#   -l   share of packets a node sends that are dropped, in percent (default 0)
#   -b   build directory with src/pmd/pmd, src/tools/pmrun, src/tools/pm and
#        tests/unit/unit_pmd_test, whose test program is the job (default build/dev)
#
# With no scenario, all four run. Exit status: 0 every scenario passed, 1 one failed, 2 bad
# usage or no namespaces.

set -u
here="$(dirname "$(readlink -f "$0")")"
# shellcheck source=tests/multi/netns.sh
. "$here/netns.sh"
netns_enter "$0" "$@"

delay=0
loss=0
build=build/dev
while getopts "d:l:b:h" opt; do
    case "$opt" in
    d) delay="$OPTARG" ;;
    l) loss="$OPTARG" ;;
    b) build="$OPTARG" ;;
    h)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
scenarios=("$@")
[ ${#scenarios[@]} -eq 0 ] && scenarios=(leave join abort spill)
build="$(readlink -f "$build")"
pmd="$build/src/pmd/pmd"
pmrun="$build/src/tools/pmrun"
pm="$build/src/tools/pm"
program="$build/tests/unit/unit_pmd_test"
for tool in "$pmd" "$pmrun" "$pm" "$program"; do
    if [ ! -x "$tool" ]; then
        echo "no $tool: build first" >&2
        exit 2
    fi
done

netns_nodes 3 "$delay" "$loss"
where="network namespaces, delay $delay ms, loss $loss%"
work="$(mktemp -d /tmp/pm-m4-XXXXXX)" # short: a socket's path is at most 107 bytes
trap 'pkill -KILL -P $$ 2>/dev/null; for pid in "${NS_HOLDERS[@]}"; do kill "$pid" 2>/dev/null; done; rm -rf "$work"' EXIT

daemons=()
# Starts node i's daemon in its namespace; the extra arguments go to pmd.
start_pmd() {
    local i=$1
    shift
    rm -rf "${work:?}/$i" && mkdir -p "$work/$i"
    # nsenter itself, not netns_run in a subshell: $! is then the daemon's own PID.
    nsenter -t "${NS_HOLDERS[$((i - 1))]}" -n "$pmd" --node-id "$i" --no-multicast --http-port 0 --state-dir "$work/$i" "$@" \
        >"$work/pmd-$i.log" 2>&1 &
    daemons[i]=$!
    for _ in $(seq 1 100); do
        [ -S "$work/$i/pmd.sock" ] && return 0
        sleep 0.1
    done
    echo "    the pmd of node $i did not start:"
    sed 's/^/    | /' "$work/pmd-$i.log"
    return 1
}

stop_daemons() {
    for pid in "${daemons[@]}"; do
        { kill "$pid" && wait "$pid"; } 2>/dev/null
    done
    daemons=()
    pkill -KILL -f "^$program --as-program" 2>/dev/null
}

# Waits up to $2 seconds for the file $1.
wait_file() {
    for _ in $(seq 1 $(($2 * 10))); do
        [ -e "$1" ] && return 0
        sleep 0.1
    done
    return 1
}

# Waits up to $2 seconds for process $1 to end; its exit status, or 255 if it did not.
wait_pid() {
    for _ in $(seq 1 $(($2 * 10))); do
        if ! kill -0 "$1" 2>/dev/null; then
            wait "$1"
            return $?
        fi
        sleep 0.1
    done
    return 255
}

# The value of `key=` in the program's report.
field() { sed -n "s/^$1=//p" "$work/report" 2>/dev/null; }

failures=0
check() { # check description test...
    local what=$1
    shift
    if "$@"; then
        echo "    ok: $what"
    else
        echo "    NOT: $what"
        failures=$((failures + 1))
    fi
}

# The three daemons, node 1 knowing the other two; then the program as a job of three, in the
# given mode, until it says node 3 is running a chunk.
start_job() { # start_job mode [pmd arguments...]
    local mode=$1
    shift
    rm -f "$work"/report*
    start_pmd 3 "$@" && start_pmd 2 "$@" &&
        start_pmd 1 --peers "${NS_ADDRS[1]},${NS_ADDRS[2]}" "$@" || return 1
    nsenter -t "${NS_HOLDERS[0]}" -n env PARAMESH_CFG_LOG_LEVEL=info "$pmrun" -n 3 --state-dir "$work/1" \
        "$program" --as-program "$work/report" "$mode" >"$work/out" 2>"$work/said" &
    job=$!
    if ! wait_file "$work/report.three" 180; then
        echo "    node 3 ran no chunk within 180 s"
        return 1
    fi
}

leave_node_3() {
    netns_run 3 "$pm" leave --state-dir "$work/3" >/dev/null &&
        wait_pid "${daemons[3]}" 60 # its daemon goes once its worker has left
}

scenario_leave() {
    start_job until-stop || return 1
    check "pm leave on node 3, and its daemon has gone" leave_node_3
    touch "$work/report.stop"
    wait_pid "$job" 300
    check "the job ended with exit 0" test $? -eq 0
    check "every number right" test "$(field wrong)" = 0
    check "the launcher let node 3 go (member_left)" grep -q '"event":"member_left"' "$work/said"
}

scenario_join() {
    start_job join || return 1
    check "pm leave on node 3, and its daemon has gone" leave_node_3
    [ ${#NS_HOLDERS[@]} -lt 4 ] && netns_add "$delay" "$loss"
    start_pmd 4 --peers "${NS_ADDRS[0]}" || return 1
    wait_pid "$job" 300
    check "the job ended with exit 0" test $? -eq 0
    check "every number right" test "$(field wrong)" = 0
    check "node 4 ran chunks ($(field node4) indexes)" test "$(field node4)" -gt 0
    check "the launcher took node 4 in (member_joined)" grep -q '"event":"member_joined"' "$work/said"
}

scenario_abort() {
    start_job until-stop || return 1
    pkill -KILL -P "${daemons[3]}"
    local from=$SECONDS
    wait_pid "$job" 30
    local status=$? took=$((SECONDS - from))
    check "the job ended with exit 1" test "$status" -eq 1
    check "within 3 s (took about $took s)" test "$took" -le 3
    check "it named node 3" grep -q 'aborted: node 3 lost' "$work/said"
    sleep 1
    check "no job process is left" test "$(pgrep -c -f "^$program --as-program")" -eq 0
}

scenario_spill() {
    # 1 MB of RAM for each node and 8 MB of numbers: a home store holds 768 KB in RAM, so most
    # of what a node is home for goes to its spill file (up to 16 MB).
    export PARAMESH_CFG_SPILL_BUDGET=16777216
    start_job big-until-stop --cap-ram 1M || return 1
    unset PARAMESH_CFG_SPILL_BUDGET
    # The job runs until a process of it has a spill file with something in it (unlinked, so
    # found by its descriptor), at most 120 s; the rounds go on meanwhile.
    local spilled=0 bytes
    for _ in $(seq 1 1200); do
        [ "$spilled" -gt 0 ] && break
        for pid in $(pgrep -f "^$program --as-program"); do
            for fd in $(find "/proc/$pid/fd" -lname '*paramesh-spill-*' 2>/dev/null); do
                bytes=$(stat -L -c %s "$fd" 2>/dev/null || echo 0)
                [ "$bytes" -gt "$spilled" ] && spilled=$bytes
            done
        done
        sleep 0.1
    done
    touch "$work/report.stop"
    wait_pid "$job" 300
    check "the job ended with exit 0" test $? -eq 0
    check "every number right" test "$(field wrong)" = 0
    check "a spill file was written ($((spilled / 1024)) KB)" test "$spilled" -gt 0
}

failed=0
for scenario in "${scenarios[@]}"; do
    if ! declare -f "scenario_$scenario" >/dev/null; then
        echo "no scenario $scenario" >&2
        exit 2
    fi
    echo "$scenario ($where):"
    failures=0
    started=$SECONDS
    "scenario_$scenario" || failures=$((failures + 1))
    stop_daemons
    if [ "$failures" -eq 0 ]; then
        echo "PASS: $scenario ($where, $((SECONDS - started)) s)"
    else
        echo "FAIL: $scenario ($where)"
        sed 's/^/    | /' "$work/said" "$work/out" 2>/dev/null | grep -v '"level":"info"' | tail -20
        failed=1
    fi
done
exit "$failed"
