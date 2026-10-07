// The task runtime: what src/lib/ calls to implement the task, lock, barrier, atomic and
// allocation functions of paramesh.h, and what the runtime needs from the rest of the job
// process in return.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 8. Implemented by M1-6
// (allocate), M2-4 (locks and barriers), M2-5 (atomic_add), M3-1 (the task registry) and
// M3-2 (parallel_for, workers).
#ifndef PARAMESH_RT_RUNTIME_H
#define PARAMESH_RT_RUNTIME_H

#include "net/transport.h"
#include "platform/ids.h"
#include "platform/result.h"
#include "wire/frame.h"

#include <paramesh.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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

// Each function carries out the paramesh.h function it is named after, and its Result maps
// to that function's pm_status: kInvalidArgument to PM_ERR_INVALID, kState to PM_ERR_STATE,
// kNotFound to PM_ERR_UNKNOWN_TASK, kNoMemory to PM_ERR_NOMEM.
class Runtime {
public:
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    virtual ~Runtime() = default;

    // pm_malloc. nullptr when the region is full or the caller is not the launcher's main().
    virtual void* allocate(std::size_t bytes) noexcept = 0;

    virtual Result<void> parallel_for(const ParallelFor& call) = 0;
    virtual Result<void> wait_all() = 0;

    virtual pm_lock_t lock_create() noexcept = 0;
    virtual Result<void> lock(pm_lock_t lock) = 0;
    virtual Result<void> unlock(pm_lock_t lock) = 0;
    virtual pm_barrier_t barrier_create(std::uint32_t count) noexcept = 0;
    virtual Result<void> barrier_wait(pm_barrier_t barrier) = 0;

    // pm_atomic_add: returns the old value. A bad address ends the job through
    // RuntimeHost::abort_job.
    virtual std::uint64_t atomic_add(std::uint64_t* addr, std::uint64_t delta) = 0;

    // A frame of the task or synchronisation group arrived (TASK_*, LOCK_*, BARRIER_*,
    // ATOMIC_RESULT). Called on the network thread; must not block.
    virtual void on_frame(const FrameHeader& header,
                          std::span<const std::byte> payload) noexcept = 0;

    // pmd pushed a new thread quota. A thread over the quota parks at its next task boundary.
    virtual void set_quota(std::uint16_t threads) noexcept = 0;

    // A member is leaving or has left: give it no more chunks, and after `grace` put the
    // chunks it has not finished back in the queue. Launcher only.
    virtual void member_leaving(NodeId node, Nanos grace) noexcept = 0;

    // Starts the worker threads; stop() ends them and returns when they have exited.
    virtual Result<void> start() = 0;
    virtual void stop() noexcept = 0;
};

struct RuntimeConfig {
    std::uint64_t region_bytes = 0;
    std::uint16_t threads = 0;  // the first quota
    bool is_launcher = false;
};

// `transport` and `host` must outlive the runtime.
Result<std::unique_ptr<Runtime>> rt_open(const RuntimeConfig& config, Transport& transport,
                                         RuntimeHost& host);

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
