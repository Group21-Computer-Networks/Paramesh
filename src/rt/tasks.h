// The task queue and the worker threads (docs/PROTOCOL.md, section 7.3). The launcher cuts
// each pm_parallel_for call into chunks and keeps them in one queue; every worker thread of
// every process asks it for a chunk with TASK_REQ, runs it, and reports TASK_DONE. The
// launcher's own threads ask the queue directly. Part of the runtime behind rt/runtime.h;
// built by M3-2.
//
// Threads: worker threads are started by start(); parallel_for is called by the launcher's
// main(); on_frame is called on the network thread and never waits. The one mutex is held
// only for a few instructions, never while a task body runs.
#ifndef PARAMESH_RT_TASKS_H
#define PARAMESH_RT_TASKS_H

#include "net/transport.h"
#include "platform/ids.h"
#include "platform/result.h"
#include "rt/runtime.h"
#include "rt/sync.h"
#include "wire/frame.h"
#include "wire/payloads.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace paramesh {

struct TasksConfig {
    std::uint16_t threads = 1;      // worker threads in this process
    std::uint32_t total_slots = 1;  // worker threads in the whole job, as far as is known
    std::uint64_t region_bytes = 0;
    // The plan's tunables, with its proposed defaults.
    Nanos backoff_initial = std::chrono::milliseconds{10};  // task.no_task_backoff: from ...
    Nanos backoff_max = std::chrono::milliseconds{200};     // ... doubling to
    std::uint32_t prefetch = 1;                             // task.prefetch
    std::uint32_t chunks_per_slot = 4;  // task.default_grain: range / (this x total_slots)
};

// Cuts lo <= i < hi into chunks of about `grain` indexes (at least 1). With an array, where
// index i covers `stride` bytes from address `data` + i * stride, every cut falls on a page
// boundary of the array if the stride allows one: it is a whole number of pages, or it
// divides a page. data 0 means no array.
std::vector<std::pair<std::uint64_t, std::uint64_t>> rt_cut_range(std::uint64_t lo,
                                                                  std::uint64_t hi,
                                                                  std::uint64_t grain,
                                                                  std::uint64_t data,
                                                                  std::uint64_t stride);

class Tasks {
public:
    // `transport`, `host` and `sync` must outlive this. The process is the launcher when
    // host.self() == host.launcher(). `sync` keeps the count of chunks not yet done, which
    // pm_wait_all waits on.
    Tasks(Transport& transport, RuntimeHost& host, Sync& sync, const TasksConfig& config);
    Tasks(const Tasks&) = delete;
    Tasks& operator=(const Tasks&) = delete;
    Tasks(Tasks&&) = delete;
    Tasks& operator=(Tasks&&) = delete;
    ~Tasks();

    // pm_parallel_for and pm_parallel_for_data. Errc::kState unless called by the launcher's
    // main(); kNotFound for a task nobody registered; kInvalidArgument for a bad range,
    // argument or array.
    Result<void> parallel_for(const ParallelFor& call);

    // Starts the worker threads; stop() ends them and returns when they have exited. A thread
    // inside a task body finishes it first.
    Result<void> start();
    void stop() noexcept;

    // A TASK_REQ, TASK_ASSIGN, NO_TASK or TASK_DONE frame arrived.
    void on_frame(const FrameHeader& header, std::span<const std::byte> payload) noexcept;

private:
    struct Call {
        std::uint64_t task_id = 0;
        std::vector<std::byte> arg;
        std::uint64_t chunks_left = 0;
    };
    struct Chunk {
        std::uint64_t id = 0;
        std::uint32_t call = 0;
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
    };
    struct Worker {
        std::thread thread;
        std::deque<TaskAssignPayload> have;  // chunks assigned to this thread, not yet run
        std::uint32_t asking = 0;            // TASK_REQs of this thread not yet answered
        bool refused = false;                // the last answer was NO_TASK: wait before asking
        bool no_more = false;                // NO_TASK reason 2: stop asking
    };

    void work(std::uint16_t thread);
    void ask(std::uint16_t thread);
    bool take(TaskAssignPayload& out);
    void done(std::uint64_t chunk_id, NodeId ran_by, std::uint64_t cpu_ns);
    template <typename Payload>
    void send(NodeId to, Opcode opcode, ReqId req, const Payload& payload, ReplyTimer timer);

    Transport& transport_;
    RuntimeHost& host_;
    Sync& sync_;
    const TasksConfig config_;
    const bool launcher_;

    std::mutex mu_;
    std::condition_variable changed_;
    bool stopping_ = false;
    // Launcher: the chunks nobody has taken, those taken and not reported done, and the calls
    // they belong to.
    std::deque<Chunk> queue_;
    std::unordered_map<std::uint64_t, Chunk> taken_;
    std::unordered_map<std::uint32_t, Call> calls_;
    std::uint64_t next_chunk_ = 1;
    std::uint32_t next_call_ = 1;
    // Every process: its worker threads, and which of them sent each unanswered TASK_REQ.
    std::vector<Worker> workers_;
    std::unordered_map<std::uint64_t, std::uint16_t> asked_;
    // ponytail: request numbers from the top quarter, apart from Sync's (bit 63 alone) and
    // from src/lib/'s page requests (from 1); one shared counter if RuntimeHost grows.
    std::uint64_t next_req_ = std::uint64_t{3} << 62U;
};

}  // namespace paramesh

#endif  // PARAMESH_RT_TASKS_H
