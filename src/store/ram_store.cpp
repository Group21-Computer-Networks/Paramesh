// HomeStore in RAM only: no eviction, no spill file. M4-3 adds both.

#include "store/home_store.h"

#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>

namespace paramesh {
namespace {

class RamStore final : public HomeStore {
public:
    explicit RamStore(std::uint64_t cap_bytes) noexcept : cap_bytes_(cap_bytes) {}

    Result<StoreLookup> get(PageId page, PageBuffer out) override {
        const auto found = pages_.find(page.value);
        if (found == pages_.end()) {
            return StoreLookup::kAbsent;
        }
        std::copy(found->second->begin(), found->second->end(), out.begin());
        return StoreLookup::kCopied;
    }

    Result<void> put(PageId page, PageView data) override {
        auto found = pages_.find(page.value);
        if (found == pages_.end()) {
            if (cap_bytes_ != 0 && ram_bytes() + kPageSize > cap_bytes_) {
                return Error{Errc::kNoSpace, 0, "put a page: the home store is at its RAM cap"};
            }
            found = pages_.emplace(page.value, std::make_unique<Page>()).first;
        }
        std::copy(data.begin(), data.end(), found->second->begin());
        return {};
    }

    Result<void> load(PageId /*page*/) override {
        return Error{Errc::kUnsupported, 0, "load a page: this store never spills"};
    }

    void drop(PageId page) override { pages_.erase(page.value); }

    [[nodiscard]] std::uint64_t ram_bytes() const noexcept override {
        return pages_.size() * kPageSize;
    }

private:
    using Page = std::array<std::byte, kPageSize>;
    std::uint64_t cap_bytes_;  // 0 means no cap
    std::unordered_map<std::uint64_t, std::unique_ptr<Page>> pages_;
};

}  // namespace

Result<std::unique_ptr<HomeStore>> store_open(const StoreConfig& config, StoreEvents& /*events*/) {
    return std::unique_ptr<HomeStore>{std::make_unique<RamStore>(config.ram_cap_bytes)};
}

}  // namespace paramesh
