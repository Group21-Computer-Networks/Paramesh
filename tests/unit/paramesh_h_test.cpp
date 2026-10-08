// include/paramesh.h: compiles as C++20 here and as C11 in paramesh_h_test.c, declares what
// the plan names, and PM_TASK registers a task from a C file and from a C++ file.
//
// The tasks are looked up in the real registry of src/rt/, by the hash of their names.

#include "paramesh.h"
#include "rt/registry.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace {

// The tasks' argument: where the sum of the indexes goes. paramesh_h_test.c has the same struct.
struct SumArgs {
    std::uint64_t* out;
};

pm_task_fn find_task(const std::string& name) {
    return paramesh::rt_find_task(paramesh::rt_task_id(name));
}

// Signatures, checked without calling anything.
static_assert(std::is_same_v<decltype(&pm_init), int (*)(int*, char***, const pm_config*)>);
static_assert(std::is_same_v<decltype(&pm_finalize), int (*)()>);
static_assert(std::is_same_v<decltype(&pm_strerror), const char* (*)(int)>);
static_assert(std::is_same_v<decltype(&pm_malloc), void* (*)(std::size_t)>);
static_assert(
    std::is_same_v<decltype(&pm_parallel_for), int (*)(const char*, std::uint64_t, std::uint64_t,
                                                       std::uint64_t, const void*, std::size_t)>);
static_assert(std::is_same_v<decltype(&pm_parallel_for_data),
                             int (*)(const char*, std::uint64_t, std::uint64_t, std::uint64_t,
                                     const void*, std::size_t, const void*, std::size_t)>);
static_assert(std::is_same_v<decltype(&pm_wait_all), int (*)()>);
static_assert(std::is_same_v<decltype(&pm_lock_create), pm_lock_t (*)()>);
static_assert(std::is_same_v<decltype(&pm_lock), int (*)(pm_lock_t)>);
static_assert(std::is_same_v<decltype(&pm_unlock), int (*)(pm_lock_t)>);
static_assert(std::is_same_v<decltype(&pm_barrier_create), pm_barrier_t (*)(std::uint32_t)>);
static_assert(std::is_same_v<decltype(&pm_barrier_wait), int (*)(pm_barrier_t)>);
static_assert(
    std::is_same_v<decltype(&pm_atomic_add), std::uint64_t (*)(std::uint64_t*, std::uint64_t)>);
static_assert(std::is_same_v<decltype(&pm_touch), int (*)(const void*, std::size_t, pm_access)>);

// Handles and the task context are plain values that can sit in shared memory.
static_assert(std::is_trivially_copyable_v<pm_lock_t> && std::is_standard_layout_v<pm_lock_t>);
static_assert(std::is_trivially_copyable_v<pm_barrier_t> &&
              std::is_standard_layout_v<pm_barrier_t>);
static_assert(sizeof(pm_lock_t) == 8 && sizeof(pm_barrier_t) == 8);
static_assert(std::is_trivially_copyable_v<pm_task_ctx> && sizeof(pm_task_ctx) == 8);
static_assert(std::is_trivially_copyable_v<pm_config> && std::is_standard_layout_v<pm_config>);

// The numbers the design fixes.
static_assert(PM_PAGE_SIZE == 4096);
static_assert(PM_REGION_MAX_BYTES == 4ULL * 1024 * 1024 * 1024);
static_assert(PM_TASK_ARG_MAX == 1024);

}  // namespace

// Same job as the C task in paramesh_h_test.c.
PM_TASK(paramesh_h_test_cpp_task) {
    const auto* args = static_cast<const SumArgs*>(arg);
    std::uint64_t sum = 0;
    for (std::uint64_t i = lo; i < hi; i++) {
        sum += i;
    }
    *args->out = sum + ctx->arg_len;
}

// A task that uses none of its parameters must still compile with warnings as errors.
PM_TASK(paramesh_h_test_empty_task) {}

TEST_CASE("PM_TASK registers tasks from a C file and a C++ file before main, found by hash") {
    CHECK(find_task("paramesh_h_test_c_task") != nullptr);
    CHECK(find_task("paramesh_h_test_cpp_task") != nullptr);
    CHECK(find_task("paramesh_h_test_empty_task") != nullptr);
    CHECK(find_task("never_registered") == nullptr);
}

TEST_CASE("a registered task is called with ctx, lo, hi and arg") {
    for (const char* name : {"paramesh_h_test_c_task", "paramesh_h_test_cpp_task"}) {
        CAPTURE(name);
        const pm_task_fn fn = find_task(name);
        REQUIRE(fn != nullptr);
        std::uint64_t result = 0;
        const SumArgs args{.out = &result};
        const pm_task_ctx ctx{.node = 7, .thread = 1, .arg_len = sizeof(args)};
        fn(&ctx, 3, 7, &args);
        CHECK(result == 3 + 4 + 5 + 6 + sizeof(args));
    }
}

TEST_CASE("error codes are zero for success and distinct negatives otherwise") {
    const std::vector<int> errors = {PM_ERR_INVALID,      PM_ERR_STATE,    PM_ERR_NOMEM,
                                     PM_ERR_UNKNOWN_TASK, PM_ERR_PLATFORM, PM_ERR_CONFIG,
                                     PM_ERR_REFUSED,      PM_ERR_NETWORK};
    CHECK(PM_OK == 0);
    for (std::size_t i = 0; i < errors.size(); i++) {
        CHECK(errors[i] < 0);
        CHECK(std::count(errors.begin(), errors.end(), errors[i]) == 1);
    }
}

TEST_CASE("a zeroed pm_config asks for every default") {
    const pm_config cfg{};
    CHECK(cfg.region_bytes == 0);
    CHECK(cfg.threads_per_node == 0);
}
