/* matmul: C = A x B for n x n matrices of doubles, with the rows of C computed as tasks.
 *
 *     matmul [--plain] [n] [threads]
 *
 * n defaults to 512. Without --plain it is a ParaMesh program: start it as a job with
 * tests/multi/run_local.sh (or pmrun, from M3-5), on one node or several. `threads` is the
 * most worker threads on each node; 0 or nothing means as many as each node allows.
 * With --plain it is an ordinary program on one thread that never calls ParaMesh: the same
 * arithmetic, for comparison.
 *
 * It prints one line with the time of the multiplication and a checksum of C, which is the
 * same for every way of running it, and, as a job, how many chunks each node ran.
 * tools/bench/matmul.sh runs the three ways and makes a table. */

#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NODE_IDS 65536 /* a node ID is 16 bits */

struct MultiplyArgs {
    const double* a;
    const double* b;
    double* c;
    uint64_t n;
    uint64_t* chunks; /* chunks[node]: how many chunks that node ran */
};

static double seconds_now(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC); /* C11; the wall clock is good enough for seconds */
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Rows lo <= i < hi of C. Each row is written afresh, so running a row twice is harmless. */
static void multiply_rows(const double* a, const double* b, double* c, uint64_t n, uint64_t lo,
                          uint64_t hi) {
    for (uint64_t i = lo; i < hi; i++) {
        double* row = c + i * n;
        memset(row, 0, n * sizeof(double));
        for (uint64_t k = 0; k < n; k++) {
            const double aik = a[i * n + k];
            const double* b_row = b + k * n;
            for (uint64_t j = 0; j < n; j++) {
                row[j] += aik * b_row[j];
            }
        }
    }
}

PM_TASK(matmul_rows) {
    const struct MultiplyArgs* args = arg;
    multiply_rows(args->a, args->b, args->c, args->n, lo, hi);
    pm_atomic_add(&args->chunks[ctx->node], 1); /* a shared total: last, and atomically */
}

/* Small whole numbers, so that every sum is exact and the checksum does not depend on the
 * order the rows were computed in. */
static void fill(double* a, double* b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        for (uint64_t j = 0; j < n; j++) {
            a[i * n + j] = (double)((i + 2 * j) % 7);
            b[i * n + j] = (double)((3 * i + j) % 5);
        }
    }
}

static double checksum(const double* c, uint64_t n) {
    double sum = 0;
    for (uint64_t i = 0; i < n * n; i++) {
        sum += c[i];
    }
    return sum;
}

static int run_plain(uint64_t n) {
    double* a = malloc(n * n * sizeof(double));
    double* b = malloc(n * n * sizeof(double));
    double* c = malloc(n * n * sizeof(double));
    const int have = a != NULL && b != NULL && c != NULL;
    if (have) {
        fill(a, b, n);
        const double start = seconds_now();
        multiply_rows(a, b, c, n, 0, n);
        const double took = seconds_now() - start;
        printf("matmul: plain, n %llu: %.3f s, checksum %.0f\n", (unsigned long long)n, took,
               checksum(c, n));
    } else {
        fprintf(stderr, "matmul: out of memory\n");
    }
    free(a);
    free(b);
    free(c);
    return have ? 0 : 1;
}

static int run_job(uint64_t n, uint32_t threads, int* argc, char*** argv) {
    const uint64_t matrix = n * n * sizeof(double);
    const uint64_t counts = NODE_IDS * sizeof(uint64_t);
    /* Three matrices and the counts, each rounded up to whole pages, and a page to spare. */
    const pm_config config = {.region_bytes = 3 * (matrix + PM_PAGE_SIZE) + counts + PM_PAGE_SIZE,
                              .threads_per_node = threads};
    const int status = pm_init(argc, argv, &config); /* a worker does not return from this */
    if (status != PM_OK) {
        fprintf(stderr, "matmul: pm_init: %s\n", pm_strerror(status));
        return 1;
    }
    double* a = pm_malloc(matrix);
    double* b = pm_malloc(matrix);
    double* c = pm_malloc(matrix);
    uint64_t* chunks = pm_malloc(counts);
    if (a == NULL || b == NULL || c == NULL || chunks == NULL) {
        fprintf(stderr, "matmul: the shared region is too small for n %llu\n",
                (unsigned long long)n);
        return 1;
    }
    fill(a, b, n);

    const struct MultiplyArgs args = {a, b, c, n, chunks};
    const double start = seconds_now();
    /* One index is one row of C: n doubles. The runtime cuts on page boundaries of C and
     * gives a node the rows it is home for. */
    int queued =
        pm_parallel_for_data("matmul_rows", 0, n, 0, &args, sizeof args, c, n * sizeof(double));
    if (queued == PM_OK) {
        queued = pm_wait_all();
    }
    const double took = seconds_now() - start;
    if (queued != PM_OK) {
        fprintf(stderr, "matmul: the tasks could not run: %s\n", pm_strerror(queued));
        return 1;
    }

    printf("matmul: job, n %llu: %.3f s, checksum %.0f\n", (unsigned long long)n, took,
           checksum(c, n));
    printf("matmul: chunks by node:");
    for (uint32_t node = 0; node < NODE_IDS; node++) {
        if (chunks[node] != 0) {
            printf(" %u:%llu", node, (unsigned long long)chunks[node]);
        }
    }
    printf("\n");
    return pm_finalize() == PM_OK ? 0 : 1;
}

int main(int argc, char** argv) {
    int plain = 0;
    int at = 1;
    if (at < argc && strcmp(argv[at], "--plain") == 0) {
        plain = 1;
        at++;
    }
    const uint64_t n = at < argc ? strtoull(argv[at], NULL, 10) : 512;
    const uint64_t threads = at + 1 < argc ? strtoull(argv[at + 1], NULL, 10) : 0;
    if (n == 0 || n > 8192 || threads > 1024) {
        fprintf(stderr, "usage: matmul [--plain] [n, 1 to 8192] [threads on each node]\n");
        return 2;
    }
    return plain ? run_plain(n) : run_job(n, (uint32_t)threads, &argc, &argv);
}
