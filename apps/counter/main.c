/* counter: every node of the job adds to one shared number, and the total is exact.
 *
 *     counter [adds]
 *
 * Twice over: first each node adds 1, `adds` times (default 1000), taking a lock around every
 * read-add-write; then each node adds with pm_atomic_add() and no lock. All nodes add at the
 * same time, the launcher included. Exit status 0 if both totals are nodes x adds.
 *
 * Start it on several nodes with tests/multi/run_local.sh or tests/multi/run_netns.sh;
 * tests/multi/counter.sh does both. */

#include "lib/test_hook.h"

#include <paramesh.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Until tasks carry arguments (M3), a worker finds the shared data where the first allocation
 * of a job always is: at the start of the shared region. */
#define REGION_BASE ((uintptr_t)0x600000000000)

struct Shared {
    pm_lock_t lock;
    uint64_t nodes;       /* how many nodes took part: each adds 1 when it starts */
    uint64_t with_lock;   /* changed only under `lock` */
    uint64_t with_atomic; /* changed only by pm_atomic_add() */
};

static uint64_t g_adds;

static struct Shared* shared(void) {
    return (struct Shared*)REGION_BASE;
}

/* What every node runs in the first phase. */
static void add_with_lock(void) {
    pm_atomic_add(&shared()->nodes, 1);
    for (uint64_t i = 0; i < g_adds; i++) {
        if (pm_lock(shared()->lock) != PM_OK) {
            pm_test_fail("counter: pm_lock failed");
        }
        shared()->with_lock = shared()->with_lock + 1;
        if (pm_unlock(shared()->lock) != PM_OK) {
            pm_test_fail("counter: pm_unlock failed");
        }
    }
}

/* And in the second. */
static void add_atomically(void) {
    for (uint64_t i = 0; i < g_adds; i++) {
        pm_atomic_add(&shared()->with_atomic, 1);
    }
}

static void* run(void* function) {
    (*(pm_test_fn*)function)();
    return NULL;
}

/* Runs `function` on every worker and, at the same time, on a thread of the launcher. */
static int on_every_node(uint32_t id, pm_test_fn function) {
    pthread_t mine = 0;
    if (pthread_create(&mine, NULL, run, (void*)&function) != 0) {
        return 0;
    }
    const int ran = pm_test_run_on_workers(id);
    return pthread_join(mine, NULL) == 0 && ran == PM_OK;
}

int main(int argc, char** argv) {
    g_adds = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000;
    if (g_adds == 0 || g_adds > 10000000) {
        fprintf(stderr, "usage: counter [adds, 1 to 10000000]\n");
        return 2;
    }
    pm_test_register(0, add_with_lock);
    pm_test_register(1, add_atomically);

    const int status = pm_init(&argc, &argv, NULL); /* a worker does not return from this */
    if (status != PM_OK) {
        fprintf(stderr, "counter: pm_init: %s\n", pm_strerror(status));
        return 1;
    }
    if (pm_malloc(sizeof(struct Shared)) != shared()) {
        fprintf(stderr, "counter: the shared data is not where the workers will look\n");
        return 1;
    }
    shared()->lock = pm_lock_create();
    if (shared()->lock.id == 0 || !on_every_node(0, add_with_lock) ||
        !on_every_node(1, add_atomically)) {
        fprintf(stderr, "counter: the job could not run the two phases\n");
        return 1;
    }

    const uint64_t nodes = shared()->nodes;
    const uint64_t with_lock = shared()->with_lock;
    const uint64_t with_atomic = shared()->with_atomic;
    const int exact = with_lock == nodes * g_adds && with_atomic == nodes * g_adds;
    printf("counter: %llu nodes, %llu adds each: %llu with a lock, %llu with pm_atomic_add: %s\n",
           (unsigned long long)nodes, (unsigned long long)g_adds, (unsigned long long)with_lock,
           (unsigned long long)with_atomic, exact ? "ok" : "WRONG");
    return pm_finalize() == PM_OK && exact ? 0 : 1;
}
