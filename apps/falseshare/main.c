/* falseshare: two tasks write neighbouring numbers, which share a page they do not mean to.
 *
 *     falseshare [writes] [pairs]
 *
 * Chunk i of the task write_left adds 1, `writes` times (default 2000000), to number 2i of an
 * array; chunk i of write_right does the same to number 2i + 1. There are `pairs` chunks of
 * each (default 16, at most 256), so every number has one writer and the result is exact.
 * But all the numbers are in one 4 KiB page, and a page has one writer node at a time: with
 * the chunks spread over several nodes, the page is pulled back and forth between them.
 *
 * The home's hold window (docs/STATE_MACHINES.md, section 2.6) notices this and lets each
 * owner keep the page for a while. Run the program twice to see what it is worth:
 *
 *     tests/multi/run_local.sh build/dev/apps/falseshare/falseshare
 *     PARAMESH_CFG_THRASH_THRESHOLD=0 tests/multi/run_local.sh build/dev/apps/falseshare/falseshare
 *
 * The second run has the hold window off. With PARAMESH_CFG_LOG_LEVEL=info each process also
 * logs the pages it received (job_pages), the home logs the fight (page_thrash, with the two
 * nodes) and the launcher logs which task ran on which node (task_on_node). */

#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NODE_IDS 65536 /* a node ID is 16 bits */
#define MAX_PAIRS 256  /* 512 numbers of 8 bytes: one page */

struct WriteArgs {
    uint64_t* numbers; /* one page of them */
    uint64_t writes;
    uint64_t* by_node; /* by_node[node]: chunks that node ran */
};

static double seconds_now(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Adds 1 to one number, `writes` times, each time through memory. The number is set first, so
 * a chunk that is run again gives the same result. */
static void write_number(const struct WriteArgs* args, uint64_t which) {
    volatile uint64_t* number = &args->numbers[which];
    *number = 0;
    for (uint64_t i = 0; i < args->writes; i++) {
        *number = *number + 1;
    }
}

PM_TASK(write_left) {
    const struct WriteArgs* args = arg;
    for (uint64_t i = lo; i < hi; i++) {
        write_number(args, 2 * i);
    }
    pm_atomic_add(&args->by_node[ctx->node], hi - lo);
}

PM_TASK(write_right) {
    const struct WriteArgs* args = arg;
    for (uint64_t i = lo; i < hi; i++) {
        write_number(args, 2 * i + 1);
    }
    pm_atomic_add(&args->by_node[ctx->node], hi - lo);
}

int main(int argc, char** argv) {
    const uint64_t writes = argc > 1 ? strtoull(argv[1], NULL, 10) : 2000000;
    const uint64_t pairs = argc > 2 ? strtoull(argv[2], NULL, 10) : 16;
    if (writes == 0 || pairs == 0 || pairs > MAX_PAIRS) {
        fprintf(stderr, "usage: falseshare [writes, 1 or more] [pairs, 1 to %d]\n", MAX_PAIRS);
        return 2;
    }
    const pm_config config = {.region_bytes = (uint64_t)4 << 20};
    const int status = pm_init(&argc, &argv, &config); /* a worker does not return from this */
    if (status != PM_OK) {
        fprintf(stderr, "falseshare: pm_init: %s\n", pm_strerror(status));
        return 1;
    }
    const struct WriteArgs args = {.numbers = pm_malloc(PM_PAGE_SIZE),
                                   .writes = writes,
                                   .by_node = pm_malloc(NODE_IDS * sizeof(uint64_t))};
    if (args.numbers == NULL || args.by_node == NULL) {
        fprintf(stderr, "falseshare: could not set up the shared data\n");
        return 1;
    }

    /* One chunk at a time, left and right in turn, so that neighbours are in the queue
     * together and go to whichever nodes ask. */
    const double start = seconds_now();
    int ran = PM_OK;
    for (uint64_t i = 0; i < pairs && ran == PM_OK; i++) {
        ran = pm_parallel_for("write_left", i, i + 1, 1, &args, sizeof args);
        if (ran == PM_OK) {
            ran = pm_parallel_for("write_right", i, i + 1, 1, &args, sizeof args);
        }
    }
    if (ran == PM_OK) {
        ran = pm_wait_all();
    }
    const double took = seconds_now() - start;
    if (ran != PM_OK) {
        fprintf(stderr, "falseshare: the tasks could not run: %s\n", pm_strerror(ran));
        return 1;
    }

    int exact = 1;
    for (uint64_t i = 0; i < 2 * pairs; i++) {
        exact = exact && args.numbers[i] == writes;
    }
    const uint64_t numbers = 2 * pairs;
    printf("falseshare: %llu numbers in one page, %llu writes each: %.3f s, %s\n",
           (unsigned long long)numbers, (unsigned long long)writes, took, exact ? "ok" : "WRONG");
    printf("falseshare: chunks by node:");
    for (uint32_t node = 0; node < NODE_IDS; node++) {
        if (args.by_node[node] != 0) {
            printf(" %u:%llu", node, (unsigned long long)args.by_node[node]);
        }
    }
    printf("\n");
    return pm_finalize() == PM_OK && exact ? 0 : 1;
}
