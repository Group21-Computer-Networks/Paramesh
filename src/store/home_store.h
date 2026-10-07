// The home store: this node's own copies of the pages it is home for.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 6. Implemented by M1-6
// (RAM only) and M4-3 (CLOCK eviction and the spill file).
#ifndef PARAMESH_STORE_HOME_STORE_H
#define PARAMESH_STORE_HOME_STORE_H

#include "platform/ids.h"
#include "platform/result.h"

#include <cstdint>
#include <memory>

namespace paramesh {

// Receives what the store does on its own. Implemented by src/lib/, which passes each on to
// the home directory.
class StoreEvents {
public:
    StoreEvents() = default;
    StoreEvents(const StoreEvents&) = delete;
    StoreEvents& operator=(const StoreEvents&) = delete;
    StoreEvents(StoreEvents&&) = delete;
    StoreEvents& operator=(StoreEvents&&) = delete;
    virtual ~StoreEvents() = default;

    // A load() finished: the page is in RAM again and get() will copy it. May be called on a
    // thread of the store's own.
    virtual void on_loaded(PageId page) noexcept = 0;
    // The store moved the page to the spill file to make room. Same threading.
    virtual void on_spilled(PageId page) noexcept = 0;
};

enum class StoreLookup : std::uint8_t {
    kCopied,   // the page was in RAM and has been copied to `out`
    kAbsent,   // the store holds no copy: never put, or dropped
    kSpilled,  // the page is in the spill file; nothing was copied; call load()
};

// get(), put(), load() and drop() are called on the network thread and never wait for the
// disk.
class HomeStore {
public:
    HomeStore() = default;
    HomeStore(const HomeStore&) = delete;
    HomeStore& operator=(const HomeStore&) = delete;
    HomeStore(HomeStore&&) = delete;
    HomeStore& operator=(HomeStore&&) = delete;
    virtual ~HomeStore() = default;

    virtual Result<StoreLookup> get(PageId page, PageBuffer out) = 0;

    // Keeps a copy of the bytes, replacing any earlier copy. Errc::kNoSpace when the RAM cap
    // and the spill budget are both used up.
    virtual Result<void> put(PageId page, PageView data) = 0;

    // Starts reading a spilled page back. StoreEvents::on_loaded follows.
    virtual Result<void> load(PageId page) = 0;

    // Forgets the copy, in RAM or in the spill file.
    virtual void drop(PageId page) = 0;

    // Bytes of page data held in RAM now, for the cap and for the status API.
    [[nodiscard]] virtual std::uint64_t ram_bytes() const noexcept = 0;
};

struct StoreConfig {
    std::uint64_t ram_cap_bytes = 0;       // most page data to hold in RAM
    std::uint64_t spill_budget_bytes = 0;  // most to hold in the spill file; 0 for none
    JobId job;                             // names the spill file
};

// `events` must outlive the store.
Result<std::unique_ptr<HomeStore>> store_open(const StoreConfig& config, StoreEvents& events);

}  // namespace paramesh

#endif  // PARAMESH_STORE_HOME_STORE_H
