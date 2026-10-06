/*
 * paramesh.h - the ParaMesh public API.
 *
 * STATUS: DRAFT from task M0-4, awaiting approval at the M0 gate. It is not frozen yet.
 * A comment tagged [GATE Gn] marks a proposal that the person approves or changes at the gate;
 * the same numbers, with the alternatives, are listed in docs/logs/M0-4.md. Untagged text
 * restates what docs/PLAN.md or the HLD already fixes.
 *
 * A ParaMesh program is one ordinary C or C++ program. The same binary runs on every node of
 * a job. On the launcher, main() runs to the end; on a worker, pm_init() serves tasks and never
 * returns. Memory from pm_malloc() is the shared region: every node sees the same bytes at the
 * same addresses, and the library fetches a page when a thread touches one it does not hold.
 *
 * What may live in shared memory: numbers, arrays, plain structs, pointers into the shared
 * region, and pm_lock_t and pm_barrier_t handles. What may not: pointers to the local heap or
 * stack, function pointers, pthread objects, and C++ containers or strings.
 *
 * Failure model. Functions report start-up and argument errors through their return value.
 * Once a job is running, a failure it cannot continue from (a lost node, a second reply
 * timeout, an exception thrown by a task) aborts the job: every process of the job prints the
 * reason and exits. Such a failure is never reported through a return value, and no thread is
 * ever resumed on missing or stale data.
 *
 * This header is C11 and C++20. Nothing C++-specific, no exception and no ownership of
 * allocated memory crosses it.
 */
#ifndef PARAMESH_H
#define PARAMESH_H

/*
 * The names below follow C conventions, not the C++ naming rules the rest of the tree uses.
 * NOLINTBEGIN(readability-identifier-naming, modernize-use-using, modernize-deprecated-headers,
 *             modernize-macro-to-enum, cppcoreguidelines-macro-to-enum,
 *             cppcoreguidelines-macro-usage, performance-enum-size)
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- constants */

/* Plain literals, with no casts, so that they are usable in #if and warning-free in C++. */

/* Size of a page of shared memory, in bytes. Fixed by the design. */
#define PM_PAGE_SIZE 4096u

/* Largest shared region a job can ask for: 4 GB. Fixed by the design. */
#define PM_REGION_MAX_BYTES UINT64_C(4294967296)

/*
 * Largest task argument pm_parallel_for() accepts, in bytes.
 * [GATE G4] 1 KB comes from the Build Plan's API sketch; no other document states a limit.
 */
#define PM_TASK_ARG_MAX 1024u

/* ------------------------------------------------------------------------ error codes */

/*
 * Every function that returns int returns PM_OK or one of the negative codes below.
 * [GATE G9] The set of codes is a proposal. The earlier documents name no error codes.
 */
enum pm_status {
    PM_OK = 0,
    /* An argument is wrong: null, out of range, misaligned, too large, or not a valid handle. */
    PM_ERR_INVALID = -1,
    /* Called at the wrong time or place: before pm_init(), after pm_finalize(), from inside a
       task when only the launcher's main() may call it, or the other way round. */
    PM_ERR_STATE = -2,
    /* The library could not allocate memory it needs for itself. */
    PM_ERR_NOMEM = -3,
    /* No task with that name was registered with PM_TASK. */
    PM_ERR_UNKNOWN_TASK = -4,
    /* This machine cannot run ParaMesh: the kernel lacks user-mode userfaultfd with
       write-protect, or the region's fixed address is already in use. */
    PM_ERR_PLATFORM = -5,
    /* The launch environment is missing or malformed: the process was not started by pmrun or
       pmd, or a PARAMESH_* variable or configuration value cannot be used. */
    PM_ERR_CONFIG = -6,
    /* The job was not admitted or could not be started on the nodes asked for. The reason,
       with the needed and available amounts, is printed. */
    PM_ERR_REFUSED = -7,
    /* The local pmd or a peer could not be reached while the job was starting. */
    PM_ERR_NETWORK = -8
};

/*
 * A short, static, English description of a status code. Never returns NULL; an unknown
 * value gives "unknown error". Safe to call at any time, from any thread.
 * [GATE G9] Not in the earlier documents; added so programs can print a code.
 */
const char* pm_strerror(int status);

/* ------------------------------------------------------------------- start and finish */

/*
 * Job configuration, passed to pm_init(). A zero in a field means "use the default", so
 * `(pm_config){0}` and a NULL pointer both mean all defaults.
 * [GATE G2] The two fields are those of the Build Plan's sketch; their meanings below are
 * proposals, because the plan since moved the thread count and admission into pmd.
 */
typedef struct pm_config {
    /*
     * Size of the shared region in bytes, at most PM_REGION_MAX_BYTES. Rounded up to a whole
     * 2 MB segment. 0 means the size given at launch, or the full 4 GB if none was given.
     * [GATE G2] The plan refuses an oversized job "before any process starts" (M4-4), but
     * this value is only known once the launcher's pm_init() runs. Draft: the launcher's
     * pm_init() sends the size to its pmd, which admits or refuses before any worker starts,
     * and pm_init() returns PM_ERR_REFUSED on refusal.
     */
    uint64_t region_bytes;
    /*
     * Most worker threads this job wants on each node. 0 means as many as each node's pmd
     * grants. The number running is never above the quota pmd pushes (its --cap-cores).
     * [GATE G2] In the Build Plan this was the thread count itself; the plan's quota from pmd
     * now sets it, so the draft keeps the field only as an upper bound.
     */
    uint32_t threads_per_node;
} pm_config;

/*
 * Starts or joins the job. Call it once, first thing in main(), on the main thread.
 *
 * On the launcher it maps the shared region, connects to the other nodes of the job, and
 * returns PM_OK when every node has joined; main() then continues. On a worker it does the
 * same, then runs tasks until the job ends and exits the process: it does not return.
 *
 * argc and argv are those of main(), or both NULL. cfg may be NULL.
 * Errors, launcher only: PM_ERR_INVALID (cfg out of range), PM_ERR_STATE (called twice),
 * PM_ERR_PLATFORM, PM_ERR_CONFIG, PM_ERR_REFUSED, PM_ERR_NETWORK, PM_ERR_NOMEM. A worker
 * that cannot start prints the reason and exits.
 * [GATE G1] Signature as in the Build Plan's sketch. The plan passes all settings to job
 * processes as environment variables, so argc and argv are not used today; the draft keeps
 * them, reserved, so the library can take its own options later without an API change.
 */
int pm_init(int* argc, char*** argv, const pm_config* cfg);

/*
 * Ends the job. Call it once, on the launcher's main thread, after the last pm_wait_all().
 * It tells every worker to exit and releases the region; shared memory must not be touched
 * afterwards. Errors: PM_ERR_STATE (not initialised, called twice, or work still outstanding).
 * [GATE G9] Returns int where the Build Plan's sketch returned void, so that a program can
 * notice it finished with work still queued.
 */
int pm_finalize(void);

/* ---------------------------------------------------------------------- shared memory */

/*
 * Allocates bytes of shared memory and returns its address, the same on every node.
 * Launcher only, from main(), and not while tasks are running. The memory reads as zeros
 * until written and lasts until pm_finalize(); there is no pm_free().
 *
 * An allocation of PM_PAGE_SIZE bytes or more starts on a page boundary and owns its pages
 * to the end, so two such allocations never share a page. A smaller one is aligned to 16 bytes.
 * Returns NULL when the region is full, when bytes is 0, or when called from a task.
 * [GATE G11] The alignment rule and the absence of pm_free() are proposals. The Build Plan
 * says large allocations are page-aligned and that pm_free() "can be a no-op", and it rules
 * out pm_malloc() from workers.
 */
void* pm_malloc(size_t bytes);

/* ------------------------------------------------------------------------------ tasks */

/*
 * What a running task is told about where it runs.
 * [GATE G3] node and thread are from the Build Plan's sketch; arg_len is added.
 */
typedef struct pm_task_ctx {
    uint16_t node;    /* ID of the node running this chunk. */
    uint16_t thread;  /* Index of the worker thread on that node, from 0. */
    uint32_t arg_len; /* Length in bytes of the argument passed to pm_parallel_for(). */
} pm_task_ctx;

/*
 * A task body. It processes the indexes lo <= i < hi of the range given to
 * pm_parallel_for(). arg points to a private, read-only copy of that call's argument; it is
 * valid only until the function returns. ctx is valid for the same time.
 *
 * A task must be safe to run twice on the same indexes, because a chunk is run again when
 * its node leaves before finishing it. Overwrite outputs; add to a shared total only with
 * pm_atomic_add(), as the last thing the task does.
 * A C++ task may throw: the runtime catches it and aborts the job with the task's name.
 * [GATE G3] Parameters as in the Build Plan's sketch.
 */
typedef void (*pm_task_fn)(const pm_task_ctx* ctx, uint64_t lo, uint64_t hi, const void* arg);

/*
 * Registers fn under name. PM_TASK calls it; programs do not call it directly. It must run
 * before pm_init(), which PM_TASK guarantees. name must be a string literal or otherwise
 * outlive the program's start-up. On the wire a task is the 64-bit FNV-1a hash of its name.
 * Two registrations with the same name, or two names with the same hash, stop the program in
 * pm_init() with a message naming both. Returns PM_OK, or PM_ERR_INVALID for a NULL argument
 * or an empty name.
 */
int pm_register_task(const char* name, pm_task_fn fn);

/*
 * Defines a task and registers it at program start. Use it at file scope, followed by the
 * body, in a C or a C++ file:
 *
 *     PM_TASK(scale_rows) {
 *         const scale_args* a = arg;
 *         for (uint64_t i = lo; i < hi; i++) { ... }
 *     }
 *
 * The body sees four parameters named ctx, lo, hi and arg, as in pm_task_fn. The task's name
 * for pm_parallel_for() is the macro argument as a string: "scale_rows". The function has
 * internal linkage, so the same name may not be registered from two files.
 * [GATE G3] The four parameter names are fixed by the macro. M3-1 may change the macro's
 * body (how registration happens), not how it is used.
 */
#define PM_TASK(name)                                                                          \
    static void name(const pm_task_ctx*, uint64_t, uint64_t, const void*);                     \
    __attribute__((constructor)) static void pm_register_task_##name(void) {                   \
        (void)pm_register_task(#name, name);                                                   \
    }                                                                                          \
    static void name(const pm_task_ctx* ctx __attribute__((unused)),                           \
                     uint64_t lo __attribute__((unused)), uint64_t hi __attribute__((unused)), \
                     const void* arg __attribute__((unused)))

/*
 * Queues the task named task to run over the indexes lo <= i < hi and returns at once.
 * The range is cut into chunks of about grain indexes; grain 0 lets the runtime choose
 * (configuration key task.default_grain). Chunks run on any node of the job, in any order,
 * each exactly once unless its node leaves, in which case it is run again elsewhere.
 * The arg_len bytes at arg are copied before the call returns and each chunk receives the
 * copy; arg may be NULL when arg_len is 0. To give tasks access to shared data, put pointers
 * from pm_malloc() in the argument.
 *
 * Launcher only, from main(). Several calls may be outstanding before one pm_wait_all().
 * Errors: PM_ERR_INVALID (task NULL, lo > hi, arg NULL with arg_len > 0, arg_len over
 * PM_TASK_ARG_MAX), PM_ERR_UNKNOWN_TASK, PM_ERR_STATE, PM_ERR_NOMEM. lo == hi queues nothing
 * and returns PM_OK.
 * [GATE G4] Signature as in the Build Plan's sketch.
 * [GATE G5] Still open #13: the draft takes the number of indexes in a chunk as its amount of
 * work when the ledger values it, so a task declares nothing and the API has no call for it.
 */
int pm_parallel_for(const char* task, uint64_t lo, uint64_t hi, uint64_t grain, const void* arg,
                    size_t arg_len);

/*
 * As pm_parallel_for(), and also tells the runtime which shared array the range indexes:
 * index i covers the stride bytes starting at (char*)data + i * stride. The runtime uses it
 * for two things. It places chunk boundaries on page boundaries of that array where the
 * stride allows, so two chunks do not write to the same page. And it prefers to give a node
 * the chunks whose pages that node is home for.
 *
 * data must point into memory from pm_malloc() and stride must be above 0; the bytes for
 * lo to hi must lie inside one allocation. Further error: PM_ERR_INVALID when they do not.
 * [GATE G4] New. The plan cuts ranges into "page-aligned chunks" (M3-2) and prefers chunks
 * "whose pages are homed on the asking node" (M3-3), but a range of indexes alone says
 * nothing about pages. This call supplies the link; pm_parallel_for() is this call with no
 * array, and gets neither alignment nor affinity.
 */
int pm_parallel_for_data(const char* task, uint64_t lo, uint64_t hi, uint64_t grain,
                         const void* arg, size_t arg_len, const void* data, size_t stride);

/*
 * Blocks until every chunk queued so far has finished, then returns PM_OK. The calling
 * thread sleeps; it runs no chunks itself. Results are in shared memory when it returns.
 * Launcher only, from main(). With nothing outstanding it returns at once.
 * Errors: PM_ERR_STATE.
 */
int pm_wait_all(void);

/* -------------------------------------------------------------------- synchronisation */

/*
 * A lock shared by every node of the job. The handle is a plain value: copy it, pass it in a
 * task argument, or store it in shared memory. All zero bytes is never a valid lock.
 * Locks are kept by the launcher, are granted first come first served, and last until
 * pm_finalize(). They are not recursive.
 * [GATE G6] Still open #1, "what pm_lock_t contains": the draft makes it one 64-bit number
 * wrapped in a struct, so a lock cannot be confused with an integer.
 */
typedef struct pm_lock_t {
    uint64_t id;
} pm_lock_t;

/*
 * Creates a lock. Launcher only, from main(). On failure the returned handle has id 0.
 * [GATE G6] Launcher-only keeps lock creation a local operation; the HLD has no opcode for it.
 */
pm_lock_t pm_lock_create(void);

/*
 * pm_lock() blocks until the calling thread holds the lock. pm_unlock() releases it and must
 * be called by the thread that holds it. Callable from a task or from the launcher's main().
 * Errors: PM_ERR_INVALID (not a lock; pm_lock() by the thread that already holds it;
 * pm_unlock() by a thread that does not), PM_ERR_STATE.
 * A task must release every lock it took before it returns.
 * [GATE G6] Both return int where the Build Plan's sketch returned void.
 */
int pm_lock(pm_lock_t lock);
int pm_unlock(pm_lock_t lock);

/*
 * A barrier shared by every node of the job: pm_barrier_wait() blocks each caller until
 * `count` threads are waiting, then releases them all, and the barrier can be used again.
 * The handle is a plain value like pm_lock_t; all zero bytes is never a valid barrier.
 * [GATE G7] The plan (FR-5.3, M2-4), the HLD (BARRIER_ENTER, BARRIER_RELEASE) and the Build
 * Plan (a pm_barrier_t handle) all name barriers, but none of the documents available gives
 * a function for them. These three declarations are a proposal modelled on POSIX barriers.
 */
typedef struct pm_barrier_t {
    uint64_t id;
} pm_barrier_t;

/* Creates a barrier for count threads, count >= 1. Launcher only, from main(). On failure
   the returned handle has id 0. */
pm_barrier_t pm_barrier_create(uint32_t count);

/* Waits at the barrier. Callable from a task or from the launcher's main().
   Errors: PM_ERR_INVALID (not a barrier), PM_ERR_STATE. */
int pm_barrier_wait(pm_barrier_t barrier);

/*
 * Adds delta to the 64-bit number at addr as one indivisible step across the whole job, and
 * returns the value it had before. The addition wraps modulo 2^64. It is carried out at the
 * page's home node, so it costs one message and does not move the page to the caller.
 * addr must be inside the shared region and a multiple of 8; anything else aborts the job
 * with a message, because the return value cannot carry an error.
 * Callable from a task or from the launcher's main().
 * [GATE G10] Signature as in the Build Plan's sketch. Aborting on a bad address is a proposal.
 */
uint64_t pm_atomic_add(uint64_t* addr, uint64_t delta);

/* ------------------------------------------------------- system calls on shared memory */

/* What the caller is about to do with a range of shared memory. */
typedef enum pm_access {
    PM_ACCESS_READ = 0, /* The range will only be read, for example by write(2). */
    PM_ACCESS_WRITE = 1 /* The range will be written, for example by read(2). */
} pm_access;

/*
 * Brings the n bytes at p onto this node with the access asked for, and returns when they
 * are here. Needed before handing shared memory to a system call: the kernel does not fetch
 * a missing shared page on a program's behalf, so read(fd, shared_buffer, n) on a page this
 * node does not hold fails with EFAULT. Ordinary reads and writes by the program itself need
 * no pm_touch().
 *
 * Another node may take a page back at any moment, so a system call can still fail with
 * EFAULT straight after pm_touch(); call pm_touch() again and retry.
 * Callable from a task or from the launcher's main(). n == 0 does nothing.
 * Errors: PM_ERR_INVALID (the range is not all inside the shared region, or access is not
 * one of the two values), PM_ERR_STATE.
 * [GATE G8] The Build Plan's sketch was void pm_touch(void* p, size_t n). The draft adds the
 * access, because a buffer the kernel will write needs write access while one it will only
 * read should not take the page away from other readers, and returns int.
 */
int pm_touch(const void* p, size_t n, pm_access access);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* NOLINTEND(readability-identifier-naming, modernize-use-using, modernize-deprecated-headers,
 *           modernize-macro-to-enum, cppcoreguidelines-macro-to-enum,
 *           cppcoreguidelines-macro-usage, performance-enum-size)
 */

#endif /* PARAMESH_H */
