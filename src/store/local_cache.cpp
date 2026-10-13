// LocalCache: first in, first out. A page offered as a victim no longer counts for
// over_budget(), since it is on its way out, but still counts for full() until it is
// discarded (a modified one stays mapped until WRITEBACK_ACK). If the node machine keeps it,
// the caller notes it installed again.

#include "store/local_cache.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>

namespace paramesh {
namespace {

class FifoCache final : public LocalCache {
public:
    explicit FifoCache(std::uint64_t budget_pages) noexcept : budget_pages_(budget_pages) {}

    void note_installed(PageId page) override {
        auto [entry, added] = mapped_.try_emplace(page.value, Entry{});
        if (!added && !entry->second.offered) {
            return;  // still queued where it was
        }
        if (entry->second.offered) {
            offered_--;
        }
        entry->second = Entry{++installs_, false};
        order_.emplace_back(page.value, installs_);
    }

    void note_discarded(PageId page) override {
        const auto found = mapped_.find(page.value);
        if (found == mapped_.end()) {
            return;
        }
        if (found->second.offered) {
            offered_--;
        }
        mapped_.erase(found);  // its place in the queue is skipped when reached
    }

    [[nodiscard]] bool over_budget() const noexcept override {
        return budget_pages_ != 0 && mapped_.size() - offered_ > budget_pages_;
    }

    [[nodiscard]] bool full() const noexcept override {
        return budget_pages_ != 0 && mapped_.size() > budget_pages_;
    }

    std::optional<PageId> next_victim() override {
        while (!order_.empty()) {
            const auto [page, install] = order_.front();
            order_.pop_front();
            const auto found = mapped_.find(page);
            if (found != mapped_.end() && found->second.install == install) {
                found->second.offered = true;
                offered_++;
                return PageId{page};
            }
        }
        return std::nullopt;
    }

private:
    struct Entry {
        std::uint64_t install = 0;  // which installation; older queue places are stale
        bool offered = false;
    };
    std::uint64_t budget_pages_;  // 0 means no cap
    std::uint64_t installs_ = 0;
    std::size_t offered_ = 0;
    std::unordered_map<std::uint64_t, Entry> mapped_;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> order_;  // page, installation
};

}  // namespace

Result<std::unique_ptr<LocalCache>> cache_open(std::uint64_t budget_bytes) {
    // A budget below one page is one page: 0 would mean no cap.
    const std::uint64_t pages =
        budget_bytes == 0 ? 0 : std::max<std::uint64_t>(1, budget_bytes / kPageSize);
    return std::unique_ptr<LocalCache>{std::make_unique<FifoCache>(pages)};
}

}  // namespace paramesh
