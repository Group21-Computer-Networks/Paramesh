// Local-cache accounting: which shared pages this node has mapped, in the order they were
// installed, so one of the oldest can be given up when the cap is reached: the one whose loss
// costs least (X-cache-policy).
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 6. Implemented by M4-2.
#ifndef PARAMESH_STORE_LOCAL_CACHE_H
#define PARAMESH_STORE_LOCAL_CACHE_H

#include "platform/ids.h"
#include "platform/result.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace paramesh {

// Called on the network thread only.
class LocalCache {
public:
    LocalCache() = default;
    LocalCache(const LocalCache&) = delete;
    LocalCache& operator=(const LocalCache&) = delete;
    LocalCache(LocalCache&&) = delete;
    LocalCache& operator=(LocalCache&&) = delete;
    virtual ~LocalCache() = default;

    // The page was installed, or discarded, on this node. `writable`: it may be modified, so
    // losing it may cost a write-back too. Called again for a page already mapped (an upgrade
    // to writable), it updates that and keeps the page's place.
    virtual void note_installed(PageId page, bool writable) = 0;
    virtual void note_discarded(PageId page) = 0;

    // True while more pages are mapped than the budget allows, not counting the pages offered
    // as victims that are still mapped (a modified page waits for WRITEBACK_ACK).
    [[nodiscard]] virtual bool over_budget() const noexcept = 0;

    // True while more pages are mapped than the budget allows, counting those as well: a page
    // about to be installed then waits until one of them is discarded (added by M4-2).
    [[nodiscard]] virtual bool full() const noexcept = 0;

    // Of the `window` pages mapped longest and not offered since they were last installed, the
    // cheapest to lose: (1 + the times it came back after this cache gave it up) x 2 if
    // writable; the oldest on a tie. None when every page has been offered. The caller
    // raises EVICT for it; if the node machine does not free it (a page in flight), the caller
    // asks again and gets the next. A window of 1 is first in, first out.
    virtual std::optional<PageId> next_victim() = 0;
};

// cache.victim_window, or 1 for cache.policy = fifo (X-cache-policy).
Result<std::unique_ptr<LocalCache>> cache_open(std::uint64_t budget_bytes,
                                               std::uint32_t window = 1);

}  // namespace paramesh

#endif  // PARAMESH_STORE_LOCAL_CACHE_H
