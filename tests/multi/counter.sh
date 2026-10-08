#!/usr/bin/env bash
# The M2 multi-node test: the counter application on three nodes, on localhost and then in
# network namespaces with a delay on every link. Reports PASS or FAIL.
#
#   tests/multi/counter.sh [build directory]     (default build/dev)
#
# Exit status: 0 if both runs passed, 1 otherwise.

set -u
here="$(dirname "$(readlink -f "$0")")"
counter="${1:-build/dev}/apps/counter/counter"
if [ ! -x "$counter" ]; then
    echo "no counter program at $counter; build first, or name the build directory" >&2
    exit 1
fi

failed=0
"$here/run_local.sh" "$counter" 1000 || failed=1
# Every add under the lock crosses the delayed links several times, so fewer of them.
"$here/run_netns.sh" -d 2 "$counter" 100 || failed=1

if [ "$failed" -eq 0 ]; then
    echo "PASS: counter, both runs"
else
    echo "FAIL: counter"
fi
exit "$failed"
