// The task runtime: what it needs from the job process (RuntimeHost), one pm_parallel_for
// call as it is handed over (ParallelFor), and the task registry.
//
// The runtime is three pieces, each owned by the job process in src/lib/, its only user:
// rt/sync.h (locks, barriers, the count pm_wait_all waits on), rt/tasks.h (the queue and the
// worker threads) and rt/registry.h (looking tasks up and calling them). The allocator behind
// pm_malloc is rt/region_allocator.h. Those headers are not part of the frozen interface.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 8. Changed at the M3 gate
// (docs/logs/X-m3-runtime-api.md): the single Runtime class and rt_open() drafted here were
// never built and are gone.
#ifndef PARAMESH_RT_RUNTIME_H
#define PARAMESH_RT_RUNTIME_H

#include "platform/ids.h"
#include "platform/result.h"
#include "wire/frame.h"

#include <paramesh.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace paramesh {

// What the runtime needs from the job process. Implemented by src/lib/, and by a fake in the
// runtime's unit tests.
class RuntimeHost {
public:
    RuntimeHost() = default;
    RuntimeHost(const RuntimeHost&) = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;
    RuntimeHost(RuntimeHost&&) = delete;
    RuntimeHost& operator=(RuntimeHost&&) = delete;
    virtual ~RuntimeHost() = default;

    [[nodiscard]] virtual NodeId self() const noexcept = 0;
    [[nodiscard]] virtual NodeId launcher() const noexcept = 0;
    // The home of a page under the current segment map, for ATOMIC_OP and for data affinity.
    [[nodiscard]] virtual NodeId home_of(PageId page) const noexcept = 0;

    // The one way to end the job on a fatal condition: logs the reason, tells the launcher
    // (or every process, on the launcher) with JOB_END, and exits the process.
    [[noreturn]] virtual void abort_job(Status status, std::string_view message) noexcept = 0;

    // A chunk finished. `ran_by` is this node for a chunk this process ran, with the CPU time
    // it measured; on the launcher it is also called for chunks peers completed, with 0.
    // src/lib/ reports it to pmd as L_CHUNK.
    virtual void chunk_finished(NodeId ran_by, std::uint64_t task_id, std::uint64_t indexes,
                                Nanos cpu) noexcept = 0;
};

// One pm_parallel_for or pm_parallel_for_data call.
struct ParallelFor {
    std::string_view task;
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::uint64_t grain = 0;
    std::span<const std::byte> arg;
    const void* data = nullptr;  // nullptr for pm_parallel_for
    std::size_t stride = 0;
};

// The task registry is process-wide and filled before pm_init() by PM_TASK's constructors.
// Errc::kInvalidArgument for an empty name or a null function.
Result<void> rt_register_task(const char* name, pm_task_fn fn) noexcept;
// 64-bit FNV-1a of the name: the task's ID on the wire.
std::uint64_t rt_task_id(std::string_view name) noexcept;
// Errc::kInvalidArgument, with the two names in Error::what, if two registered names are
// equal or share an ID. pm_init() calls it and stops the program on failure.
Result<void> rt_check_registry() noexcept;

}  // namespace paramesh

#endif  // PARAMESH_RT_RUNTIME_H
