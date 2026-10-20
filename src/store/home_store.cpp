// HomeStore: pages in RAM up to the cap and, past it, in a spill file (M4-3). CLOCK picks a
// cold page; a thread of the store's own writes it out and reads pages back, so the network
// thread never waits for the disk. A page being written still counts as in RAM until the
// write is done: put() answers Errc::kState meanwhile and the caller puts again after
// on_spilled, so the store never holds more than its cap.

#include "store/home_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace paramesh {
namespace {

using Page = std::array<std::byte, kPageSize>;

class SpillingStore final : public HomeStore {
public:
    // fd: the spill file, or -1 for a store that never spills.
    SpillingStore(const StoreConfig& config, StoreEvents& events, int fd)
        : cap_pages_(pages_of(config.ram_cap_bytes)),
          budget_slots_(fd < 0 ? 0 : config.spill_budget_bytes / kPageSize),
          events_(events),
          fd_(fd) {
        if (fd_ >= 0) {
            io_ = std::thread{[this] { run(); }};
        }
    }
    SpillingStore(const SpillingStore&) = delete;
    SpillingStore& operator=(const SpillingStore&) = delete;
    SpillingStore(SpillingStore&&) = delete;
    SpillingStore& operator=(SpillingStore&&) = delete;

    ~SpillingStore() override {
        {
            const std::lock_guard<std::mutex> lock{mu_};
            stop_ = true;
        }
        wake_.notify_one();
        if (io_.joinable()) {
            io_.join();
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Result<StoreLookup> get(PageId page, PageBuffer out) override {
        const std::lock_guard<std::mutex> lock{mu_};
        const auto found = pages_.find(page.value);
        if (found == pages_.end()) {
            return StoreLookup::kAbsent;
        }
        Entry& e = found->second;
        if (e.lost) {
            return Error{Errc::kIo, 0, "get a page: the spill file could not be read"};
        }
        if (!e.data) {
            return StoreLookup::kSpilled;
        }
        e.referenced = true;
        std::copy(e.data->begin(), e.data->end(), out.begin());
        return StoreLookup::kCopied;
    }

    Result<void> put(PageId page, PageView data) override {
        const std::lock_guard<std::mutex> lock{mu_};
        auto found = pages_.find(page.value);
        if (found == pages_.end() || !found->second.data) {  // it needs a place in RAM
            if (full()) {
                if (slots_used_ >= budget_slots_) {
                    return Error{Errc::kNoSpace, 0,
                                 "put a page: the home store's RAM and spill file are full"};
                }
                want_room_ = true;
                wake_.notify_one();
                return Error{Errc::kState, 0, "put a page: a page is being spilled to make room"};
            }
            found = pages_.try_emplace(page.value).first;
            if (found->second.slot >= 0) {  // the copy in the file is replaced
                release(found->second.slot);
            }
            found->second = Entry{};
            found->second.data = std::make_unique<Page>();
            ram_pages_++;
            clock_.push_back(page.value);
        }
        Entry& e = found->second;
        std::copy(data.begin(), data.end(), e.data->begin());
        e.stamp = ++stamps_;
        e.referenced = true;
        if (full() && slots_used_ < budget_slots_) {  // spill ahead: the next page finds room
            want_room_ = true;
            wake_.notify_one();
        }
        return {};
    }

    Result<void> load(PageId page) override {
        if (fd_ < 0) {
            return Error{Errc::kUnsupported, 0, "load a page: this store never spills"};
        }
        {
            const std::lock_guard<std::mutex> lock{mu_};
            const auto found = pages_.find(page.value);
            if (found == pages_.end()) {
                return Error{Errc::kNotFound, 0, "load a page: the store has no copy"};
            }
            Entry& e = found->second;
            if (!e.data && !e.lost) {
                if (!e.loading) {
                    e.loading = true;
                    loads_.push_back(page.value);
                    wake_.notify_one();
                }
                return {};
            }
        }
        events_.on_loaded(page);  // it is in RAM already (or lost: get() then says so)
        return {};
    }

    void drop(PageId page) override {
        const std::lock_guard<std::mutex> lock{mu_};
        const auto found = pages_.find(page.value);
        if (found == pages_.end()) {
            return;
        }
        if (found->second.data) {
            ram_pages_--;
        }
        if (found->second.slot >= 0) {
            release(found->second.slot);
        }
        pages_.erase(found);
    }

    [[nodiscard]] std::uint64_t ram_bytes() const noexcept override {
        const std::lock_guard<std::mutex> lock{mu_};
        return ram_pages_ * kPageSize;
    }

    void grow(std::uint64_t ram_cap_bytes, std::uint64_t spill_budget_bytes) noexcept override {
        const std::lock_guard<std::mutex> lock{mu_};
        if (cap_pages_ != 0) {  // no cap stays no cap
            cap_pages_ = std::max(cap_pages_, pages_of(ram_cap_bytes));
        }
        if (fd_ >= 0) {
            budget_slots_ = std::max(budget_slots_, spill_budget_bytes / kPageSize);
        }
    }

private:
    struct Entry {
        std::unique_ptr<Page> data;  // set while the page is in RAM
        std::int64_t slot = -1;      // its place in the spill file while it is there
        std::uint64_t stamp = 0;     // changes whenever the bytes do
        bool referenced = false;     // CLOCK's bit: used since the hand last passed
        bool loading = false;        // queued for the I/O thread to read back
        bool lost = false;           // the spill file could not be read
    };
    using Lock = std::unique_lock<std::mutex>;

    // A cap below one page is one page: 0 means no cap.
    static std::uint64_t pages_of(std::uint64_t bytes) noexcept {
        return bytes == 0 ? 0 : std::max<std::uint64_t>(1, bytes / kPageSize);
    }

    [[nodiscard]] bool full() const noexcept { return cap_pages_ != 0 && ram_pages_ >= cap_pages_; }

    Entry* find(std::uint64_t page) {
        const auto found = pages_.find(page);
        return found == pages_.end() ? nullptr : &found->second;
    }

    std::int64_t take_slot() {
        slots_used_++;
        if (free_slots_.empty()) {
            return next_slot_++;
        }
        const std::int64_t slot = free_slots_.back();
        free_slots_.pop_back();
        return slot;
    }

    void release(std::int64_t slot) {
        slots_used_--;
        free_slots_.push_back(slot);
    }

    // CLOCK: the first page in RAM not used since the hand last passed it; a used one loses
    // its bit and goes to the back. None only if nothing is in RAM.
    std::optional<std::uint64_t> pick() {
        for (std::size_t tries = 2 * clock_.size() + 1; tries > 0 && !clock_.empty(); tries--) {
            const std::uint64_t page = clock_.front();
            clock_.pop_front();
            Entry* e = find(page);
            if (e == nullptr || !e->data) {
                continue;  // it left RAM since it was queued
            }
            clock_.push_back(page);
            if (!e->referenced) {
                return page;
            }
            e->referenced = false;
        }
        return std::nullopt;
    }

    // ---- the I/O thread ------------------------------------------------------------------

    void run() {
        Lock lock{mu_};
        for (;;) {
            wake_.wait(lock, [this] { return stop_ || !loads_.empty() || want_room_; });
            if (stop_) {
                return;
            }
            if (!loads_.empty()) {
                const std::uint64_t page = loads_.front();
                loads_.pop_front();
                load_one(lock, page);
            } else {
                make_room(lock);
            }
        }
    }

    void told(Lock& lock, bool loaded, std::uint64_t page) {
        lock.unlock();
        if (loaded) {
            events_.on_loaded(PageId{page});
        } else {
            events_.on_spilled(PageId{page});
        }
        lock.lock();
    }

    [[nodiscard]] bool disk(bool write, Page& bytes, std::int64_t slot) const {
        for (std::size_t done = 0; done < kPageSize;) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes for a system call
            char* at = reinterpret_cast<char*>(bytes.data()) + done;
            const off_t where = (slot * static_cast<off_t>(kPageSize)) + static_cast<off_t>(done);
            const ssize_t moved = write ? ::pwrite(fd_, at, kPageSize - done, where)
                                        : ::pread(fd_, at, kPageSize - done, where);
            if (moved <= 0 && errno != EINTR) {
                return false;
            }
            done += moved > 0 ? static_cast<std::size_t>(moved) : 0;
        }
        return true;
    }

    // Writes cold pages out until RAM has a free place.
    void make_room(Lock& lock) {
        while (!stop_ && full() && slots_used_ < budget_slots_) {
            const std::optional<std::uint64_t> victim = pick();
            if (!victim) {
                break;
            }
            Entry* e = find(*victim);
            out_ = *e->data;
            const std::uint64_t stamp = e->stamp;
            const std::int64_t slot = take_slot();
            lock.unlock();
            const bool wrote = disk(true, out_, slot);
            lock.lock();
            e = find(*victim);
            if (wrote && e != nullptr && e->stamp == stamp && e->data) {
                e->data.reset();
                e->slot = slot;
                ram_pages_--;
                told(lock, false, *victim);
                continue;
            }
            release(slot);  // dropped or put again meanwhile: another page, then
            if (!wrote) {
                // The file takes no more. Nothing is lost, but no room can be made: the next
                // put() of a new page says kNoSpace. The caller waiting for room is woken
                // with a true statement: this page is in RAM.
                budget_slots_ = slots_used_;
                told(lock, true, *victim);
            }
        }
        want_room_ = false;
    }

    // Reads a spilled page back into RAM. When RAM is full a cold page takes its place in
    // the file, so neither the cap nor the budget is passed on the way.
    void load_one(Lock& lock, std::uint64_t page) {
        Entry* e = find(page);
        if (e == nullptr) {
            return;
        }
        e->loading = false;
        if (e->data || e->slot < 0) {
            if (e->data) {
                told(lock, true, page);
            }
            return;
        }
        const std::int64_t slot = e->slot;
        const std::uint64_t stamp = e->stamp;
        lock.unlock();
        const bool read = disk(false, in_, slot);
        lock.lock();
        e = find(page);
        if (e == nullptr || e->stamp != stamp || e->data) {  // dropped or put again meanwhile
            if (e != nullptr && e->data) {
                told(lock, true, page);
            }
            return;
        }
        if (!read) {
            e->lost = true;  // get() reports it and the job ends: never a stale or empty page
            told(lock, true, page);
            return;
        }
        e->slot = -1;  // the slot is this thread's now; the bytes are in in_
        for (;;) {
            e = find(page);
            if (e == nullptr || e->stamp != stamp) {
                release(slot);
                return;
            }
            const std::optional<std::uint64_t> victim = full() ? pick() : std::nullopt;
            if (!victim) {  // there is room
                e->data = std::make_unique<Page>(in_);
                ram_pages_++;
                release(slot);
                break;
            }
            Entry* cold = find(*victim);
            out_ = *cold->data;
            const std::uint64_t cold_stamp = cold->stamp;
            lock.unlock();
            const bool wrote = disk(true, out_, slot);
            lock.lock();
            cold = find(*victim);
            e = find(page);
            if (!wrote) {
                // The page's own slot may be half overwritten and RAM is full.
                if (e != nullptr && e->stamp == stamp) {
                    e->lost = true;
                    told(lock, true, page);
                }
                release(slot);
                return;
            }
            if (cold == nullptr || cold->stamp != cold_stamp || !cold->data) {
                continue;  // the cold page changed meanwhile: try another
            }
            std::unique_ptr<Page> place = std::move(cold->data);
            cold->slot = slot;
            told(lock, false, *victim);
            e = find(page);
            if (e == nullptr || e->stamp != stamp) {
                ram_pages_--;  // nobody wants the page any more
                return;
            }
            *place = in_;
            e->data = std::move(place);
            break;
        }
        e->referenced = true;
        clock_.push_back(page);
        told(lock, true, page);
    }

    std::uint64_t cap_pages_;     // 0 means no cap
    std::uint64_t budget_slots_;  // most pages in the spill file
    StoreEvents& events_;         // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    const int fd_;

    mutable std::mutex mu_;  // never held across a read or write of the file
    std::condition_variable wake_;
    std::unordered_map<std::uint64_t, Entry> pages_;
    std::deque<std::uint64_t> clock_;  // pages in RAM, the hand at the front
    std::deque<std::uint64_t> loads_;
    std::vector<std::int64_t> free_slots_;
    std::int64_t next_slot_ = 0;
    std::uint64_t slots_used_ = 0;
    std::uint64_t ram_pages_ = 0;
    std::uint64_t stamps_ = 0;
    bool want_room_ = false;
    bool stop_ = false;

    // The I/O thread's two pages, outside the cap: what it writes and what it reads.
    Page out_{};
    Page in_{};
    std::thread io_;
};

}  // namespace

Result<std::unique_ptr<HomeStore>> store_open(const StoreConfig& config, StoreEvents& events) {
    int fd = -1;
    if (config.spill_budget_bytes >= kPageSize) {
        // Unlinked at once: the file goes when the process does, however it ends.
        std::string path = (config.spill_dir.empty() ? std::string{"/var/tmp"} : config.spill_dir) +
                           "/paramesh-spill-" + std::to_string(config.job.value) + "-XXXXXX";
        fd = ::mkostemp(path.data(), O_CLOEXEC);
        if (fd < 0) {
            return Error{Errc::kIo, errno, "create the spill file"};
        }
        ::unlink(path.c_str());
    }
    return std::unique_ptr<HomeStore>{std::make_unique<SpillingStore>(config, events, fd)};
}

}  // namespace paramesh
