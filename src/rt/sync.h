// Locks, barriers and the launcher's wait for outstanding work (docs/PROTOCOL.md, section 7.5).
// The launcher keeps every lock and barrier; any process asks it with LOCK_ACQ, LOCK_REL and
// BARRIER_ENTER, and the launcher's own threads ask it directly. Part of the runtime behind
// rt/runtime.h; built by M2-4.
//
// Threads: the calls that wait (lock, barrier_wait, wait_all) are made by application threads;
// on_frame is called on the network thread and never waits. The one mutex is held only for a
// few instructions and never across a touch of shared memory.
#ifndef PARAMESH_RT_SYNC_H
#define PARAMESH_RT_SYNC_H

#include "net/transport.h"
#include "platform/ids.h"
#include "platform/result.h"
#include "rt/runtime.h"
#include "wire/frame.h"
#include "wire/payloads.h"

#include <paramesh.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace paramesh {

// The `thread` of the launcher's main thread (docs/PROTOCOL.md, LOCK_ACQ).
inline constexpr std::uint16_t kMainThread = 0xFFFF;

class Sync {
public:
    // `transport` and `host` must outlive this. The process is the launcher when
    // host.self() == host.launcher().
    Sync(Transport& transport, RuntimeHost& host);

    // Launcher only; elsewhere, and for a barrier of 0 threads, the handle has id 0.
    pm_lock_t lock_create() noexcept;
    pm_barrier_t barrier_create(std::uint32_t count) noexcept;

    // `thread` is the caller's index in its process, kMainThread for the launcher's main().
    // Errc::kInvalidArgument: not a lock or barrier; lock() by the thread that holds it;
    // unlock() by a thread that does not.
    Result<void> lock(pm_lock_t lock, std::uint16_t thread);
    Result<void> unlock(pm_lock_t lock, std::uint16_t thread);
    Result<void> barrier_wait(pm_barrier_t barrier, std::uint16_t thread);

    // pm_wait_all: the launcher counts the work it has handed out; wait_all() sleeps until
    // all of it is done and returns at once when none is outstanding. Errc::kState on a worker.
    void work_added(std::uint64_t items) noexcept;
    void work_done(std::uint64_t items) noexcept;
    Result<void> wait_all();

    // A LOCK_* or BARRIER_* frame arrived.
    void on_frame(const FrameHeader& header, std::span<const std::byte> payload) noexcept;

    // Launcher: does this thread of this node hold any lock? A chunk that has to be run again
    // while its thread holds one ends the job (paramesh.h, pm_lock).
    [[nodiscard]] bool holds_lock(NodeId node, std::uint16_t thread) const;

private:
    struct Waiter {
        NodeId node;
        std::uint16_t thread = 0;
        ReqId req;
    };
    struct Lock {
        bool held = false;
        Waiter holder;
        std::deque<Waiter> queue;  // first come, first served
    };
    struct Barrier {
        std::uint32_t count = 0;
        std::vector<Waiter> waiting;
    };

    Result<void> request(Opcode opcode, std::uint64_t id, std::uint16_t thread,
                         std::unique_lock<std::mutex>& hold);
    void manage(Opcode opcode, const Waiter& from, std::uint64_t id);
    void answer(const Waiter& to, Opcode opcode, std::uint64_t id, Status status);
    void send(NodeId to, Opcode opcode, ReqId req, const SyncPayload& payload);

    Transport& transport_;
    RuntimeHost& host_;
    const bool launcher_;

    mutable std::mutex mu_;
    std::condition_variable changed_;
    // Launcher: what it manages. One counter numbers both, so a lock is never a barrier.
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, Lock> locks_;
    std::unordered_map<std::uint64_t, Barrier> barriers_;
    std::uint64_t outstanding_ = 0;
    // Every process: its threads' requests still waiting (no value yet) or just answered, and
    // the locks its threads hold.
    // ponytail: request numbers come from the upper half so they never meet the ones src/lib/
    // gives its page requests on the same connection; one shared counter if RuntimeHost grows.
    std::uint64_t next_req_ = std::uint64_t{1} << 63U;
    std::unordered_map<std::uint64_t, std::optional<Status>> answers_;
    std::set<std::pair<std::uint64_t, std::uint16_t>> held_;
};

}  // namespace paramesh

#endif  // PARAMESH_RT_SYNC_H
