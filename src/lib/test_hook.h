/* Test-only hook for M1 and M2, before the task runtime exists (docs/PLAN.md, "Running code on
 * workers before M3"). Not part of the public API; M3-7 removes its users.
 *
 * Every process registers the same functions under the same numbers before pm_init(). The
 * launcher then says when the workers run one, which gives a test its order: write first,
 * read after. */
#ifndef PARAMESH_LIB_TEST_HOOK_H
#define PARAMESH_LIB_TEST_HOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C spelling on purpose: this header is also for C programs. */
typedef void (*pm_test_fn)(void); /* NOLINT(modernize-use-using, modernize-redundant-void-arg) */

/* id is 0 to 15. Call before pm_init(), in every process. */
void pm_test_register(uint32_t id, pm_test_fn fn);

/* Launcher only: runs function `id` once on every worker, on a thread of its own, and returns
 * PM_OK when all of them have finished. */
int pm_test_run_on_workers(uint32_t id);

/* Ends the job everywhere with this message; the calling process exits with status 1. */
void pm_test_fail(const char* message);

#ifdef __cplusplus
}
#endif

#endif /* PARAMESH_LIB_TEST_HOOK_H */
