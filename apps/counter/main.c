/* counter: the nodes of a job add to one shared number, and the total is exact.
 *
 *     counter [adds]
 *
 * Twice over: `adds` tasks (default 3000) each add 1 to a shared number, first taking a lock
 * around every read-add-write, then with pm_atomic_add() and no lock. The tasks run on every
 * node of the job at the same time. Exit status 0 if both totals are `adds`.
 *
 * Start it with pmrun, or with tests/multi/run_local.sh or run_netns.sh;
 * tests/multi/counter.sh does all three. */

#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NODE_IDS 65536 /* a node ID is 16 bits */

struct Shared {
    uint64_t with_lock;   /* changed only under the lock */
    uint64_t with_atomic; /* changed only by pm_atomic_add() */
    uint64_t failed;      /* lock calls that did not return PM_OK */
};

struct AddArgs {
    pm_lock_t lock;
    struct Shared* shared;
    uint64_t* by_node; /* by_node[node]: adds that node made, both ways together */
};

/* Adds under the lock. Unlike most tasks this one may not be run twice on the same indexes:
 * it shows what the lock is for, and a job that loses a node while it runs would count some
 * adds again. */
PM_TASK(add_with_lock) {
    const struct AddArgs* args = arg;
    uint64_t failed = 0;
    for (uint64_t i = lo; i < hi; i++) {
        failed += pm_lock(args->lock) != PM_OK;
        args->shared->with_lock = args->shared->with_lock + 1;
        failed += pm_unlock(args->lock) != PM_OK;
    }
    pm_atomic_add(&args->shared->failed, failed);
    pm_atomic_add(&args->by_node[ctx->node], hi - lo);
}

PM_TASK(add_atomically) {
    const struct AddArgs* args = arg;
    for (uint64_t i = lo; i < hi; i++) {
        pm_atomic_add(&args->shared->with_atomic, 1);
    }
    pm_atomic_add(&args->by_node[ctx->node], hi - lo);
}

int main(int argc, char** argv) {
    const uint64_t adds = argc > 1 ? strtoull(argv[1], NULL, 10) : 3000;
    if (adds == 0 || adds > 100000000) {
        fprintf(stderr, "usage: counter [adds, 1 to 100000000]\n");
        return 2;
    }
    const pm_config config = {.region_bytes = (uint64_t)4 << 20};
    const int status = pm_init(&argc, &argv, &config); /* a worker does not return from this */
    if (status != PM_OK) {
        fprintf(stderr, "counter: pm_init: %s\n", pm_strerror(status));
        return 1;
    }
    struct AddArgs args = {.lock = pm_lock_create(),
                           .shared = pm_malloc(sizeof(struct Shared)),
                           .by_node = pm_malloc(NODE_IDS * sizeof(uint64_t))};
    if (args.lock.id == 0 || args.shared == NULL || args.by_node == NULL) {
        fprintf(stderr, "counter: could not set up the shared data\n");
        return 1;
    }

    int ran = pm_parallel_for("add_with_lock", 0, adds, 0, &args, sizeof args);
    if (ran == PM_OK) {
        ran = pm_wait_all();
    }
    if (ran == PM_OK) {
        ran = pm_parallel_for("add_atomically", 0, adds, 0, &args, sizeof args);
    }
    if (ran == PM_OK) {
        ran = pm_wait_all();
    }
    if (ran != PM_OK) {
        fprintf(stderr, "counter: the tasks could not run: %s\n", pm_strerror(ran));
        return 1;
    }

    const int exact = args.shared->with_lock == adds && args.shared->with_atomic == adds &&
                      args.shared->failed == 0;
    printf("counter: %llu adds each way: %llu with a lock, %llu with pm_atomic_add: %s\n",
           (unsigned long long)adds, (unsigned long long)args.shared->with_lock,
           (unsigned long long)args.shared->with_atomic, exact ? "ok" : "WRONG");
    printf("counter: adds by node:");
    for (uint32_t node = 0; node < NODE_IDS; node++) {
        if (args.by_node[node] != 0) {
            printf(" %u:%llu", node, (unsigned long long)args.by_node[node]);
        }
    }
    printf("\n");
    return pm_finalize() == PM_OK && exact ? 0 : 1;
}
