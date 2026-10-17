/* faultlat: how long a page fault takes to fetch a page from another node (M5-5).
 *
 *     faultlat [pages] [threads]
 *
 * A ParaMesh program: start it with pmrun. The launcher writes `pages` pages (default 4096,
 * 16 MB). Tasks then read the first byte of each page, timing each read: the first touch of a
 * page faults, and the page comes from the launcher, which wrote it. Reads on the launcher's
 * own node are left out. It prints how many reads were timed and their 50th, 90th and 99th
 * percentiles and maximum, in microseconds. tools/bench/fairness.sh runs it with and without
 * other jobs beside it. */

#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>

struct TouchArgs {
    const volatile char* data;
    uint64_t* ns;      /* ns[page]: how long its first read took */
    uint64_t* reader;  /* reader[page]: the node that read it */
    uint64_t launcher; /* its node, whose reads are local and are not timed */
};

static uint64_t now_ns(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC); /* C11; nanoseconds, as fine as a fault needs */
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

PM_TASK(faultlat_touch) {
    const struct TouchArgs* args = arg;
    if (ctx->node == args->launcher) {
        /* Its copies are local: it waits instead, so the other nodes get the chunks. */
        thrd_sleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 5000000}, NULL);
        for (uint64_t page = lo; page < hi; page++) {
            args->reader[page] = ctx->node;
        }
        return;
    }
    for (uint64_t page = lo; page < hi; page++) {
        const uint64_t before = now_ns();
        (void)args->data[page * PM_PAGE_SIZE]; /* the fault, and the fetch */
        const uint64_t took = now_ns() - before;
        args->ns[page] = took;
        args->reader[page] = ctx->node;
    }
}

static int by_value(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a;
    const uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv) {
    const uint64_t pages = argc > 1 ? strtoull(argv[1], NULL, 10) : 4096;
    const uint32_t threads = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
    if (pages == 0 || pages > (1ULL << 20)) {
        fprintf(stderr, "usage: faultlat [pages, 1 to 1048576] [threads on each node]\n");
        return 2;
    }
    const uint64_t bytes = pages * PM_PAGE_SIZE;
    const pm_config config = {
        .region_bytes = bytes + 2 * pages * sizeof(uint64_t) + 4ULL * PM_PAGE_SIZE,
        .threads_per_node = threads};
    const int status = pm_init(&argc, &argv, &config); /* a worker does not return from this */
    if (status != PM_OK) {
        fprintf(stderr, "faultlat: pm_init: %s\n", pm_strerror(status));
        return 1;
    }
    char* data = pm_malloc(bytes);
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in this process changes the environment */
    const char* self = getenv("PARAMESH_NODE_ID");
    const uint64_t launcher = self != NULL ? strtoull(self, NULL, 10) : 1;
    struct TouchArgs args = {data, pm_malloc(pages * sizeof(uint64_t)),
                             pm_malloc(pages * sizeof(uint64_t)), launcher};
    if (data == NULL || args.ns == NULL || args.reader == NULL) {
        fprintf(stderr, "faultlat: the shared region is too small\n");
        return 1;
    }
    for (uint64_t page = 0; page < pages; page++) {
        data[page * PM_PAGE_SIZE] = (char)page; /* the launcher holds every page */
    }
    /* No array: the queue hands chunks out first come, first served, not to their pages' homes. */
    if (pm_parallel_for("faultlat_touch", 0, pages, 64, &args, sizeof args) != PM_OK ||
        pm_wait_all() != PM_OK) {
        fprintf(stderr, "faultlat: the tasks could not run\n");
        return 1;
    }
    uint64_t* timed = malloc(pages * sizeof(uint64_t));
    uint64_t count = 0;
    for (uint64_t page = 0; timed != NULL && page < pages; page++) {
        if (args.reader[page] != launcher) {
            timed[count++] = args.ns[page];
        }
    }
    if (count == 0) {
        printf("faultlat: no read ran on another node\n");
    } else {
        qsort(timed, count, sizeof(uint64_t), by_value);
        const uint64_t p50 = timed[count / 2];
        const uint64_t p90 = timed[count * 9 / 10];
        const uint64_t p99 = timed[count * 99 / 100];
        printf(
            "faultlat: %llu remote first reads: p50 %.1f us, p90 %.1f us, p99 %.1f us, max "
            "%.1f us\n",
            (unsigned long long)count, (double)p50 / 1000.0, (double)p90 / 1000.0,
            (double)p99 / 1000.0, (double)timed[count - 1] / 1000.0);
    }
    free(timed);
    return pm_finalize() == PM_OK ? 0 : 1;
}
