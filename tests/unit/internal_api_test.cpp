// The internal interfaces of docs/INTERNAL_API.md: every header compiles, Result<T> and the
// ID helpers behave, the constants agree with paramesh.h, and each abstract interface can be
// implemented by a fake that uses nothing but the interface headers.

#include "coh/home_machine.h"
#include "coh/node_machine.h"
#include "mem/memory_engine.h"
#include "net/transport.h"
#include "platform/ids.h"
#include "platform/result.h"
#include "rt/runtime.h"
#include "store/home_store.h"
#include "store/local_cache.h"
#include "store/placement.h"
#include "wire/frame.h"

#include <doctest/doctest.h>

#include <paramesh.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using paramesh::Errc;
using paramesh::Error;
using paramesh::PageId;
using paramesh::Result;

using PageArray = std::array<std::byte, paramesh::kPageSize>;

// The design's numbers are the same in the public header and inside the library.
static_assert(paramesh::kPageSize == PM_PAGE_SIZE);
static_assert(paramesh::kRegionMaxBytes == PM_REGION_MAX_BYTES);
static_assert(paramesh::kSegmentSize == 2ULL * 1024 * 1024);
static_assert(paramesh::kFrameHeaderSize == 36);

// IDs are distinct types of the sizes the wire gives them.
static_assert(sizeof(paramesh::NodeId) == 2 && sizeof(paramesh::JobId) == 4);
static_assert(sizeof(paramesh::ReqId) == 8 && sizeof(paramesh::Epoch) == 4);
static_assert(!std::is_convertible_v<paramesh::NodeId, PageId>);
static_assert(!std::is_convertible_v<std::uint64_t, PageId>);

// One state byte per page, as the HLD says.
static_assert(sizeof(paramesh::PageState) == 1);
// The longest row of the node table has four actions.
static_assert(paramesh::kMaxNodeActions == 4);

// ---- fakes: each implements one interface with standard containers only ----------------

class FakeMemory final : public paramesh::MemoryEngine {
public:
    Result<void> install(PageId page, paramesh::PageView data,
                         paramesh::PageProtection protection) override {
        if (!pages_.contains(page.value)) {
            Mapped mapped;
            std::copy(data.begin(), data.end(), mapped.bytes.begin());
            mapped.writable = protection == paramesh::PageProtection::kWritable;
            pages_.emplace(page.value, mapped);
        }
        return {};
    }
    Result<void> zap(PageId page) override {
        pages_.erase(page.value);
        return {};
    }
    Result<void> write_protect(PageId page, bool on) override {
        const auto it = pages_.find(page.value);
        if (it == pages_.end()) {
            return Error{Errc::kState, 0, "write_protect on an unmapped page"};
        }
        it->second.writable = !on;
        return {};
    }
    Result<void> wake(PageId /*page*/) override {
        wakes_++;
        return {};
    }
    Result<void> read(PageId page, paramesh::PageBuffer out) override {
        const auto it = pages_.find(page.value);
        if (it == pages_.end()) {
            return Error{Errc::kState, 0, "read of an unmapped page"};
        }
        std::copy(it->second.bytes.begin(), it->second.bytes.end(), out.begin());
        return {};
    }
    [[nodiscard]] bool mapped(PageId page) const { return pages_.contains(page.value); }
    [[nodiscard]] int wakes() const { return wakes_; }

private:
    struct Mapped {
        PageArray bytes{};
        bool writable = false;
    };
    std::map<std::uint64_t, Mapped> pages_;
    int wakes_ = 0;
};

class FakeStore final : public paramesh::HomeStore {
public:
    Result<paramesh::StoreLookup> get(PageId page, paramesh::PageBuffer out) override {
        const auto it = pages_.find(page.value);
        if (it == pages_.end()) {
            return paramesh::StoreLookup::kAbsent;
        }
        std::copy(it->second.begin(), it->second.end(), out.begin());
        return paramesh::StoreLookup::kCopied;
    }
    Result<void> put(PageId page, paramesh::PageView data) override {
        std::copy(data.begin(), data.end(), pages_[page.value].begin());
        return {};
    }
    Result<void> load(PageId /*page*/) override {
        return Error{Errc::kUnsupported, 0, "the fake store never spills"};
    }
    void drop(PageId page) override { pages_.erase(page.value); }
    [[nodiscard]] std::uint64_t ram_bytes() const noexcept override {
        return pages_.size() * paramesh::kPageSize;
    }

private:
    std::map<std::uint64_t, PageArray> pages_;
};

class FakeCache final : public paramesh::LocalCache {
public:
    explicit FakeCache(std::size_t budget_pages) : budget_pages_(budget_pages) {}
    void note_installed(PageId page) override { order_.push_back(page); }
    void note_discarded(PageId page) override { std::erase(order_, page); }
    [[nodiscard]] bool over_budget() const noexcept override {
        return order_.size() > budget_pages_;
    }
    std::optional<PageId> next_victim() override {
        if (order_.empty()) {
            return std::nullopt;
        }
        return order_.front();
    }

private:
    std::size_t budget_pages_;
    std::deque<PageId> order_;
};

struct SentFrame {
    paramesh::NodeId to;
    paramesh::FrameHeader header;
    std::vector<std::byte> payload;
    paramesh::ReplyTimer timer;
};

class FakeTransport final : public paramesh::Transport {
public:
    Result<std::uint16_t> listen(paramesh::Endpoint local) override { return local.port; }
    Result<void> add_peer(paramesh::NodeId peer, paramesh::Endpoint /*remote*/) override {
        peers_.push_back(peer);
        return {};
    }
    void remove_peer(paramesh::NodeId peer) override { std::erase(peers_, peer); }
    Result<void> send(paramesh::NodeId to, const paramesh::FrameHeader& header,
                      std::span<const std::byte> payload, paramesh::ReplyTimer timer) override {
        if (std::find(peers_.begin(), peers_.end(), to) == peers_.end()) {
            return Error{Errc::kNotFound, 0, "send to an unknown peer"};
        }
        sent_.push_back({to, header, {payload.begin(), payload.end()}, timer});
        return {};
    }
    paramesh::TimerId start_timer(paramesh::Nanos /*delay*/) override {
        return paramesh::TimerId{++timers_};
    }
    void cancel_timer(paramesh::TimerId /*timer*/) override {}
    [[nodiscard]] paramesh::Nanos now() const noexcept override { return paramesh::Nanos{0}; }
    Result<void> run() override { return {}; }
    void stop() noexcept override {}

    [[nodiscard]] const std::vector<SentFrame>& sent() const { return sent_; }

private:
    std::vector<paramesh::NodeId> peers_;
    std::vector<SentFrame> sent_;
    std::uint64_t timers_ = 0;
};

// The callback interfaces and the runtime's two sides are implementable too. These are only
// instantiated, to show that no pure virtual function is out of reach.
class NullSinks final : public paramesh::FaultSink,
                        public paramesh::StoreEvents,
                        public paramesh::NetHandler {
public:
    void on_fault(const paramesh::FaultEvent& /*event*/) noexcept override {}
    void on_loaded(PageId /*page*/) noexcept override {}
    void on_spilled(PageId /*page*/) noexcept override {}
    void on_peer_joined(const paramesh::JoinInfo& /*peer*/) noexcept override {}
    void on_frame(const paramesh::FrameHeader& /*header*/,
                  std::span<const std::byte> /*payload*/) noexcept override {}
    void on_peer_lost(paramesh::NodeId /*peer*/, Errc /*why*/) noexcept override {}
    void on_reply_timeout(paramesh::NodeId /*peer*/, paramesh::ReqId /*req*/) noexcept override {}
    void on_timer(paramesh::TimerId /*timer*/, paramesh::Nanos /*now*/) noexcept override {}
};

static_assert(std::is_abstract_v<paramesh::RuntimeHost>);
static_assert(std::has_virtual_destructor_v<paramesh::Transport>);
static_assert(std::has_virtual_destructor_v<paramesh::MemoryEngine>);
static_assert(std::has_virtual_destructor_v<paramesh::HomeStore>);

Result<int> half(int value) {
    if (value % 2 != 0) {
        return Error{Errc::kInvalidArgument, 0, "odd"};
    }
    return value / 2;
}

PageArray page_of_bytes(std::byte fill) {
    PageArray page{};
    page.fill(fill);
    return page;
}

}  // namespace

TEST_CASE("Result carries a value or an error, never both") {
    const Result<int> good = half(10);
    REQUIRE(good.ok());
    CHECK(good.value() == 5);

    const Result<int> bad = half(7);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == Errc::kInvalidArgument);
    CHECK(std::string_view{bad.error().what} == "odd");
    CHECK_FALSE(static_cast<bool>(bad));
}

TEST_CASE("Result holds a move-only value and gives it up when moved from") {
    Result<std::unique_ptr<int>> made = std::make_unique<int>(42);
    REQUIRE(made.ok());
    const std::unique_ptr<int> taken = std::move(made).value();
    REQUIRE(taken != nullptr);
    CHECK(*taken == 42);
}

TEST_CASE("Result<void> is ok by default and carries an error otherwise") {
    const Result<void> fine;
    CHECK(fine.ok());
    const Result<void> failed = Error{Errc::kIo, 5, "pwrite"};
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().code == Errc::kIo);
    CHECK(failed.error().os_error == 5);
}

TEST_CASE("page, segment and address conversions agree") {
    CHECK(paramesh::address_of(PageId{0}) == paramesh::kRegionBase);
    CHECK(paramesh::address_of(PageId{3}) == paramesh::kRegionBase + 3 * paramesh::kPageSize);
    CHECK(paramesh::page_of(paramesh::kRegionBase + 4095) == PageId{0});
    CHECK(paramesh::page_of(paramesh::kRegionBase + 4096) == PageId{1});
    CHECK(paramesh::segment_of(PageId{511}) == paramesh::SegmentId{0});
    CHECK(paramesh::segment_of(PageId{512}) == paramesh::SegmentId{1});
    // The last page of a full region is in the last of its 2,048 segments.
    const PageId last{paramesh::kRegionMaxBytes / paramesh::kPageSize - 1};
    CHECK(paramesh::segment_of(last) == paramesh::SegmentId{2047});
}

TEST_CASE("a fake memory engine follows the contract of the four helpers") {
    FakeMemory memory;
    const PageArray ones = page_of_bytes(std::byte{1});
    const PageArray twos = page_of_bytes(std::byte{2});
    PageArray out{};

    CHECK_FALSE(memory.read(PageId{9}, out).ok());
    REQUIRE(memory.install(PageId{9}, ones, paramesh::PageProtection::kReadOnly).ok());
    // Installing over a mapped page succeeds and leaves the page as it was.
    REQUIRE(memory.install(PageId{9}, twos, paramesh::PageProtection::kWritable).ok());
    REQUIRE(memory.read(PageId{9}, out).ok());
    CHECK(out == ones);

    CHECK(memory.write_protect(PageId{9}, false).ok());
    CHECK(memory.wake(PageId{9}).ok());
    CHECK(memory.wakes() == 1);
    REQUIRE(memory.zap(PageId{9}).ok());
    CHECK_FALSE(memory.mapped(PageId{9}));
    CHECK(memory.write_protect(PageId{9}, true).error().code == Errc::kState);
}

TEST_CASE("a fake home store gets what was put and reports what it never had") {
    FakeStore store;
    const PageArray sevens = page_of_bytes(std::byte{7});
    PageArray out{};

    REQUIRE(store.get(PageId{4}, out).ok());
    CHECK(store.get(PageId{4}, out).value() == paramesh::StoreLookup::kAbsent);
    REQUIRE(store.put(PageId{4}, sevens).ok());
    CHECK(store.get(PageId{4}, out).value() == paramesh::StoreLookup::kCopied);
    CHECK(out == sevens);
    CHECK(store.ram_bytes() == paramesh::kPageSize);
    store.drop(PageId{4});
    CHECK(store.get(PageId{4}, out).value() == paramesh::StoreLookup::kAbsent);
    CHECK(store.load(PageId{4}).error().code == Errc::kUnsupported);
}

TEST_CASE("a fake local cache offers the page installed first") {
    FakeCache cache{2};
    cache.note_installed(PageId{10});
    cache.note_installed(PageId{11});
    CHECK_FALSE(cache.over_budget());
    cache.note_installed(PageId{12});
    CHECK(cache.over_budget());
    CHECK(cache.next_victim() == std::optional<PageId>{PageId{10}});
    cache.note_discarded(PageId{10});
    CHECK_FALSE(cache.over_budget());
}

TEST_CASE("a fake transport records frames in order and refuses unknown peers") {
    FakeTransport transport;
    const paramesh::NodeId home{7};
    paramesh::FrameHeader header;
    header.opcode = paramesh::Opcode::kReadReq;
    header.dst = home;
    header.req = paramesh::ReqId{1};
    const std::array<std::byte, 8> page_id{};

    CHECK(transport.send(home, header, page_id, paramesh::ReplyTimer::kTimed).error().code ==
          Errc::kNotFound);
    REQUIRE(transport.add_peer(home, paramesh::Endpoint{0x7F000001, 47100}).ok());
    REQUIRE(transport.send(home, header, page_id, paramesh::ReplyTimer::kTimed).ok());
    header.opcode = paramesh::Opcode::kWriteReq;
    header.req = paramesh::ReqId{2};
    REQUIRE(transport.send(home, header, page_id, paramesh::ReplyTimer::kTimed).ok());

    REQUIRE(transport.sent().size() == 2);
    CHECK(transport.sent()[0].header.opcode == paramesh::Opcode::kReadReq);
    CHECK(transport.sent()[1].header.req == paramesh::ReqId{2});
    CHECK(transport.sent()[0].payload.size() == 8);
    CHECK(transport.start_timer(paramesh::Nanos{5}) != transport.start_timer(paramesh::Nanos{5}));
}

TEST_CASE("the callback interfaces can all be implemented") {
    NullSinks sinks;
    sinks.on_fault(paramesh::FaultEvent{PageId{1}, paramesh::FaultKind::kWrite});
    sinks.on_loaded(PageId{1});
    sinks.on_timer(paramesh::TimerId{1}, paramesh::Nanos{0});
    // A step result and a home action are plain values a caller can build and read.
    paramesh::NodeStep step;
    step.state = paramesh::PageState::kReadPending;
    step.actions.at(0) =
        paramesh::NodeAction{paramesh::NodeActionKind::kSend, paramesh::Opcode::kReadReq,
                             paramesh::ReqId{1}, false, false};
    step.action_count = 1;
    CHECK(step.actions.at(0).opcode == paramesh::Opcode::kReadReq);
    const paramesh::HomeEntryView fresh;
    CHECK(fresh.state == paramesh::HomeState::kUncached);
    CHECK(fresh.where == paramesh::HomeWhere::kZero);
}
