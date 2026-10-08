// src/rt/: the bump allocator behind pm_malloc; locks, barriers and the wait for outstanding
// work, against a fake transport.

#include "rt/region_allocator.h"
#include "rt/sync.h"

#include <doctest/doctest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

TEST_CASE(
    "small allocations are 16-byte aligned; a page or more is page-aligned and owns its pages") {
    paramesh::RegionAllocator region{paramesh::kSegmentSize};
    CHECK(region.allocate(1) == 0U);
    CHECK(region.allocate(24) == 16U);
    CHECK(region.allocate(8) == 48U);  // 16 + 24 = 40, rounded up to 48

    CHECK(region.allocate(4096) == 4096U);  // the next page boundary
    CHECK(region.allocate(5000) == 8192U);  // takes two pages
    CHECK(region.allocate(8) == 16384U);    // does not share them
    CHECK(region.allocate(4096) == 20480U);
}

TEST_CASE("the allocator refuses zero bytes and anything past the end of the region") {
    paramesh::RegionAllocator region{2 * paramesh::kPageSize};
    CHECK_FALSE(region.allocate(0).has_value());
    CHECK_FALSE(region.allocate(3 * paramesh::kPageSize).has_value());
    CHECK(region.allocate(paramesh::kPageSize) == 0U);
    CHECK(region.allocate(paramesh::kPageSize) == paramesh::kPageSize);
    CHECK_FALSE(region.allocate(1).has_value());      // full
    CHECK_FALSE(region.allocate(~0ULL).has_value());  // no overflow
}

namespace {

using paramesh::FrameHeader;
using paramesh::NodeId;
using paramesh::Opcode;
using paramesh::ReqId;
using paramesh::Status;
using paramesh::Sync;
using paramesh::SyncPayload;

constexpr NodeId kLauncher{1};
constexpr std::uint16_t kOk = 0;
constexpr auto kBadHandle = static_cast<std::uint16_t>(Status::kBadHandle);

// Records what is sent, as "LOCK_GRANT>2 req 5 id 1 word 0", and lets a test wait for a send.
class FakeTransport final : public paramesh::Transport {
public:
    paramesh::Result<std::uint16_t> listen(paramesh::Endpoint /*local*/) override { return {0}; }
    paramesh::Result<void> add_peer(NodeId /*peer*/, paramesh::Endpoint /*remote*/) override {
        return {};
    }
    void remove_peer(NodeId /*peer*/) override {}
    paramesh::Result<void> send(NodeId to, const FrameHeader& header,
                                std::span<const std::byte> payload,
                                paramesh::ReplyTimer timer) override {
        const auto body = paramesh::wire_decode_sync(payload);
        const std::scoped_lock hold{mu_};
        timed_ = timed_ || timer == paramesh::ReplyTimer::kTimed;
        if (!body.ok() || header.dst != to) {
            sent_.emplace_back("malformed");
        } else {
            sent_.push_back(name(header.opcode) + ">" + std::to_string(to.value) + " req " +
                            std::to_string(header.req.value & 0xFFFFU) + " id " +
                            std::to_string(body.value().id) + " word " +
                            std::to_string(body.value().word));
            last_req_ = header.req;
        }
        changed_.notify_all();
        return {};
    }
    paramesh::TimerId start_timer(paramesh::Nanos /*delay*/) override { return {}; }
    void cancel_timer(paramesh::TimerId /*timer*/) override {}
    [[nodiscard]] paramesh::Nanos now() const noexcept override { return {}; }
    paramesh::Result<void> run() override { return {}; }
    void stop() noexcept override {}

    // Everything sent since the last call, joined by ", ".
    std::string take() {
        const std::scoped_lock hold{mu_};
        std::string all;
        for (const std::string& one : sent_) {
            all += (all.empty() ? "" : ", ") + one;
        }
        sent_.clear();
        return all;
    }
    // Sleeps until something has been sent, then returns it as take() does.
    std::string wait() {
        {
            std::unique_lock hold{mu_};
            changed_.wait(hold, [this] { return !sent_.empty(); });
        }
        return take();
    }
    ReqId last_req() {
        const std::scoped_lock hold{mu_};
        return last_req_;
    }
    bool timed() {
        const std::scoped_lock hold{mu_};
        return timed_;
    }

private:
    static std::string name(Opcode opcode) {
        switch (opcode) {
            case Opcode::kLockAcq:
                return "LOCK_ACQ";
            case Opcode::kLockGrant:
                return "LOCK_GRANT";
            case Opcode::kLockRel:
                return "LOCK_REL";
            case Opcode::kBarrierEnter:
                return "BARRIER_ENTER";
            case Opcode::kBarrierRelease:
                return "BARRIER_RELEASE";
            default:
                return "?";
        }
    }
    std::mutex mu_;
    std::condition_variable changed_;
    std::vector<std::string> sent_;
    ReqId last_req_;
    bool timed_ = false;
};

// abort_job ends the process with status 42, which the last test looks for in a child.
class FakeHost final : public paramesh::RuntimeHost {
public:
    explicit FakeHost(NodeId self) : self_(self) {}
    [[nodiscard]] NodeId self() const noexcept override { return self_; }
    [[nodiscard]] NodeId launcher() const noexcept override { return kLauncher; }
    [[nodiscard]] NodeId home_of(paramesh::PageId /*page*/) const noexcept override {
        return kLauncher;
    }
    [[noreturn]] void abort_job(Status /*status*/, std::string_view /*message*/) noexcept override {
        _exit(42);
    }
    void chunk_finished(NodeId /*ran_by*/, std::uint64_t /*task_id*/, std::uint64_t /*indexes*/,
                        paramesh::Nanos /*cpu*/) noexcept override {}

private:
    NodeId self_;
};

// One process's Sync with its fakes. Node 1 is the launcher.
struct Process {
    FakeTransport net;
    FakeHost host;
    Sync sync;
    explicit Process(std::uint16_t node = 1) : host(NodeId{node}), sync(net, host) {}

    // A frame from another node arrives.
    void frame(Opcode opcode, std::uint16_t from, std::uint64_t req, std::uint64_t id,
               std::uint16_t word) {
        std::array<std::byte, 16> bytes{};
        REQUIRE(paramesh::wire_encode(SyncPayload{id, word}, bytes).ok());
        FrameHeader header;
        header.opcode = opcode;
        header.src = NodeId{from};
        header.dst = host.self();
        header.req = ReqId{req};
        sync.on_frame(header, bytes);
    }
};

// Runs `body` in a child process and returns its exit status.
template <typename Body>
int exit_status_of(Body body) {
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        body();
        _exit(0);
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

TEST_CASE("handles: only the launcher makes them, each is new, and zero is never one") {
    Process launcher;
    const pm_lock_t a = launcher.sync.lock_create();
    const pm_lock_t b = launcher.sync.lock_create();
    const pm_barrier_t c = launcher.sync.barrier_create(2);
    CHECK(a.id != 0);
    CHECK(b.id != a.id);
    CHECK(c.id != 0);
    CHECK(c.id != a.id);
    CHECK(c.id != b.id);
    CHECK(launcher.sync.barrier_create(0).id == 0);  // a barrier nobody could pass

    Process worker{2};
    CHECK(worker.sync.lock_create().id == 0);
    CHECK(worker.sync.barrier_create(2).id == 0);
    CHECK(worker.sync.lock(pm_lock_t{0}, 0).error().code == paramesh::Errc::kInvalidArgument);
    CHECK(worker.sync.barrier_wait(pm_barrier_t{0}, 0).error().code ==
          paramesh::Errc::kInvalidArgument);
    CHECK(worker.net.take().empty());  // nothing was asked of the launcher
}

TEST_CASE("the launcher grants a lock first come, first served") {
    Process launcher;
    const std::uint64_t lock = launcher.sync.lock_create().id;
    const std::uint64_t other = launcher.sync.lock_create().id;

    launcher.frame(Opcode::kLockAcq, 2, 5, lock, 0);
    CHECK(launcher.net.take() == "LOCK_GRANT>2 req 5 id 1 word 0");  // free: granted at once
    CHECK(launcher.sync.holds_lock(NodeId{2}, 0));
    CHECK_FALSE(launcher.sync.holds_lock(NodeId{2}, 1));
    CHECK_FALSE(launcher.sync.holds_lock(NodeId{3}, 0));

    launcher.frame(Opcode::kLockAcq, 3, 9, lock, 0);
    launcher.frame(Opcode::kLockAcq, 2, 6, lock, 1);  // another thread of the holder's node
    launcher.frame(Opcode::kLockAcq, 4, 1, lock, 7);
    CHECK(launcher.net.take().empty());  // all three wait

    launcher.frame(Opcode::kLockAcq, 4, 2, other, 3);  // a second lock is its own queue
    CHECK(launcher.net.take() == "LOCK_GRANT>4 req 2 id 2 word 0");

    launcher.frame(Opcode::kLockRel, 2, 0, lock, 0);
    CHECK(launcher.net.take() == "LOCK_GRANT>3 req 9 id 1 word 0");
    launcher.frame(Opcode::kLockRel, 3, 0, lock, 0);
    CHECK(launcher.net.take() == "LOCK_GRANT>2 req 6 id 1 word 0");
    CHECK(launcher.sync.holds_lock(NodeId{2}, 1));
    launcher.frame(Opcode::kLockRel, 2, 0, lock, 1);
    CHECK(launcher.net.take() == "LOCK_GRANT>4 req 1 id 1 word 0");
    launcher.frame(Opcode::kLockRel, 4, 0, lock, 7);
    CHECK(launcher.net.take().empty());  // nobody waits: the lock is free again
    CHECK_FALSE(launcher.sync.holds_lock(NodeId{4}, 7));
    CHECK(launcher.sync.holds_lock(NodeId{4}, 3));  // still holds the other lock

    launcher.frame(Opcode::kLockAcq, 3, 10, lock, 0);
    CHECK(launcher.net.take() == "LOCK_GRANT>3 req 10 id 1 word 0");
    CHECK_FALSE(launcher.net.timed());  // these waits are not under the reply timer
}

TEST_CASE("a barrier releases every waiter when its count is reached, and works again") {
    Process launcher;
    const std::uint64_t barrier = launcher.sync.barrier_create(3).id;
    for (int round = 0; round < 2; round++) {
        launcher.frame(Opcode::kBarrierEnter, 2, 11, barrier, 0);
        launcher.frame(Opcode::kBarrierEnter, 3, 12, barrier, 0);
        CHECK(launcher.net.take().empty());
        launcher.frame(Opcode::kBarrierEnter, 2, 13, barrier, 1);
        CHECK(launcher.net.take() ==
              "BARRIER_RELEASE>2 req 11 id 1 word 0, "
              "BARRIER_RELEASE>3 req 12 id 1 word 0, "
              "BARRIER_RELEASE>2 req 13 id 1 word 0");
    }
    const std::uint64_t alone = launcher.sync.barrier_create(1).id;
    launcher.frame(Opcode::kBarrierEnter, 3, 14, alone, 0);
    CHECK(launcher.net.take() == "BARRIER_RELEASE>3 req 14 id 2 word 0");
}

TEST_CASE("a request that names no lock or barrier is answered BAD_HANDLE") {
    Process launcher;
    const std::uint64_t lock = launcher.sync.lock_create().id;
    const std::uint64_t barrier = launcher.sync.barrier_create(2).id;
    const std::string bad = std::to_string(kBadHandle);

    launcher.frame(Opcode::kLockAcq, 2, 5, 99, 0);
    CHECK(launcher.net.take() == "LOCK_GRANT>2 req 5 id 99 word " + bad);
    launcher.frame(Opcode::kBarrierEnter, 2, 6, 99, 0);
    CHECK(launcher.net.take() == "BARRIER_RELEASE>2 req 6 id 99 word " + bad);
    launcher.frame(Opcode::kLockAcq, 2, 7, barrier, 0);  // a barrier is not a lock
    CHECK(launcher.net.take() == "LOCK_GRANT>2 req 7 id 2 word " + bad);
    launcher.frame(Opcode::kBarrierEnter, 2, 8, lock, 0);  // nor a lock a barrier
    CHECK(launcher.net.take() == "BARRIER_RELEASE>2 req 8 id 1 word " + bad);

    CHECK(launcher.sync.lock(pm_lock_t{99}, paramesh::kMainThread).error().code ==
          paramesh::Errc::kInvalidArgument);  // and so is the launcher's own thread
}

TEST_CASE("the launcher's own threads ask the manager directly, in the same queue") {
    Process launcher;
    const pm_lock_t lock = launcher.sync.lock_create();
    const NodeId self{1};

    REQUIRE(launcher.sync.lock(lock, paramesh::kMainThread).ok());
    CHECK(launcher.sync.holds_lock(self, paramesh::kMainThread));
    CHECK(launcher.sync.lock(lock, paramesh::kMainThread).error().code ==
          paramesh::Errc::kInvalidArgument);  // not recursive
    CHECK(launcher.sync.unlock(lock, 0).error().code ==
          paramesh::Errc::kInvalidArgument);  // not its

    launcher.frame(Opcode::kLockAcq, 2, 5, lock.id, 0);  // a worker waits behind main()
    std::thread second{[&] { CHECK(launcher.sync.lock(lock, 0).ok()); }};
    CHECK(launcher.net.take().empty());  // nothing travels for the launcher's own requests

    REQUIRE(launcher.sync.unlock(lock, paramesh::kMainThread).ok());
    CHECK(launcher.net.take() == "LOCK_GRANT>2 req 5 id 1 word 0");  // the worker asked first
    CHECK(launcher.sync.holds_lock(NodeId{2}, 0));
    launcher.frame(Opcode::kLockRel, 2, 0, lock.id, 0);
    second.join();  // and now the launcher's second thread has it
    CHECK(launcher.sync.holds_lock(self, 0));
    CHECK(launcher.sync.unlock(lock, paramesh::kMainThread).error().code ==
          paramesh::Errc::kInvalidArgument);  // released already

    const pm_barrier_t barrier = launcher.sync.barrier_create(2);
    launcher.frame(Opcode::kBarrierEnter, 2, 6, barrier.id, 0);
    CHECK(launcher.sync.barrier_wait(barrier, paramesh::kMainThread).ok());  // the second of two
    CHECK(launcher.net.take().find("BARRIER_RELEASE>2 req 6") != std::string::npos);
}

TEST_CASE("a worker's thread sleeps from LOCK_ACQ until LOCK_GRANT") {
    Process worker{2};
    std::atomic<bool> locked{false};
    std::thread thread{[&] {
        CHECK(worker.sync.lock(pm_lock_t{7}, 3).ok());
        locked = true;
    }};
    CHECK(worker.net.wait() == "LOCK_ACQ>1 req " +
                                   std::to_string(worker.net.last_req().value & 0xFFFFU) +
                                   " id 7 word 3");
    CHECK_FALSE(locked);  // nothing has granted it
    worker.frame(Opcode::kLockGrant, 1, worker.net.last_req().value, 7, kOk);
    thread.join();
    CHECK(locked);

    CHECK(worker.sync.lock(pm_lock_t{7}, 3).error().code == paramesh::Errc::kInvalidArgument);
    CHECK(worker.sync.unlock(pm_lock_t{7}, 4).error().code == paramesh::Errc::kInvalidArgument);
    CHECK(worker.net.take().empty());  // both were refused here, without a message
    REQUIRE(worker.sync.unlock(pm_lock_t{7}, 3).ok());
    CHECK(worker.net.take() == "LOCK_REL>1 req 0 id 7 word 3");
    CHECK_FALSE(worker.net.timed());

    // BAD_HANDLE comes back as an error, and the thread does not hold the lock.
    std::thread refused{[&] {
        CHECK(worker.sync.lock(pm_lock_t{8}, 3).error().code == paramesh::Errc::kInvalidArgument);
    }};
    worker.net.wait();
    worker.frame(Opcode::kLockGrant, 1, worker.net.last_req().value, 8, kBadHandle);
    refused.join();
    CHECK(worker.sync.unlock(pm_lock_t{8}, 3).error().code == paramesh::Errc::kInvalidArgument);
}

TEST_CASE("a worker's thread waits at a barrier until BARRIER_RELEASE") {
    Process worker{3};
    std::atomic<bool> through{false};
    std::thread thread{[&] {
        CHECK(worker.sync.barrier_wait(pm_barrier_t{4}, 0).ok());
        through = true;
    }};
    CHECK(worker.net.wait().starts_with("BARRIER_ENTER>1 req "));
    CHECK_FALSE(through);
    worker.frame(Opcode::kBarrierRelease, 1, worker.net.last_req().value, 4, kOk);
    thread.join();
    CHECK(through);
}

TEST_CASE("wait_all sleeps until the outstanding work is done") {
    Process launcher;
    CHECK(launcher.sync.wait_all().ok());  // nothing outstanding: at once

    launcher.sync.work_added(3);
    std::atomic<bool> returned{false};
    std::thread waiter{[&] {
        CHECK(launcher.sync.wait_all().ok());
        returned = true;
    }};
    launcher.sync.work_done(1);
    CHECK_FALSE(returned);  // two are still outstanding
    launcher.sync.work_done(2);
    waiter.join();
    CHECK(returned);
    CHECK(launcher.sync.wait_all().ok());

    Process worker{2};
    CHECK(worker.sync.wait_all().error().code == paramesh::Errc::kState);
}

TEST_CASE("what cannot happen between correct processes ends the job") {
    // A release by a thread that does not hold the lock.
    CHECK(exit_status_of([] {
              Process launcher;
              const std::uint64_t lock = launcher.sync.lock_create().id;
              launcher.frame(Opcode::kLockAcq, 2, 5, lock, 0);
              launcher.frame(Opcode::kLockRel, 3, 0, lock, 0);
          }) == 42);
    // A release of a lock that is free.
    CHECK(exit_status_of([] {
              Process launcher;
              launcher.frame(Opcode::kLockRel, 2, 0, launcher.sync.lock_create().id, 0);
          }) == 42);
    // A grant nobody asked for.
    CHECK(exit_status_of([] {
              Process worker{2};
              worker.frame(Opcode::kLockGrant, 1, 5, 1, kOk);
          }) == 42);
    // A lock request sent to a worker.
    CHECK(exit_status_of([] {
              Process worker{2};
              worker.frame(Opcode::kLockAcq, 3, 5, 1, 0);
          }) == 42);
    // A payload of the wrong size.
    CHECK(exit_status_of([] {
              Process launcher;
              FrameHeader header;
              header.opcode = Opcode::kLockAcq;
              header.src = NodeId{2};
              const std::array<std::byte, 8> eight{};
              launcher.sync.on_frame(header, eight);
          }) == 42);
    // And the same calls between correct processes end normally.
    CHECK(exit_status_of([] {
              Process launcher;
              const std::uint64_t lock = launcher.sync.lock_create().id;
              launcher.frame(Opcode::kLockAcq, 2, 5, lock, 0);
              launcher.frame(Opcode::kLockRel, 2, 0, lock, 0);
          }) == 0);
}
