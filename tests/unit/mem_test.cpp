// src/mem/: the userfaultfd memory engine against the contract in docs/INTERNAL_API.md,
// section 5.
//
// Every test but the first two maps the real region, so they need user-mode userfaultfd and
// cannot run under ThreadSanitizer, which refuses to map the region's address (M0-2). Where
// they cannot run they are skipped, and the second test says why.

#include "mem/memory_engine.h"
#include "platform/ids.h"
#include "platform/result.h"

#include <doctest/doctest.h>

#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using paramesh::Errc;
using paramesh::FaultEvent;
using paramesh::FaultKind;
using paramesh::MemoryEngine;
using paramesh::PageId;
using paramesh::PageProtection;

using PageArray = std::array<std::byte, paramesh::kPageSize>;
using EnginePtr = std::unique_ptr<MemoryEngine>;

constexpr std::chrono::seconds kWait{5};
constexpr int kAccessFailed = 44;  // exit status of a child whose access to the region failed

bool is_tsan_build() {
    return std::string_view{PARAMESH_SANITIZER_NAME} == "thread";
}

bool userfaultfd_available() {
    static const bool available = [] {
        const long fd = ::syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
        if (fd < 0) {
            return false;
        }
        ::close(static_cast<int>(fd));
        return true;
    }();
    return available;
}

bool region_tests_can_run() {
    return !is_tsan_build() && userfaultfd_available();
}

// Records faults so the test thread can wait for them. on_fault only queues, as the
// interface requires of a sink.
class RecordingSink final : public paramesh::FaultSink {
public:
    void on_fault(const FaultEvent& event) noexcept override {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            events_.push_back(event);
        }
        changed_.notify_all();
    }

    // The next fault not yet handed out, or nothing if none arrives in time.
    std::optional<FaultEvent> next() {
        std::unique_lock<std::mutex> lock{mutex_};
        if (!changed_.wait_for(lock, kWait, [this] { return taken_ < events_.size(); })) {
            return std::nullopt;
        }
        return events_[taken_++];
    }

    std::size_t count() {
        const std::lock_guard<std::mutex> lock{mutex_};
        return events_.size();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<FaultEvent> events_;
    std::size_t taken_ = 0;
};

EnginePtr open_engine(RecordingSink& sink, std::uint64_t bytes = paramesh::kSegmentSize) {
    auto opened = paramesh::mem_open(paramesh::RegionConfig{bytes}, sink);
    REQUIRE_MESSAGE(opened.ok(), opened.error().what);
    return std::move(opened).value();
}

PageArray filled(std::uint8_t value) {
    PageArray page{};
    page.fill(std::byte{value});
    return page;
}

// The next fault, which must arrive in time.
FaultEvent next_fault(RecordingSink& sink) {
    const std::optional<FaultEvent> fault = sink.next();
    REQUIRE_MESSAGE(fault.has_value(), "no fault was reported in time");
    return fault.value_or(FaultEvent{});
}

// The first byte of a page, read and written the way a task would.
volatile std::uint8_t* byte_of(PageId page) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the region lives at a fixed address
    return reinterpret_cast<volatile std::uint8_t*>(paramesh::address_of(page));
}

}  // namespace

TEST_CASE("mem_open refuses a size that is not a whole number of segments, at most 4 GiB") {
    RecordingSink sink;
    for (const std::uint64_t bytes :
         {std::uint64_t{0}, std::uint64_t{4096}, paramesh::kSegmentSize + 4096,
          paramesh::kRegionMaxBytes + paramesh::kSegmentSize}) {
        CAPTURE(bytes);
        const auto opened = paramesh::mem_open(paramesh::RegionConfig{bytes}, sink);
        REQUIRE_FALSE(opened.ok());
        CHECK(opened.error().code == Errc::kInvalidArgument);
    }
}

TEST_CASE("this build says whether it can run the tests that map the region") {
    if (region_tests_can_run()) {
        MESSAGE("region tests: running");
    } else if (is_tsan_build()) {
        MESSAGE("region tests: SKIPPED, ThreadSanitizer cannot map the region's address");
    } else {
        MESSAGE("region tests: SKIPPED, user-mode userfaultfd is not available here");
    }
}

TEST_CASE("the region maps at its fixed address, and only once at a time" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    EnginePtr first = open_engine(sink);

    const auto second = paramesh::mem_open(paramesh::RegionConfig{paramesh::kSegmentSize}, sink);
    REQUIRE_FALSE(second.ok());
    CHECK(second.error().code == Errc::kState);

    first.reset();
    const EnginePtr again = open_engine(sink);
    CHECK(again != nullptr);
}

TEST_CASE("a read of a missing page reports a read fault, and install resumes the thread" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{3};
    std::atomic<int> seen{-1};

    std::thread reader{[&] { seen = *byte_of(page); }};
    const FaultEvent fault = next_fault(sink);
    CHECK(fault.page == page);
    CHECK(fault.kind == FaultKind::kRead);
    CHECK(seen == -1);  // still parked

    REQUIRE(engine->install(page, filled(0x11), PageProtection::kReadOnly).ok());
    reader.join();
    CHECK(seen == 0x11);
    CHECK(sink.count() == 1);
}

TEST_CASE("a write to a missing page reports a write fault" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{5};

    std::thread writer{[&] { *byte_of(page) = 0x77; }};
    const FaultEvent fault = next_fault(sink);
    CHECK(fault.page == page);
    CHECK(fault.kind == FaultKind::kWrite);

    REQUIRE(engine->install(page, filled(0x22), PageProtection::kWritable).ok());
    writer.join();
    CHECK(*byte_of(page) == 0x77);
    CHECK(*(byte_of(page) + 1) == 0x22);  // the rest of the installed page is intact
    CHECK(sink.count() == 1);
}

TEST_CASE("a write to a read-only page reports a write fault; allowing writes resumes it" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{7};
    REQUIRE(engine->install(page, filled(0x33), PageProtection::kReadOnly).ok());
    CHECK(*byte_of(page) == 0x33);  // reading a read-only page does not fault
    CHECK(sink.count() == 0);

    std::atomic<bool> wrote{false};
    std::thread writer{[&] {
        *byte_of(page) = 0x44;
        wrote = true;
    }};
    const FaultEvent fault = next_fault(sink);
    CHECK(fault.page == page);
    CHECK(fault.kind == FaultKind::kWrite);
    CHECK_FALSE(wrote);

    REQUIRE(engine->write_protect(page, false).ok());
    writer.join();
    CHECK(wrote);
    CHECK(*byte_of(page) == 0x44);
}

TEST_CASE("write_protect on makes the next write to a writable page fault" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{9};
    REQUIRE(engine->install(page, filled(0x01), PageProtection::kWritable).ok());
    *byte_of(page) = 0x02;  // writable: no fault
    CHECK(sink.count() == 0);

    REQUIRE(engine->write_protect(page, true).ok());
    std::thread writer{[&] { *byte_of(page) = 0x03; }};
    const FaultEvent fault = next_fault(sink);
    CHECK(fault.kind == FaultKind::kWrite);
    CHECK(*byte_of(page) == 0x02);  // the write has not happened yet

    REQUIRE(engine->write_protect(page, false).ok());
    writer.join();
    CHECK(*byte_of(page) == 0x03);
}

TEST_CASE("installing over a mapped page succeeds and leaves the page as it was (EEXIST)" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{11};
    REQUIRE(engine->install(page, filled(0x55), PageProtection::kReadOnly).ok());

    const auto again = engine->install(page, filled(0x66), PageProtection::kWritable);
    CHECK(again.ok());
    CHECK(*byte_of(page) == 0x55);

    // It stays read-only too: the second install did not change the protection.
    std::thread writer{[&] { *byte_of(page) = 0x67; }};
    const FaultEvent fault = next_fault(sink);
    CHECK(fault.kind == FaultKind::kWrite);
    REQUIRE(engine->write_protect(page, false).ok());
    writer.join();
}

TEST_CASE("installing over a mapped page wakes a thread parked on it (EEXIST)" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{13};
    REQUIRE(engine->install(page, filled(0x10), PageProtection::kReadOnly).ok());

    std::thread writer{[&] { *byte_of(page) = 0x20; }};
    static_cast<void>(next_fault(sink));  // parked on the write-protect fault

    // The install changes nothing, but it wakes the thread, which writes again and faults again.
    REQUIRE(engine->install(page, filled(0x30), PageProtection::kWritable).ok());
    const FaultEvent second = next_fault(sink);
    CHECK(second.page == page);
    CHECK(second.kind == FaultKind::kWrite);

    REQUIRE(engine->write_protect(page, false).ok());
    writer.join();
    CHECK(*byte_of(page) == 0x20);
}

TEST_CASE("zap unmaps the page: it can no longer be read out, and the next access faults" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{15};
    REQUIRE(engine->install(page, filled(0x41), PageProtection::kWritable).ok());
    PageArray out{};
    REQUIRE(engine->read(page, out).ok());

    REQUIRE(engine->zap(page).ok());
    CHECK(engine->read(page, out).error().code == Errc::kState);

    std::atomic<int> seen{-1};
    std::thread reader{[&] { seen = *byte_of(page); }};
    const FaultEvent fault = next_fault(sink);
    CHECK(fault.page == page);
    CHECK(fault.kind == FaultKind::kRead);

    // What comes back is what is installed now, not the old bytes and not zeros.
    REQUIRE(engine->install(page, filled(0x42), PageProtection::kReadOnly).ok());
    reader.join();
    CHECK(seen == 0x42);
}

TEST_CASE("wake resumes a parked thread, which faults again if the page is still missing" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{17};
    std::atomic<int> seen{-1};

    std::thread reader{[&] { seen = *byte_of(page); }};
    static_cast<void>(next_fault(sink));

    REQUIRE(engine->wake(page).ok());
    const FaultEvent second = next_fault(sink);
    CHECK(second.page == page);
    CHECK(seen == -1);  // woken, but with no page it could not proceed

    REQUIRE(engine->install(page, filled(0x51), PageProtection::kReadOnly).ok());
    reader.join();
    CHECK(seen == 0x51);
    CHECK(sink.count() == 2);
}

TEST_CASE("read copies a mapped page out without raising a fault" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    PageArray out{};

    // A page never installed is not mapped, and asking for it must not park this thread.
    CHECK(engine->read(PageId{19}, out).error().code == Errc::kState);

    REQUIRE(engine->install(PageId{19}, filled(0x61), PageProtection::kReadOnly).ok());
    REQUIRE(engine->read(PageId{19}, out).ok());
    CHECK(out == filled(0x61));
    CHECK(sink.count() == 0);
}

TEST_CASE("one install wakes every thread parked on the page" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId page{21};
    std::atomic<int> sum{0};

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int i = 0; i < 4; i++) {
        readers.emplace_back([&] { sum += *byte_of(page); });
    }
    static_cast<void>(next_fault(sink));

    REQUIRE(engine->install(page, filled(0x02), PageProtection::kReadOnly).ok());
    for (std::thread& reader : readers) {
        reader.join();
    }
    CHECK(sum == 4 * 0x02);
}

TEST_CASE("a page outside the region is refused by every operation" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink);
    const PageId beyond{paramesh::kPagesPerSegment};  // the region is one segment
    PageArray out{};

    CHECK(engine->install(beyond, filled(1), PageProtection::kReadOnly).error().code ==
          Errc::kInvalidArgument);
    CHECK(engine->zap(beyond).error().code == Errc::kInvalidArgument);
    CHECK(engine->write_protect(beyond, true).error().code == Errc::kInvalidArgument);
    CHECK(engine->wake(beyond).error().code == Errc::kInvalidArgument);
    CHECK(engine->read(beyond, out).error().code == Errc::kInvalidArgument);
}

TEST_CASE("faults are served on the first and last page of a full 4 GiB region" *
          doctest::skip(!region_tests_can_run())) {
    RecordingSink sink;
    const EnginePtr engine = open_engine(sink, paramesh::kRegionMaxBytes);
    const PageId last{paramesh::kRegionMaxBytes / paramesh::kPageSize - 1};

    for (const PageId page : {PageId{0}, last}) {
        CAPTURE(page.value);
        std::atomic<int> seen{-1};
        std::thread reader{[&] { seen = *byte_of(page); }};
        const FaultEvent fault = next_fault(sink);
        CHECK(fault.page == page);
        REQUIRE(engine->install(page, filled(0x7A), PageProtection::kReadOnly).ok());
        reader.join();
        CHECK(seen == 0x7A);
    }
}

TEST_CASE("a thread still parked when the engine is destroyed is never resumed on zeros" *
          doctest::skip(!region_tests_can_run())) {
    // The right outcome is that the parked thread's access fails. That is a SIGSEGV, so it
    // runs in a child, which catches the signal and reports it through its exit status. No
    // process actually crashes, so the system's crash reporter is never involved.
    constexpr int kReadReturned = 42;
    constexpr int kNeverFaulted = 43;
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        static_cast<void>(std::signal(SIGSEGV, [](int) { ::_exit(kAccessFailed); }));

        RecordingSink sink;
        auto opened = paramesh::mem_open(paramesh::RegionConfig{paramesh::kSegmentSize}, sink);
        if (!opened.ok()) {
            ::_exit(kNeverFaulted);
        }
        EnginePtr engine = std::move(opened).value();
        std::thread reader{[] {
            const int value = *byte_of(PageId{1});
            static_cast<void>(value);
            ::_exit(kReadReturned);  // the read came back with something: zeros, or anything
        }};
        if (!sink.next().has_value()) {
            ::_exit(kNeverFaulted);
        }
        engine.reset();  // the reader is parked on page 1, and no page was ever installed
        reader.join();   // not reached: the reader's access fails first
        ::_exit(kReadReturned);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == kAccessFailed);
}
