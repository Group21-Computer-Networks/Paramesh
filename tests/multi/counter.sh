#!/usr/bin/env bash
# The counter application on three nodes, three ways: started by hand on localhost, in
# network namespaces with a delay on every link, and through three daemons and pmrun.
# Reports PASS or FAIL.
#
#   tests/multi/counter.sh [build directory]     (default build/dev)
#
# Exit status: 0 if all three runs passed, 1 otherwise.

set -u
here="$(dirname "$(readlink -f "$0")")"
build="${1:-build/dev}"
counter="$build/apps/counter/counter"
if [ ! -x "$counter" ]; then
    echo "no counter program at $counter; build first, or name the build directory" >&2
    exit 1
fi

failed=0
"$here/run_local.sh" "$counter" 3000 || failed=1
# Every add under the lock crosses the delayed links several times, so fewer of them.
"$here/run_netns.sh" -d 2 "$counter" 300 || failed=1
"$here/run_pmrun.sh" -b "$build" "$counter" 3000 || failed=1

if [ "$failed" -eq 0 ]; then
    echo "PASS: counter, all three runs"
else
    echo "FAIL: counter"
fi
exit "$failed"
