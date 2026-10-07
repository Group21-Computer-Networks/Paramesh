// Local-cache accounting: which shared pages this node has mapped, in the order they were
// installed, so the oldest can be given up when the cap is reached.
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

    // The page was installed, or discarded, on this node.
    virtual void note_installed(PageId page) = 0;
    virtual void note_discarded(PageId page) = 0;

    // True while more pages are mapped than the budget allows.
    [[nodiscard]] virtual bool over_budget() const noexcept = 0;

    // The page mapped longest that has not been offered since it was last installed, or none.
    // The caller raises EVICT for it; if the node machine does not free it (a page in flight),
    // the caller asks again and gets the next.
    virtual std::optional<PageId> next_victim() = 0;
};

Result<std::unique_ptr<LocalCache>> cache_open(std::uint64_t budget_bytes);

}  // namespace paramesh

#endif  // PARAMESH_STORE_LOCAL_CACHE_H
