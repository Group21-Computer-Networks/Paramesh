/* quickstart: the example of docs/API.md. Squares a shared array across the pool, adds up the
 * squares with an atomic counter, and checks the answer.
 *
 *     pmrun -n 3 /opt/paramesh/apps/quickstart [count]
 */
#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct Args {
    uint64_t* values; /* shared: from pm_malloc() */
    uint64_t* total;  /* shared: one number */
};

/* Runs on any node, for indexes lo <= i < hi. Safe to run twice: it overwrites its outputs,
 * and the one addition to the shared total comes last. */
PM_TASK(square) {
    const struct Args* a = arg;
    uint64_t sum = 0;
    for (uint64_t i = lo; i < hi; i++) {
        a->values[i] = i * i;
        sum += a->values[i];
    }
    pm_atomic_add(a->total, sum);
}

int main(int argc, char** argv) {
    const uint64_t count = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000000;
    const pm_config config = {.region_bytes = count * sizeof(uint64_t) + 2 * PM_PAGE_SIZE};
    int status = pm_init(&argc, &argv, &config); /* a worker never returns from here */
    if (status != PM_OK) {
        fprintf(stderr, "quickstart: %s\n", pm_strerror(status));
        return 1;
    }
    const struct Args args = {pm_malloc(count * sizeof(uint64_t)), pm_malloc(sizeof(uint64_t))};
    if (args.values == NULL || args.total == NULL) {
        fprintf(stderr, "quickstart: the shared region is too small\n");
        return 1;
    }
    status = pm_parallel_for_data("square", 0, count, 0, &args, sizeof args, args.values,
                                  sizeof(uint64_t));
    if (status == PM_OK) {
        status = pm_wait_all();
    }
    if (status != PM_OK) {
        fprintf(stderr, "quickstart: %s\n", pm_strerror(status));
        return 1;
    }
    const uint64_t n = count - 1; /* 0^2 + ... + n^2 = n(n+1)(2n+1)/6 */
    const uint64_t expected = n * (n + 1) / 2 * (2 * n + 1) / 3;
    const uint64_t total = *args.total; /* read it before pm_finalize() releases the region */
    printf("sum of squares below %llu: %llu (%s)\n", (unsigned long long)count,
           (unsigned long long)total, total == expected ? "right" : "WRONG");
    return pm_finalize() == PM_OK && total == expected ? 0 : 1;
}
