/* sweep: rounds over one shared array of n MB, each index updated by a task, so a chunk run
 * where its pages are homed touches no other node.
 *
 *     sweep [n] [rounds] [threads]
 *
 * n defaults to 64 MB, rounds to 4. Start it as a job with pmrun. It prints one line with the
 * time of the rounds and a checksum, the same however the chunks were placed, and how many
 * chunks each node ran. The array is not touched before the first round, so each page starts
 * at its home. tools/bench/evaluate.sh's sched experiment runs it (X-locality-sched). */

#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NODE_IDS 65536 /* a node ID is 16 bits */

struct SweepArgs {
    uint64_t* values;
    uint64_t round;
    uint64_t* chunks; /* chunks[node]: how many chunks that node ran */
};

static double seconds_now(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC); /* C11; the wall clock is good enough for seconds */
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* One round writes v[i] = 3i + round: the same however often a chunk runs (FR-3.4). */
PM_TASK(sweep_round) {
    const struct SweepArgs* args = arg;
    for (uint64_t i = lo; i < hi; i++) {
        args->values[i] = i * 3 + args->round;
    }
    pm_atomic_add(&args->chunks[ctx->node], 1);
}

int main(int argc, char** argv) {
    const uint64_t mb = argc > 1 ? strtoull(argv[1], NULL, 10) : 64;
    const uint64_t rounds = argc > 2 ? strtoull(argv[2], NULL, 10) : 4;
    const uint64_t threads = argc > 3 ? strtoull(argv[3], NULL, 10) : 0;
    if (mb == 0 || mb > 2048 || rounds == 0 || rounds > 64 || threads > 1024) {
        fprintf(stderr, "usage: sweep [n MB, 1 to 2048] [rounds, 1 to 64] [threads]\n");
        return 2;
    }
    const uint64_t count = mb * 1024 * 1024 / sizeof(uint64_t);
    const uint64_t counts = NODE_IDS * sizeof(uint64_t);
    const pm_config config = {
        .region_bytes = count * sizeof(uint64_t) + counts + 2 * (uint64_t)PM_PAGE_SIZE,
        .threads_per_node = (uint32_t)threads};
    const int status = pm_init(&argc, &argv, &config); /* a worker does not return from this */
    if (status != PM_OK) {
        fprintf(stderr, "sweep: pm_init: %s\n", pm_strerror(status));
        return 1;
    }
    uint64_t* values = pm_malloc(count * sizeof(uint64_t));
    uint64_t* chunks = pm_malloc(counts);
    if (values == NULL || chunks == NULL) {
        fprintf(stderr, "sweep: the shared region is too small\n");
        return 1;
    }
    const double start = seconds_now();
    int ran = PM_OK;
    for (uint64_t r = 0; r < rounds && ran == PM_OK; r++) {
        const struct SweepArgs args = {values, r, chunks};
        ran = pm_parallel_for_data("sweep_round", 0, count, 0, &args, sizeof args, values,
                                   sizeof(uint64_t));
        if (ran == PM_OK) {
            ran = pm_wait_all();
        }
    }
    const double took = seconds_now() - start;
    if (ran != PM_OK) {
        fprintf(stderr, "sweep: the tasks could not run: %s\n", pm_strerror(ran));
        return 1;
    }
    uint64_t sum = 0; /* one number a page: the last round's, if every chunk ran */
    for (uint64_t i = 0; i < count; i += PM_PAGE_SIZE / sizeof(uint64_t)) {
        sum += values[i] - i * 3;
    }
    printf("sweep: job, n %llu: %.3f s, checksum %llu\n", (unsigned long long)mb, took,
           (unsigned long long)sum);
    printf("sweep: chunks by node:");
    for (uint32_t node = 0; node < NODE_IDS; node++) {
        if (chunks[node] != 0) {
            printf(" %u:%llu", node, (unsigned long long)chunks[node]);
        }
    }
    printf("\n");
    return pm_finalize() == PM_OK ? 0 : 1;
}
