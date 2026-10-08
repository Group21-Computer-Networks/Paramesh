#include "rt/sync.h"

#include <algorithm>
#include <array>

namespace paramesh {

namespace {

constexpr Error kNotAHandle{Errc::kInvalidArgument, 0,
                            "not a lock or barrier, or the wrong thread for it"};

}  // namespace

std::uint16_t& rt_thread_index() noexcept {
    thread_local std::uint16_t index = kMainThread;
    return index;
}

Sync::Sync(Transport& transport, RuntimeHost& host)
    : transport_(transport), host_(host), launcher_(host.self() == host.launcher()) {}

pm_lock_t Sync::lock_create() noexcept {
    if (!launcher_) {
        return pm_lock_t{0};
    }
    const std::scoped_lock hold{mu_};
    locks_[next_id_];
    return pm_lock_t{next_id_++};
}

pm_barrier_t Sync::barrier_create(std::uint32_t count) noexcept {
    if (!launcher_ || count == 0) {
        return pm_barrier_t{0};
    }
    const std::scoped_lock hold{mu_};
    barriers_[next_id_].count = count;
    return pm_barrier_t{next_id_++};
}

Result<void> Sync::lock(pm_lock_t lock, std::uint16_t thread) {
    std::unique_lock hold{mu_};
    if (lock.id == 0 || held_.contains({lock.id, thread})) {
        return kNotAHandle;
    }
    Result<void> granted = request(Opcode::kLockAcq, lock.id, thread, hold);
    if (granted.ok()) {
        held_.insert({lock.id, thread});
    }
    return granted;
}

Result<void> Sync::unlock(pm_lock_t lock, std::uint16_t thread) {
    const std::scoped_lock hold{mu_};
    if (held_.erase({lock.id, thread}) == 0) {
        return kNotAHandle;
    }
    if (launcher_) {
        manage(Opcode::kLockRel, Waiter{host_.self(), thread, kNoReq}, lock.id);
    } else {
        send(host_.launcher(), Opcode::kLockRel, kNoReq, SyncPayload{lock.id, thread});
    }
    return {};
}

Result<void> Sync::barrier_wait(pm_barrier_t barrier, std::uint16_t thread) {
    std::unique_lock hold{mu_};
    if (barrier.id == 0) {
        return kNotAHandle;
    }
    return request(Opcode::kBarrierEnter, barrier.id, thread, hold);
}

void Sync::work_added(std::uint64_t items) noexcept {
    const std::scoped_lock hold{mu_};
    outstanding_ += items;
}

void Sync::work_done(std::uint64_t items) noexcept {
    const std::scoped_lock hold{mu_};
    outstanding_ -= std::min(items, outstanding_);
    changed_.notify_all();
}

Result<void> Sync::wait_all() {
    if (!launcher_) {
        return Error{Errc::kState, 0, "wait_all: only the launcher waits for the job's work"};
    }
    std::unique_lock hold{mu_};
    changed_.wait(hold, [this] { return outstanding_ == 0; });
    return {};
}

std::uint64_t Sync::outstanding() const {
    const std::scoped_lock hold{mu_};
    return outstanding_;
}

bool Sync::holds_lock(NodeId node, std::uint16_t thread) const {
    const std::scoped_lock hold{mu_};
    return std::any_of(locks_.begin(), locks_.end(), [&](const auto& entry) {
        const Lock& lock = entry.second;
        return lock.held && lock.holder.node == node && lock.holder.thread == thread;
    });
}

void Sync::on_frame(const FrameHeader& header, std::span<const std::byte> payload) noexcept {
    const Result<SyncPayload> decoded = wire_decode_sync(payload);
    if (!decoded.ok()) {
        host_.abort_job(Status::kProtocol, "a lock or barrier frame is malformed");
    }
    const SyncPayload& body = decoded.value();
    const std::scoped_lock hold{mu_};
    switch (header.opcode) {
        case Opcode::kLockAcq:
        case Opcode::kLockRel:
        case Opcode::kBarrierEnter:
            if (!launcher_) {
                host_.abort_job(Status::kProtocol, "a lock request reached a worker");
            }
            manage(header.opcode, Waiter{header.src, body.word, header.req}, body.id);
            break;
        case Opcode::kLockGrant:
        case Opcode::kBarrierRelease: {
            const auto waiting = answers_.find(header.req.value);
            if (waiting == answers_.end() || waiting->second.has_value()) {
                // The launcher now counts a thread as holding or released that never asked.
                host_.abort_job(Status::kProtocol, "a lock grant answers no request");
            }
            waiting->second = body.word == 0 ? Status::kOk : Status::kBadHandle;
            changed_.notify_all();
            break;
        }
        default:
            break;
    }
}

// Sends the request (or hands it to the manager, on the launcher) and sleeps until its answer.
Result<void> Sync::request(Opcode opcode, std::uint64_t id, std::uint16_t thread,
                           std::unique_lock<std::mutex>& hold) {
    const ReqId req{next_req_++};
    const auto mine = answers_.emplace(req.value, std::nullopt).first;
    if (launcher_) {
        manage(opcode, Waiter{host_.self(), thread, req}, id);
    } else {
        send(host_.launcher(), opcode, req, SyncPayload{id, thread});
    }
    // References into an unordered_map stay valid while other threads insert and erase.
    std::optional<Status>& status = mine->second;
    changed_.wait(hold, [&status] { return status.has_value(); });
    const bool ok = status == Status::kOk;
    answers_.erase(req.value);
    if (!ok) {
        return kNotAHandle;
    }
    return {};
}

// The launcher's side: one request against the lock or barrier it names.
void Sync::manage(Opcode opcode, const Waiter& from, std::uint64_t id) {
    if (opcode == Opcode::kBarrierEnter) {
        const auto found = barriers_.find(id);
        if (found == barriers_.end()) {
            answer(from, Opcode::kBarrierRelease, id, Status::kBadHandle);
            return;
        }
        Barrier& barrier = found->second;
        barrier.waiting.push_back(from);
        if (barrier.waiting.size() == barrier.count) {
            for (const Waiter& waiter : barrier.waiting) {
                answer(waiter, Opcode::kBarrierRelease, id, Status::kOk);
            }
            barrier.waiting.clear();  // and the barrier can be used again
        }
        return;
    }
    const auto found = locks_.find(id);
    if (opcode == Opcode::kLockAcq) {
        if (found == locks_.end()) {
            answer(from, Opcode::kLockGrant, id, Status::kBadHandle);
        } else if (found->second.held) {
            found->second.queue.push_back(from);
        } else {
            found->second.held = true;
            found->second.holder = from;
            answer(from, Opcode::kLockGrant, id, Status::kOk);
        }
        return;
    }
    // LOCK_REL. Its sender checked that the thread holds the lock, so anything else is a bug.
    if (found == locks_.end() || !found->second.held || found->second.holder.node != from.node ||
        found->second.holder.thread != from.thread) {
        host_.abort_job(Status::kProtocol, "a lock was released by a thread that does not hold it");
    }
    Lock& lock = found->second;
    lock.held = !lock.queue.empty();
    if (lock.held) {
        lock.holder = lock.queue.front();
        lock.queue.pop_front();
        answer(lock.holder, Opcode::kLockGrant, id, Status::kOk);
    }
}

void Sync::answer(const Waiter& to, Opcode opcode, std::uint64_t id, Status status) {
    if (to.node != host_.self()) {
        send(to.node, opcode, to.req, SyncPayload{id, static_cast<std::uint16_t>(status)});
        return;
    }
    const auto waiting = answers_.find(to.req.value);  // one of the launcher's own threads
    if (waiting != answers_.end()) {
        waiting->second = status;
        changed_.notify_all();
    }
}

void Sync::send(NodeId to, Opcode opcode, ReqId req, const SyncPayload& payload) {
    std::array<std::byte, 16> bytes{};
    FrameHeader header;
    header.opcode = opcode;
    header.dst = to;
    header.req = req;
    // LOCK_ACQ and BARRIER_ENTER wait as long as they must: no reply timer (PROTOCOL.md 9).
    if (!wire_encode(payload, bytes).ok() ||
        !transport_.send(to, header, bytes, ReplyTimer::kNone).ok()) {
        host_.abort_job(Status::kNodeLost, "a lock or barrier message could not be sent");
    }
}

}  // namespace paramesh
