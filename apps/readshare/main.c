/* readshare: node B reads what node A wrote, with no socket code here.
 *
 *     readshare [numbers]
 *
 * The launcher takes three 2 MiB arrays from the shared region, which land in three segments
 * and so, on several nodes, usually on more than one home. It writes `numbers` 64-bit values
 * (default 8192) at the start of each. Every worker then reads and checks all of them, and the
 * launcher reads them back. Exit status 0 if every value was right on every node.
 *
 * Start it on several nodes with tests/multi/run_local.sh or tests/multi/run_netns.sh. */

#include "lib/test_hook.h"

#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SEGMENT_BYTES ((size_t)2 << 20)
#define ARRAYS 3
/* Until tasks carry arguments (M3), a worker finds the arrays where the first allocations of a
 * job always are: at the start of the shared region. */
#define REGION_BASE ((uintptr_t)0x600000000000)

static uint64_t g_numbers;

static uint64_t* array(int which) {
    return (uint64_t*)(REGION_BASE + (uintptr_t)which * SEGMENT_BYTES);
}

static uint64_t value(int which, uint64_t i) {
    return (uint64_t)(which + 1) * 1000003U + i * i;
}

static int all_right(void) {
    for (int a = 0; a < ARRAYS; a++) {
        for (uint64_t i = 0; i < g_numbers; i++) {
            if (array(a)[i] != value(a, i)) {
                return 0;
            }
        }
    }
    return 1;
}

/* What each worker runs when the launcher says so. */
static void check_on_worker(void) {
    if (!all_right()) {
        pm_test_fail("readshare: a worker read a wrong number");
    }
}

int main(int argc, char** argv) {
    g_numbers = argc > 1 ? strtoull(argv[1], NULL, 10) : 8192;
    if (g_numbers == 0 || g_numbers > SEGMENT_BYTES / sizeof(uint64_t)) {
        fprintf(stderr, "usage: readshare [numbers, 1 to %zu]\n", SEGMENT_BYTES / sizeof(uint64_t));
        return 2;
    }
    pm_test_register(0, check_on_worker);

    const pm_config config = {.region_bytes = (ARRAYS + 1) * SEGMENT_BYTES};
    const int status = pm_init(&argc, &argv, &config); /* a worker does not return from this */
    if (status != PM_OK) {
        fprintf(stderr, "readshare: pm_init: %s\n", pm_strerror(status));
        return 1;
    }

    for (int a = 0; a < ARRAYS; a++) {
        uint64_t* numbers = pm_malloc(SEGMENT_BYTES);
        if (numbers != array(a)) {
            fprintf(stderr, "readshare: array %d is not where the workers will look\n", a);
            return 1;
        }
        for (uint64_t i = 0; i < g_numbers; i++) {
            numbers[i] = value(a, i);
        }
    }
    if (pm_test_run_on_workers(0) != PM_OK || !all_right()) {
        fprintf(stderr, "readshare: the launcher could not have its data checked\n");
        return 1;
    }
    printf(
        "readshare: %llu numbers in each of %d arrays, written by the launcher, read by every "
        "node: ok\n",
        (unsigned long long)g_numbers, ARRAYS);
    return pm_finalize() == PM_OK ? 0 : 1;
}
