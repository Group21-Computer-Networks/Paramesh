/* The C11 half of paramesh_h_test.cpp: proves include/paramesh.h compiles as C, that every
 * declaration accepts C arguments, and that PM_TASK works in a C file. */

#include "paramesh.h"

/* sizeof does not evaluate its operand, so these check each call against its declaration
 * without needing the library to exist yet. */
_Static_assert(sizeof(pm_init((int*)0, (char***)0, &(pm_config){.region_bytes = 1})) == sizeof(int),
               "pm_init");
_Static_assert(sizeof(pm_finalize()) == sizeof(int), "pm_finalize");
_Static_assert(sizeof(pm_strerror(PM_ERR_INVALID)) == sizeof(const char*), "pm_strerror");
_Static_assert(sizeof(pm_malloc(PM_PAGE_SIZE)) == sizeof(void*), "pm_malloc");
_Static_assert(sizeof(pm_parallel_for("t", 0, 8, 0, (const void*)0, 0)) == sizeof(int),
               "pm_parallel_for");
_Static_assert(sizeof(pm_parallel_for_data("t", 0, 8, 0, (const void*)0, 0, (const void*)0, 8)) ==
                   sizeof(int),
               "pm_parallel_for_data");
_Static_assert(sizeof(pm_wait_all()) == sizeof(int), "pm_wait_all");
_Static_assert(sizeof(pm_lock_create()) == sizeof(uint64_t), "pm_lock_create");
_Static_assert(sizeof(pm_lock((pm_lock_t){1})) == sizeof(int), "pm_lock");
_Static_assert(sizeof(pm_unlock((pm_lock_t){1})) == sizeof(int), "pm_unlock");
_Static_assert(sizeof(pm_barrier_create(3)) == sizeof(uint64_t), "pm_barrier_create");
_Static_assert(sizeof(pm_barrier_wait((pm_barrier_t){1})) == sizeof(int), "pm_barrier_wait");
_Static_assert(sizeof(pm_atomic_add((uint64_t*)0, 1)) == sizeof(uint64_t), "pm_atomic_add");
_Static_assert(sizeof(pm_touch((const void*)0, 0, PM_ACCESS_WRITE)) == sizeof(int), "pm_touch");

_Static_assert(PM_OK == 0, "PM_OK is zero");
_Static_assert(PM_TASK_ARG_MAX == 1024, "argument limit");

/* The task's argument: where the sum of the indexes goes. */
struct SumArgs {
    uint64_t* out;
};

PM_TASK(paramesh_h_test_c_task) {
    const struct SumArgs* args = arg;
    uint64_t sum = 0;
    for (uint64_t i = lo; i < hi; i++) {
        sum += i;
    }
    *args->out = sum + ctx->arg_len;
}
