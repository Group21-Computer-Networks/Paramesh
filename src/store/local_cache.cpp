// LocalCache: of the oldest few pages, the one whose loss costs least (X-cache-policy); with a
// window of one, first in, first out. A page offered as a victim no longer counts for
// over_budget(), since it is on its way out, but still counts for full() until it is
// discarded (a modified one stays mapped until WRITEBACK_ACK). If the node machine keeps it,
// the caller notes it installed again. A page discarded after it was offered is remembered,
// up to as many as the budget, so that one wanted back soon after counts as reused.

#include "store/local_cache.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>

namespace paramesh {
namespace {

class CostCache final : public LocalCache {
public:
    CostCache(std::uint64_t budget_pages, std::uint32_t window) noexcept
        : budget_pages_(budget_pages), window_(std::max<std::uint32_t>(window, 1)) {}

    void note_installed(PageId page, bool writable) override {
        auto [entry, added] = mapped_.try_emplace(page.value, Entry{});
        Entry& e = entry->second;
        if (added) {
            if (const auto ghost = ghosts_.find(page.value); ghost != ghosts_.end()) {
                e.reuse = ghost->second + 1;  // given up, and wanted back
                ghosts_.erase(ghost);
            }
        }
        e.writable = e.writable || writable;
        if (!added && !e.offered) {
            return;  // still queued where it was
        }
        if (e.offered) {
            offered_--;
        }
        e.install = ++installs_;
        e.offered = false;
        order_.emplace_back(page.value, installs_);
    }

    void note_discarded(PageId page) override {
        const auto found = mapped_.find(page.value);
        if (found == mapped_.end()) {
            return;
        }
        if (found->second.offered) {
            offered_--;
            // ponytail: the oldest remembered page goes first, even if it was remembered
            // again since; an exact LRU of ghosts if the reuse counts prove too rough.
            ghosts_[page.value] = found->second.reuse;
            ghost_order_.push_back(page.value);
            while (ghosts_.size() > budget_pages_ && !ghost_order_.empty()) {
                ghosts_.erase(ghost_order_.front());
                ghost_order_.pop_front();
            }
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
        while (!order_.empty() && waiting(order_.front()) == nullptr) {
            order_.pop_front();  // discarded, offered or installed again since
        }
        Entry* best = nullptr;
        std::uint64_t best_page = 0;
        std::uint32_t seen = 0;
        for (std::size_t i = 0; i < order_.size() && seen < window_; i++) {
            Entry* e = waiting(order_[i]);
            if (e == nullptr) {
                continue;
            }
            seen++;
            if (best == nullptr || loss(*e) < loss(*best)) {
                best = e;
                best_page = order_[i].first;
            }
        }
        if (best == nullptr) {
            return std::nullopt;
        }
        best->offered = true;  // its place in the queue is skipped from now on
        offered_++;
        return PageId{best_page};
    }

private:
    struct Entry {
        std::uint64_t install = 0;  // which installation; older queue places are stale
        bool offered = false;
        bool writable = false;    // losing it means a write-back too
        std::uint64_t reuse = 0;  // times it came back after it was given up
    };

    // The entry behind a queue place still waiting to be offered, or null for a stale place.
    Entry* waiting(const std::pair<std::uint64_t, std::uint64_t>& place) {
        const auto found = mapped_.find(place.first);
        return found != mapped_.end() && found->second.install == place.second &&
                       !found->second.offered
                   ? &found->second
                   : nullptr;
    }

    static std::uint64_t loss(const Entry& e) noexcept {
        // ponytail: no fetch time in the score; X-cache-policy measured it slowing faults
        // (a page's last fetch time is mostly noise). A per-home cost if a workload needs it.
        return (1 + e.reuse) * (e.writable ? 2 : 1);
    }

    std::uint64_t budget_pages_;  // 0 means no cap
    std::uint32_t window_;        // how many of the oldest pages a victim is chosen from
    std::uint64_t installs_ = 0;
    std::size_t offered_ = 0;
    std::unordered_map<std::uint64_t, Entry> mapped_;
    std::deque<std::pair<std::uint64_t, std::uint64_t>> order_;  // page, installation
    std::unordered_map<std::uint64_t, std::uint64_t> ghosts_;    // page given up: its reuse
    std::deque<std::uint64_t> ghost_order_;
};

}  // namespace

Result<std::unique_ptr<LocalCache>> cache_open(std::uint64_t budget_bytes, std::uint32_t window) {
    // A budget below one page is one page: 0 would mean no cap.
    const std::uint64_t pages =
        budget_bytes == 0 ? 0 : std::max<std::uint64_t>(1, budget_bytes / kPageSize);
    return std::unique_ptr<LocalCache>{std::make_unique<CostCache>(pages, window)};
}

}  // namespace paramesh
