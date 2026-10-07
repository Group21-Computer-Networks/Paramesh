// The M0 demo: a program that reads and writes memory it never allocated.
//
// It maps the 4 GiB shared region and treats the start of it as an array of a million 64-bit
// numbers. No page of that array exists until the program touches it. Each touch is a page
// fault; a "home" object in this same process plays the part a remote node will play from M1
// on and supplies the page. The program then has its pages taken away and gets them back,
// with its own writes intact.

#include "mem/memory_engine.h"
#include "platform/ids.h"
#include "platform/result.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

using paramesh::PageId;
using PageArray = std::array<std::byte, paramesh::kPageSize>;

constexpr std::uint64_t kElements = 1'000'000;
constexpr std::uint64_t kPerPage = paramesh::kPageSize / sizeof(std::uint64_t);
constexpr std::uint64_t kPages = (kElements + kPerPage - 1) / kPerPage;
constexpr int kSkipped = 77;  // exit status: this machine cannot run the demo

// Plays the home of every page: it keeps the pages the program does not hold and hands one
// over when the program faults on it.
class LocalHome final : public paramesh::FaultSink {
public:
    void attach(paramesh::MemoryEngine& engine) noexcept { engine_ = &engine; }

    // Runs on the fault-handler thread. It never waits: the page operations are single system
    // calls, and the lock is only ever held around such calls.
    void on_fault(const paramesh::FaultEvent& event) noexcept override {
        const std::lock_guard<std::mutex> lock{mutex_};
        Holding& holding = holding_[event.page.value];
        if (event.kind == paramesh::FaultKind::kRead) {
            read_faults_++;
            ok_ = ok_ &&
                  engine_
                      ->install(event.page, stored(event.page), paramesh::PageProtection::kReadOnly)
                      .ok();
            holding = Holding::kReadOnly;
        } else if (holding == Holding::kReadOnly) {
            // The program has the page and now wants to write it: the upgrade of M2.
            write_faults_++;
            ok_ = ok_ && engine_->write_protect(event.page, false).ok();
            holding = Holding::kWritable;
        } else {
            write_faults_++;
            ok_ = ok_ &&
                  engine_
                      ->install(event.page, stored(event.page), paramesh::PageProtection::kWritable)
                      .ok();
            holding = Holding::kWritable;
        }
    }

    // Takes a page back from the program: stop its writes, copy the page, unmap it.
    bool take_back(PageId page) {
        const std::lock_guard<std::mutex> lock{mutex_};
        Holding& holding = holding_[page.value];
        if (holding == Holding::kNothing) {
            return true;
        }
        PageArray copy{};
        const bool done = engine_->write_protect(page, true).ok() &&
                          engine_->read(page, copy).ok() && engine_->zap(page).ok();
        if (done) {
            pages_[page.value] = copy;
            holding = Holding::kNothing;
        }
        return done;
    }

    [[nodiscard]] std::uint64_t read_faults() const noexcept { return read_faults_; }
    [[nodiscard]] std::uint64_t write_faults() const noexcept { return write_faults_; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void reset_counts() noexcept {
        read_faults_ = 0;
        write_faults_ = 0;
    }

private:
    enum class Holding : std::uint8_t { kNothing, kReadOnly, kWritable };

    // The home's copy of a page. A page nobody has written yet holds the numbers 0, 1, 2, ...
    // in order, so element i of the array starts out equal to i.
    const PageArray& stored(PageId page) {
        const auto found = pages_.find(page.value);
        if (found != pages_.end()) {
            return found->second;
        }
        PageArray fresh{};
        for (std::uint64_t slot = 0; slot < kPerPage; slot++) {
            const std::uint64_t value = page.value * kPerPage + slot;
            std::memcpy(&fresh.at(slot * sizeof value), &value, sizeof value);
        }
        return pages_.emplace(page.value, fresh).first->second;
    }

    std::mutex mutex_;
    paramesh::MemoryEngine* engine_ = nullptr;
    std::unordered_map<std::uint64_t, PageArray> pages_;
    std::unordered_map<std::uint64_t, Holding> holding_;
    std::atomic<std::uint64_t> read_faults_{0};
    std::atomic<std::uint64_t> write_faults_{0};
    std::atomic<bool> ok_{true};
};

std::uint64_t sum(const std::uint64_t* numbers) {
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < kElements; i++) {
        total += numbers[i];
    }
    return total;
}

bool report(const char* step, const LocalHome& home, std::uint64_t result, std::uint64_t expected) {
    const bool right = result == expected;
    std::printf("  %-34s %6" PRIu64 " read, %6" PRIu64 " write   %14" PRIu64 "   %s\n", step,
                home.read_faults(), home.write_faults(), result, right ? "ok" : "WRONG");
    return right;
}

}  // namespace

int main() {
    LocalHome home;
    auto opened = paramesh::mem_open(paramesh::RegionConfig{paramesh::kRegionMaxBytes}, home);
    if (!opened.ok()) {
        const paramesh::Error& error = opened.error();
        std::printf("m0_demo: cannot %s (errno %d)\n", error.what, error.os_error);
        return error.code == paramesh::Errc::kUnsupported ? kSkipped : 1;
    }
    const std::unique_ptr<paramesh::MemoryEngine> engine = std::move(opened).value();
    home.attach(*engine);

    // The array is simply "the region": nothing was allocated for it, here or anywhere.
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the region lives at a fixed address
    auto* const numbers = reinterpret_cast<std::uint64_t*>(paramesh::kRegionBase);
    const std::uint64_t first_sum = kElements * (kElements - 1) / 2;

    std::printf("ParaMesh M0 demo: memory this program never allocated\n");
    std::printf("  region: 4 GiB at %#" PRIx64 "; array: %" PRIu64 " numbers on %" PRIu64
                " pages\n\n",
                paramesh::kRegionBase, kElements, kPages);
    std::printf("  %-34s %-27s %14s\n", "step", "page faults", "sum");

    bool right = report("1. read every number", home, sum(numbers), first_sum);

    home.reset_counts();
    for (std::uint64_t i = 0; i < kElements; i++) {
        numbers[i] *= 2;
    }
    right = report("2. double every number", home, sum(numbers), 2 * first_sum) && right;

    home.reset_counts();
    right = report("3. read them again", home, sum(numbers), 2 * first_sum) && right;

    bool taken = true;
    for (std::uint64_t page = 0; page < kPages; page++) {
        taken = home.take_back(PageId{page}) && taken;
    }
    std::printf("  %-34s %s\n", "4. take every page away", taken ? "done" : "FAILED");

    home.reset_counts();
    right = report("5. read them once more", home, sum(numbers), 2 * first_sum) && right;

    const bool all = right && taken && home.ok();
    std::printf("\n%s\n",
                all ? "The program read and wrote memory it never allocated; every sum is right."
                    : "FAILED: see the lines marked above.");
    return all ? 0 : 1;
}
