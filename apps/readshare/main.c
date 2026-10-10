/* readshare: other nodes read what the launcher wrote, with no socket code here.
 *
 *     readshare [numbers]
 *
 * The launcher takes three 2 MiB arrays from the shared region, which land in three segments
 * and so, on several nodes, usually on more than one home. It writes `numbers` 64-bit values
 * (default 8192) at the start of each. Tasks on every node of the job then read and check all
 * of them. Exit status 0 if every value was right wherever it was read.
 *
 * Start it with pmrun, or with tests/multi/run_local.sh or run_netns.sh. */

#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEGMENT_BYTES ((size_t)2 << 20)
#define ARRAYS 3
#define NODE_IDS 65536 /* a node ID is 16 bits */

struct CheckArgs {
    const uint64_t* array[ARRAYS];
    uint64_t numbers;
    uint64_t* wrong;   /* values that were not what the launcher wrote */
    uint64_t* by_node; /* by_node[node]: values that node checked */
};

static uint64_t value(uint64_t which, uint64_t i) {
    return (which + 1) * 1000003U + i * i;
}

/* Index i is number i % numbers of array i / numbers. Reading is all it does, so running a
 * chunk twice only counts it twice. */
PM_TASK(readshare_check) {
    const struct CheckArgs* args = arg;
    uint64_t wrong = 0;
    for (uint64_t i = lo; i < hi; i++) {
        const uint64_t which = i / args->numbers;
        const uint64_t at = i % args->numbers;
        wrong += args->array[which][at] != value(which, at);
    }
    pm_atomic_add(args->wrong, wrong);
    pm_atomic_add(&args->by_node[ctx->node], hi - lo);
}

int main(int argc, char** argv) {
    const uint64_t numbers = argc > 1 ? strtoull(argv[1], NULL, 10) : 8192;
    if (numbers == 0 || numbers > SEGMENT_BYTES / sizeof(uint64_t)) {
        fprintf(stderr, "usage: readshare [numbers, 1 to %zu]\n", SEGMENT_BYTES / sizeof(uint64_t));
        return 2;
    }
    const pm_config config = {.region_bytes = (ARRAYS + 2) * SEGMENT_BYTES};
    const int status = pm_init(&argc, &argv, &config); /* a worker does not return from this */
    if (status != PM_OK) {
        fprintf(stderr, "readshare: pm_init: %s\n", pm_strerror(status));
        return 1;
    }

    struct CheckArgs args = {.numbers = numbers,
                             .wrong = pm_malloc(sizeof(uint64_t)),
                             .by_node = pm_malloc(NODE_IDS * sizeof(uint64_t))};
    for (uint64_t which = 0; which < ARRAYS; which++) {
        uint64_t* array = pm_malloc(SEGMENT_BYTES);
        if (array == NULL || args.wrong == NULL || args.by_node == NULL) {
            fprintf(stderr, "readshare: the shared region is too small\n");
            return 1;
        }
        for (uint64_t i = 0; i < numbers; i++) {
            array[i] = value(which, i);
        }
        args.array[which] = array;
    }

    int ran = pm_parallel_for("readshare_check", 0, ARRAYS * numbers, 0, &args, sizeof args);
    if (ran == PM_OK) {
        ran = pm_wait_all();
    }
    if (ran != PM_OK) {
        fprintf(stderr, "readshare: the tasks could not run: %s\n", pm_strerror(ran));
        return 1;
    }

    uint64_t checked = 0;
    printf("readshare: numbers checked by node:");
    for (uint32_t node = 0; node < NODE_IDS; node++) {
        if (args.by_node[node] != 0) {
            printf(" %u:%llu", node, (unsigned long long)args.by_node[node]);
            checked += args.by_node[node];
        }
    }
    const int right = *args.wrong == 0 && checked == ARRAYS * numbers;
    printf(
        "\nreadshare: %llu numbers in each of %d arrays, written by the launcher, read by "
        "tasks: %s\n",
        (unsigned long long)numbers, ARRAYS, right ? "ok" : "WRONG");
    return pm_finalize() == PM_OK && right ? 0 : 1;
}
