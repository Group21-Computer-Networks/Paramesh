#!/usr/bin/env bash
# Benchmarks matrix multiply three ways and prints a results table:
# the plain program on one thread, ParaMesh on one node, and ParaMesh on N nodes.
#
#   tools/bench/matmul.sh [-n nodes] [-s size] [-t threads] [-r runs] [-b build directory]
#
#   -n   nodes of the third way (default 3)
#   -s   the matrices are size x size (default 1024; the M3 gate uses 4096)
#   -t   worker threads on each node (default 2)
#   -r   runs of each way (default 5); the table has their median and range
#   -b   build directory with apps/matmul/matmul. Default build/bench, which this script
#        configures as a Release build and builds if the program is not there.
#
# The nodes are processes on this machine, started by tests/multi/run_local.sh, so they share
# its processors: keep nodes x threads at or below the number of processors, or the N-node
# time measures the crowding and not the runtime. Exit status 0, or 1 if a run failed.

set -u
root="$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")"
nodes=3
size=1024
threads=2
runs=5
build=""
while getopts "n:s:t:r:b:h" opt; do
    case "$opt" in
    n) nodes="$OPTARG" ;;
    s) size="$OPTARG" ;;
    t) threads="$OPTARG" ;;
    r) runs="$OPTARG" ;;
    b) build="$OPTARG" ;;
    h)
        sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) exit 1 ;;
    esac
done

if [ -z "$build" ]; then
    build="$root/build/bench"
    if [ ! -x "$build/apps/matmul/matmul" ]; then
        echo "building a Release build in $build ..." >&2
        # Warnings are not errors here: with optimisation on, GCC 13 reports null dereferences
        # in the library that the Debug builds of CI do not (docs/TODO.md).
        cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPARAMESH_WERROR=OFF \
            >/dev/null &&
            cmake --build "$build" --target matmul >/dev/null 2>&1 || {
            echo "the Release build failed" >&2
            exit 1
        }
    fi
fi
matmul="$build/apps/matmul/matmul"
if [ ! -x "$matmul" ]; then
    echo "no matmul program at $matmul" >&2
    exit 1
fi
run_local="$root/tests/multi/run_local.sh"

# Runs one way `runs` times. Sets: times (one per line, sorted), last (the last run's output).
measure() {
    times=""
    for _ in $(seq 1 "$runs"); do
        last="$("$@" 2>&1)"
        seconds="$(printf '%s\n' "$last" | sed -n 's/.*matmul: [a-z]*, n [0-9]*: \([0-9.]*\) s.*/\1/p')"
        if [ -z "$seconds" ]; then
            echo "a run failed: $*" >&2
            printf '%s\n' "$last" >&2
            exit 1
        fi
        times="$times$seconds"$'\n'
    done
    times="$(printf '%s' "$times" | sort -n)"
}

# The middle of the sorted times, and their smallest and largest.
median() { printf '%s\n' "$times" | sed -n "$(((runs + 1) / 2))p"; }
range() { echo "$(printf '%s\n' "$times" | head -1) to $(printf '%s\n' "$times" | tail -1)"; }
ratio() { awk -v a="$1" -v b="$2" 'BEGIN { if (b > 0) printf "%.2f", a / b; else printf "-" }'; }

measure "$matmul" --plain "$size"
plain_median="$(median)"
plain_range="$(range)"
plain_sum="$(printf '%s\n' "$last" | sed -n 's/.*checksum \([0-9]*\).*/\1/p')"

measure "$run_local" -n 1 -t 3600 "$matmul" "$size" "$threads"
one_median="$(median)"
one_range="$(range)"
one_sum="$(printf '%s\n' "$last" | sed -n 's/.*checksum \([0-9]*\).*/\1/p')"

measure "$run_local" -n "$nodes" -t 3600 "$matmul" "$size" "$threads"
many_median="$(median)"
many_range="$(range)"
many_sum="$(printf '%s\n' "$last" | sed -n 's/.*checksum \([0-9]*\).*/\1/p')"
chunks="$(printf '%s\n' "$last" | sed -n 's/.*chunks by node: *//p')"

echo "Matrix multiply, n = $size, $runs runs each, on $(hostname) ($(nproc) processors)."
echo
echo "| Way | Threads | Median (s) | Range (s) | Against one node |"
echo "| --- | --- | --- | --- | --- |"
echo "| Plain program | 1 | $plain_median | $plain_range | $(ratio "$one_median" "$plain_median") |"
echo "| ParaMesh, 1 node | $threads | $one_median | $one_range | 1.00 |"
echo "| ParaMesh, $nodes nodes | $nodes x $threads | $many_median | $many_range | $(ratio "$one_median" "$many_median") |"
echo
echo "\"Against one node\" is the one-node median divided by the row's median: above 1 is faster."
echo "Chunks by node in the last $nodes-node run (node:chunks): $chunks"
if [ "$plain_sum" != "$one_sum" ] || [ "$plain_sum" != "$many_sum" ]; then
    echo "FAIL: the three ways gave different checksums: $plain_sum, $one_sum, $many_sum"
    exit 1
fi
echo "The three ways gave the same checksum, $plain_sum."
