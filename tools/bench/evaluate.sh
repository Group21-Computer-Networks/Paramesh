#!/usr/bin/env bash
# The M6 evaluation runs (M6-5), each writing one results file:
#
#   tools/bench/evaluate.sh [-s size] [-t threads] [-b build directory] [-o results directory]
#                           [experiment...]
#
#   impairment  matrix multiply on three nodes in network namespaces, once for every added
#               delay (0, 1, 5, 10, 25, 50 ms), jitter (0 or 25% of the delay) and loss (0, 1,
#               2, 5%): impairment.csv, one row a run
#   outofcore   three nodes whose --cap-ram (1 MB) is far below the job's region, so pages go to
#               the spill files: outofcore.csv
#   histogram   one run without impairment: each node's fault-latency histogram: histogram.csv
#   cache       the local cache's two replacement policies, fifo and cost (X-cache-policy),
#               at --cap-ram 1M and 2M, matrix size doubled: cache.csv
#   prefetch    predictive prefetching (X-prefetch) off and at prefetch.depth 8 and 32, with
#               the caps' defaults and at --cap-ram 1M, matrix size doubled: prefetch.csv
#   sched       apps/sweep (64 MB, 4 rounds) with task.affinity 0, 1 and 2 (X-locality-sched),
#               with no added delay and with 1 ms: sched.csv
#   encoding    page.encoding off, auto and on (X-page-encoding), for matrix multiply and
#               apps/sweep (16 MB, 4 rounds), FIFO chunk placement, on a plain link, with 1 ms
#               delay and at 20 Mbit/s: encoding.csv
#   coherence   apps/falseshare, a write-write and a read-write fight over one page, with
#               matmul and apps/sweep beside them, with the hold window off, for writers
#               only, and for readers too (X-adaptive-coherence), three runs each: coherence.csv
#   rehome      dynamic home reassignment (X-rehome), off and every 500 ms, FIFO chunk
#               placement, 1 ms delay, for a read-write fight over one page with no hold
#               window, matrix multiply and apps/sweep (16 MB, 4 rounds), with the segments
#               moved in the last column: rehome.csv. The page of the fight is homed off the
#               fighting nodes (REHOME_SKIP segments on). A run that aborts leaves its logs in
#               abort-<time> under the results directory.
#
#   -s   matrix size (default 256)
#   -t   worker threads a node (default 1)
#   -b   build directory (default build/dev)
#   -o   where the results go (default build/results)
#
# A row has the run time, faults per second, MB per second received, the median and 99th
# percentile fault latency (the upper bound of their power-of-two bucket; ">16384" for the
# last, open-ended one), retries, whether the job aborted, and the pages received and sent
# (write-backs among them), the KB of page payloads sent and what they would have been plain,
# the job processes' CPU seconds, and the pages sent as diffs. The counts are what each job process logs as it ends (job_pages),
# summed over the nodes. With no experiment, the first three run. Needs no root where
# unprivileged user namespaces are allowed. The reason for any abort goes to aborts.log. Exit
# status 0, or 1 if a run aborted.

set -u
here="$(dirname "$(readlink -f "$0")")"
# shellcheck source=tests/multi/netns.sh
. "$here/../../tests/multi/netns.sh"
netns_enter "$0" "$@"

size=256
threads=1
build=build/dev
results=build/results
while getopts "s:t:b:o:h" opt; do
    case "$opt" in
    s) size="$OPTARG" ;;
    t) threads="$OPTARG" ;;
    b) build="$OPTARG" ;;
    o) results="$OPTARG" ;;
    h)
        sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
experiments=("$@")
[ ${#experiments[@]} -eq 0 ] && experiments=(impairment outofcore histogram)
build="$(readlink -f "$build")"
mkdir -p "$results"
results="$(readlink -f "$results")"
: >"$results/aborts.log"
for tool in "$build/src/pmd/pmd" "$build/src/tools/pmrun" "$build/apps/matmul/matmul"; do
    [ -x "$tool" ] || { echo "no $tool: build first" >&2; exit 2; }
done

netns_nodes 3 0 0
work="$(mktemp -d /tmp/pm-eval-XXXXXX)"
trap 'pkill -P $$ 2>/dev/null; for pid in "${NS_HOLDERS[@]}"; do kill "$pid" 2>/dev/null; done; rm -rf "$work"' EXIT
HEADER="run_s,faults,faults_per_s,mb_per_s,p50_us,p99_us,retries,aborted,pages_in,pages_out,page_kb,page_kb_plain,cpu_s,diffs"

# Every node's link: this delay and jitter, in ms, and loss, in percent.
impair() {
    local spec="delay ${1}ms"
    [ "$2" != 0 ] && spec="$spec ${2}ms"
    spec="$spec rate ${4:-100gbit}"  # impair delay jitter loss [rate]
    for i in 1 2 3; do
        netns_run "$i" tc qdisc change dev eth0 root netem $spec loss "${3}%"
    done
}

# run_job [pmd arguments...]: fresh daemons, the job, and its counts as "$HEADER" values.
run_job() {
    rm -rf "${work:?}"/[0-9]
    local daemons=()
    export PARAMESH_CFG_LOG_LEVEL=info  # every job process logs job_pages as it ends
    for i in 3 2 1; do
        local peers=""
        for j in 1 2 3; do [ "$j" -ne "$i" ] && peers="$peers${peers:+,}${NS_ADDRS[$((j - 1))]}"; done
        nsenter -t "${NS_HOLDERS[$((i - 1))]}" -n "$build/src/pmd/pmd" --node-id "$i" \
            --no-multicast --http-port 0 --state-dir "$work/$i" --peers "$peers" "$@" \
            2>"$work/pmd-$i.log" &
        daemons+=($!)
    done
    for i in 1 2 3; do
        for _ in $(seq 1 100); do [ -S "$work/$i/pmd.sock" ] && break; sleep 0.1; done
    done
    sleep 2
    timeout 900 nsenter -t "${NS_HOLDERS[0]}" -n "$build/src/tools/pmrun" -n 3 \
        --state-dir "$work/1" ${APP:-"$build/apps/matmul/matmul" "$size" "$threads"} >"$work/out" 2>&1
    local status=$?
    unset PARAMESH_CFG_LOG_LEVEL
    if [ "$status" -ne 0 ]; then  # why, for aborts.log
        { echo "== ${RUN:-run}: pmrun exit $status"; grep -h 'aborted' "$work/out" "$work"/pmd-*.log |
            grep -v '"event"' | sort -u; } >>"$results/aborts.log"
        cp -r "$work" "$results/abort-$(date +%s)"  # every process's log, for the reason
    fi
    sleep 1  # the workers end on JOB_END
    for pid in "${daemons[@]}"; do { kill "$pid" && wait "$pid"; } 2>/dev/null; done
    python3 - "$work" "$status" <<'PY'
import glob, json, re, sys
work, status = sys.argv[1], int(sys.argv[2])
out = open(f"{work}/out").read()
m = re.search(r"(?:job, n \d+|falseshare: .*?): ([0-9.]+) s", out)
seconds = float(m.group(1)) if m else 0.0
faults = pages = sent = retries = 0
page_bytes = plain = cpu_ms = diffs = 0
buckets = [0] * 16
for path in [f"{work}/out"] + glob.glob(f"{work}/pmd-*.log"):
    for line in open(path, errors="replace"):
        if '"event":"job_pages"' not in line:
            continue
        f = json.loads(line)["fields"]
        faults += f["faults"]; pages += f["received"]; retries += f["sent_again"]
        sent += f["sent"]
        page_bytes += f.get("page_bytes", 0); plain += f.get("page_bytes_plain", 0)
        cpu_ms += f.get("cpu_ms", 0); diffs += f.get("diffs", 0)
        buckets = [a + b for a, b in zip(buckets, f["latency"])]
def pct(share):  # the bucket's upper bound; the last bucket has none
    total, seen = sum(buckets), 0
    for i, n in enumerate(buckets):
        seen += n
        if total and seen >= share * total:
            return str(1 << i) if i < 15 else ">16384"
    return "0"
rate = lambda x: x / seconds if seconds else 0.0
print(f"{seconds:.3f},{faults},{rate(faults):.1f},{rate(pages * 4096 / 1e6):.2f},"
      f"{pct(0.5)},{pct(0.99)},{retries},{1 if status != 0 else 0},{pages},{sent},"
      f"{page_bytes // 1024},{plain // 1024},{cpu_ms / 1000:.2f},{diffs}")
open(f"{work}/buckets", "w").write(" ".join(map(str, buckets)))
PY
}

aborted=0
note_abort() { [ "$(echo "$1" | cut -d, -f8)" = 1 ] && aborted=1; }

experiment_impairment() {
    local file="$results/impairment.csv"
    echo "delay_ms,jitter_ms,loss_pct,$HEADER" >"$file"
    for delay in 0 1 5 10 25 50; do
        for jitter_share in 0 25; do
            [ "$delay" -eq 0 ] && [ "$jitter_share" -ne 0 ] && continue  # no jitter on no delay
            local jitter=$((delay * jitter_share / 100))
            for loss in 0 1 2 5; do
                impair "$delay" "$jitter" "$loss"
                local row
                row="$(RUN="delay $delay ms, jitter $jitter ms, loss $loss%" run_job)"
                note_abort "$row"
                echo "$delay,$jitter,$loss,$row" | tee -a "$file"
            done
        done
    done
    impair 0 0 0
}

experiment_outofcore() {
    local file="$results/outofcore.csv"
    echo "cap_ram,spill_budget,size,$HEADER" >"$file"
    export PARAMESH_CFG_SPILL_BUDGET=67108864
    local row
    row="$(size=$((size * 2)) run_job --cap-ram 1M)"
    unset PARAMESH_CFG_SPILL_BUDGET
    note_abort "$row"
    echo "1M,64M,$((size * 2)),$row" | tee -a "$file"
}

experiment_cache() {
    local file="$results/cache.csv"
    echo "policy,cap_ram,size,$HEADER" >"$file"
    export PARAMESH_CFG_SPILL_BUDGET=67108864
    local row cap policy
    for cap in 1M 2M; do
        for policy in fifo cost; do
            row="$(PARAMESH_CFG_CACHE_POLICY=$policy size=$((size * 2)) run_job --cap-ram "$cap")"
            note_abort "$row"
            echo "$policy,$cap,$((size * 2)),$row" | tee -a "$file"
        done
    done
    unset PARAMESH_CFG_SPILL_BUDGET
}

experiment_prefetch() {
    local file="$results/prefetch.csv"
    echo "depth,cap_ram,size,$HEADER" >"$file"
    export PARAMESH_CFG_SPILL_BUDGET=67108864
    local row cap depth
    for cap in default 1M; do
        for depth in 0 8 32; do
            local caps=()
            [ "$cap" != default ] && caps=(--cap-ram "$cap")
            row="$(PARAMESH_CFG_PREFETCH_DEPTH=$depth size=$((size * 2)) run_job "${caps[@]}")"
            note_abort "$row"
            echo "$depth,$cap,$((size * 2)),$row" | tee -a "$file"
        done
    done
    unset PARAMESH_CFG_SPILL_BUDGET
}

experiment_sched() {
    local file="$results/sched.csv"
    echo "delay_ms,affinity,$HEADER" >"$file"
    local row delay affinity
    for delay in 0 1; do
        impair "$delay" 0 0
        for affinity in 0 1 2; do
            row="$(APP="$build/apps/sweep/sweep 64 4 $threads" PARAMESH_CFG_TASK_AFFINITY=$affinity run_job)"
            note_abort "$row"
            echo "$delay,$affinity,$row" | tee -a "$file"
        done
    done
    impair 0 0 0
}

experiment_encoding() {
    local file="$results/encoding.csv"
    echo "link,app,encoding,$HEADER" >"$file"
    export PARAMESH_CFG_TASK_AFFINITY=0  # chunks anywhere, so pages move between rounds
    local row link app encoding
    for link in plain 1ms 20mbit; do
        case "$link" in
            plain) impair 0 0 0 ;;
            1ms) impair 1 0 0 ;;
            20mbit) impair 0 0 0 20mbit ;;
        esac
        for app in matmul sweep; do
            local command=""
            [ "$app" = sweep ] && command="$build/apps/sweep/sweep 16 4 $threads"
            for encoding in off auto on; do
                row="$(APP="$command" PARAMESH_CFG_PAGE_ENCODING=$encoding run_job)"
                note_abort "$row"
                echo "$link,$app,$encoding,$row" | tee -a "$file"
            done
        done
    done
    impair 0 0 0
    unset PARAMESH_CFG_TASK_AFFINITY
}

experiment_coherence() {
    local file="$results/coherence.csv"
    echo "workload,hold,run,$HEADER" >"$file"
    local row workload app hold run
    for workload in write read matmul sweep; do
        case "$workload" in
        write) app="$build/apps/falseshare/falseshare 20000000 16" ;;
        read) app="$build/apps/falseshare/falseshare 20000000 16 read" ;;
        matmul) app="$build/apps/matmul/matmul $size $threads" ;;
        sweep) app="$build/apps/sweep/sweep 16 4 $threads" ;;
        esac
        for hold in off writers readers; do
            for run in 1 2 3; do
                row="$(APP="$app" \
                    PARAMESH_CFG_THRASH_THRESHOLD=$([ "$hold" = off ] && echo 0 || echo 8) \
                    PARAMESH_CFG_THRASH_HOLD_READERS=$([ "$hold" = readers ] && echo 1 || echo 0) \
                    run_job)"
                note_abort "$row"
                echo "$workload,$hold,$run,$row" | tee -a "$file"
            done
        done
    done
}

experiment_rehome() {
    local file="$results/rehome.csv"
    echo "workload,rehome_ms,run,$HEADER,moves" >"$file"
    local row workload app rehome run moves threshold
    impair 1 0 0
    for workload in ${REHOME_WORKLOADS:-fight matmul sweep}; do
        threshold=8
        case "$workload" in
        fight) app="$build/apps/falseshare/falseshare 200000000 1 read ${REHOME_SKIP:-2}"; threshold=0 ;;
        matmul) app="$build/apps/matmul/matmul $size $threads" ;;
        sweep) app="$build/apps/sweep/sweep 16 4 $threads" ;;
        esac
        for rehome in ${REHOME_PERIODS:-0 500}; do
            for run in 1 2 3; do
                row="$(APP="$app" PARAMESH_CFG_TASK_AFFINITY=0 PARAMESH_CFG_TASK_PREFETCH=0 \
                    PARAMESH_CFG_THRASH_THRESHOLD=$threshold \
                    PARAMESH_CFG_REHOME_PERIOD_MS=$rehome PARAMESH_CFG_REHOME_MIN_REQUESTS=16 \
                    PARAMESH_CFG_REHOME_SHARE=50 run_job)"
                note_abort "$row"
                moves="$(cat "$work"/out "$work"/pmd-*.log | grep -c '"event":"segment_rehomed"')"
                echo "$workload,$rehome,$run,$row,$moves" | tee -a "$file"
            done
        done
    done
    impair 0 0 0
}

experiment_histogram() {
    local file="$results/histogram.csv"
    local row
    row="$(run_job)"
    note_abort "$row"
    {
        echo "below_us,faults"
        local i=0
        for count in $(cat "$work/buckets"); do
            if [ "$i" -lt 15 ]; then echo "$((1 << i)),$count"; else echo "longer,$count"; fi
            i=$((i + 1))
        done
    } >"$file"
    echo "$HEADER"
    echo "$row"
    column -t -s, "$file"
}

for experiment in "${experiments[@]}"; do
    declare -f "experiment_$experiment" >/dev/null || { echo "no experiment $experiment" >&2; exit 2; }
    echo "$experiment (matmul $size, $threads thread(s) a node):"
    "experiment_$experiment"
done
echo "results in $results"
exit "$aborted"
