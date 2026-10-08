// The task registry, and the one place application task bodies are called. For src/rt/ and
// src/lib/; what other directories may use is in rt/runtime.h (rt_register_task, rt_task_id,
// rt_check_registry). Built by M3-1.
#ifndef PARAMESH_RT_REGISTRY_H
#define PARAMESH_RT_REGISTRY_H

#include "platform/result.h"
#include "rt/runtime.h"

#include <paramesh.h>

#include <cstdint>

namespace paramesh {

// The function registered under this ID, or nullptr; and its name, or "".
pm_task_fn rt_find_task(std::uint64_t id) noexcept;
const char* rt_task_name(std::uint64_t id) noexcept;

// rt_register_task with the ID given instead of computed. For tests: the only way to have two
// names with one ID without searching for a real collision of a 64-bit hash.
Result<void> rt_register_task_as(const char* name, std::uint64_t id, pm_task_fn fn) noexcept;

// Runs the chunk lo <= i < hi of task `id`. If the body throws, the job is ended through
// `host` with status TASK_FAILED and a message naming the task and what it threw; so is a
// call for an ID nobody registered.
void rt_run_task(RuntimeHost& host, std::uint64_t id, const pm_task_ctx& ctx, std::uint64_t lo,
                 std::uint64_t hi, const void* arg) noexcept;

}  // namespace paramesh

#endif  // PARAMESH_RT_REGISTRY_H
