// Each of these runs on the real runtime; the server's tests compile and run them all.
window.PM_EXAMPLES = [
  { name: "A plain C program", source: `#include <stdio.h>

int main(void) {
    for (int i = 1; i <= 3; i++) {
        printf("line %d, from one process\\n", i);
    }
    return 0;
}
` },
  { name: "pm_init and pm_finalize", source: `#include <paramesh.h>
#include <stdio.h>

int main(int argc, char** argv) {
    /* On a worker, pm_init never returns: it serves the job until it ends. */
    const pm_config config = {.region_bytes = 1 << 20};
    int status = pm_init(&argc, &argv, &config);
    if (status != PM_OK) {
        fprintf(stderr, "pm_init: %s\\n", pm_strerror(status));
        return 1;
    }
    printf("the job is up\\n");
    return pm_finalize() == PM_OK ? 0 : 1;
}
` },
  { name: "pm_malloc: shared memory", source: `#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>

int main(int argc, char** argv) {
    const pm_config config = {.region_bytes = 1 << 20};
    if (pm_init(&argc, &argv, &config) != PM_OK) {
        return 1;
    }
    uint64_t* numbers = pm_malloc(1000 * sizeof(uint64_t));
    if (numbers == NULL) {
        return 1;
    }
    for (int i = 0; i < 1000; i++) {
        numbers[i] = i * 2;
    }
    printf("numbers[999] = %llu\\n", (unsigned long long)numbers[999]);
    return pm_finalize() == PM_OK ? 0 : 1;
}
` },
  { name: "PM_TASK and pm_parallel_for", source: `#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>

struct Args {
    uint64_t* squares;
};

/* Runs on any node, for lo <= i < hi. Its printf shows under the worker's output. */
PM_TASK(square) {
    const struct Args* a = arg;
    for (uint64_t i = lo; i < hi; i++) {
        a->squares[i] = i * i;
    }
}

int main(int argc, char** argv) {
    const uint64_t count = 100000;
    const pm_config config = {.region_bytes = 2 << 20};
    if (pm_init(&argc, &argv, &config) != PM_OK) {
        return 1;
    }
    const struct Args args = {pm_malloc(count * sizeof(uint64_t))};
    if (args.squares == NULL ||
        pm_parallel_for("square", 0, count, 0, &args, sizeof args) != PM_OK ||
        pm_wait_all() != PM_OK) {
        return 1;
    }
    printf("squares[99999] = %llu\\n", (unsigned long long)args.squares[99999]);
    return pm_finalize() == PM_OK ? 0 : 1;
}
` },
  { name: "pm_parallel_for_data: tasks near their data", source: `#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>

struct Args {
    uint64_t* values;
};

PM_TASK(fill) {
    const struct Args* a = arg;
    for (uint64_t i = lo; i < hi; i++) {
        a->values[i] = i + 1;
    }
}

int main(int argc, char** argv) {
    const uint64_t count = 500000;
    const pm_config config = {.region_bytes = 8 << 20};
    if (pm_init(&argc, &argv, &config) != PM_OK) {
        return 1;
    }
    const struct Args args = {pm_malloc(count * sizeof(uint64_t))};
    /* Each chunk is preferred by the node that is home for its part of the array. */
    if (args.values == NULL ||
        pm_parallel_for_data("fill", 0, count, 0, &args, sizeof args, args.values,
                             sizeof(uint64_t)) != PM_OK ||
        pm_wait_all() != PM_OK) {
        return 1;
    }
    printf("last value: %llu\\n", (unsigned long long)args.values[count - 1]);
    return pm_finalize() == PM_OK ? 0 : 1;
}
` },
  { name: "pm_wait_all: tasks in two rounds", source: `#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>

struct Args {
    uint64_t* cells;
};

PM_TASK(first_round) {
    const struct Args* a = arg;
    for (uint64_t i = lo; i < hi; i++) {
        a->cells[i] = i;
    }
}

PM_TASK(second_round) {
    const struct Args* a = arg;
    for (uint64_t i = lo; i < hi; i++) {
        a->cells[i] *= 2; /* sees what the first round wrote, on any node */
    }
}

int main(int argc, char** argv) {
    const uint64_t count = 100000;
    const pm_config config = {.region_bytes = 2 << 20};
    if (pm_init(&argc, &argv, &config) != PM_OK) {
        return 1;
    }
    const struct Args args = {pm_malloc(count * sizeof(uint64_t))};
    if (args.cells == NULL ||
        pm_parallel_for("first_round", 0, count, 0, &args, sizeof args) != PM_OK ||
        pm_wait_all() != PM_OK || /* the first round is over before the second starts */
        pm_parallel_for("second_round", 0, count, 0, &args, sizeof args) != PM_OK ||
        pm_wait_all() != PM_OK) {
        return 1;
    }
    printf("cells[50000] = %llu\\n", (unsigned long long)args.cells[50000]);
    return pm_finalize() == PM_OK ? 0 : 1;
}
` },
  { name: "pm_atomic_add: sum from every node", source: `#include <paramesh.h>
#include <stdint.h>
#include <stdio.h>

struct Args {
    uint64_t* total;
};

PM_TASK(add_up) {
    const struct Args* a = arg;
    uint64_t sum = 0;
    for (uint64_t i = lo; i < hi; i++) {
        sum += i;
    }
    pm_atomic_add(a->total, sum); /* one message to the page's home: no update is lost */
}

int main(int argc, char** argv) {
    const uint64_t count = 10000000;
    const pm_config config = {.region_bytes = 1 << 20};
    if (pm_init(&argc, &argv, &config) != PM_OK) {
        return 1;
    }
    const struct Args args = {pm_malloc(sizeof(uint64_t))};
    if (args.total == NULL ||
        pm_parallel_for("add_up", 0, count, 0, &args, sizeof args) != PM_OK ||
        pm_wait_all() != PM_OK) {
        return 1;
    }
    const uint64_t total = *args.total;
    printf("%llu (%s)\\n", (unsigned long long)total,
           total == count * (count - 1) / 2 ? "right" : "WRONG");
    return pm_finalize() == PM_OK ? 0 : 1;
}
` },
];
