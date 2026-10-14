// src/store/: the RAM home store against its contract, and home placement.

#include "store/home_store.h"
#include "store/local_cache.h"
#include "store/placement.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace {

using paramesh::Errc;
using paramesh::NodeId;
using paramesh::PageId;
using paramesh::PlacementMember;
using paramesh::Slot;
using PageArray = std::array<std::byte, paramesh::kPageSize>;

class NoEvents final : public paramesh::StoreEvents {
public:
    void on_loaded(PageId /*page*/) noexcept override {}
    void on_spilled(PageId /*page*/) noexcept override {}
};

// Records what the store says it did, and waits for it.
class Told final : public paramesh::StoreEvents {
public:
    void on_loaded(PageId page) noexcept override {
        add(loaded_, page);
        std::unique_lock<std::mutex> lock{mu_};
        changed_.wait_for(lock, std::chrono::seconds{10}, [this] { return !holding_; });
    }
    void on_spilled(PageId page) noexcept override { add(spilled_, page); }

    // While held, the store's I/O thread stops in the next on_loaded until let go: what the
    // test checks then cannot change under it.
    void hold() {
        const std::lock_guard<std::mutex> lock{mu_};
        holding_ = true;
    }
    void let_go() {
        {
            const std::lock_guard<std::mutex> lock{mu_};
            holding_ = false;
        }
        changed_.notify_all();
    }

    // The pages spilled, or loaded, so far; waits until there are at least `count`.
    std::vector<PageId> spilled(std::size_t count) { return wait(spilled_, count); }
    std::vector<PageId> loaded(std::size_t count) { return wait(loaded_, count); }

private:
    void add(std::vector<PageId>& list, PageId page) noexcept {
        {
            const std::lock_guard<std::mutex> lock{mu_};
            list.push_back(page);
        }
        changed_.notify_all();
    }
    std::vector<PageId> wait(const std::vector<PageId>& list, std::size_t count) {
        std::unique_lock<std::mutex> lock{mu_};
        changed_.wait_for(lock, std::chrono::seconds{10}, [&] { return list.size() >= count; });
        return list;
    }

    std::mutex mu_;
    std::condition_variable changed_;
    std::vector<PageId> spilled_;
    std::vector<PageId> loaded_;
    bool holding_ = false;
};

// A store that may hold `ram_pages` in RAM and `spill_pages` in a spill file under /tmp.
std::unique_ptr<paramesh::HomeStore> spilling_store(std::uint64_t ram_pages,
                                                    std::uint64_t spill_pages, Told& told) {
    paramesh::StoreConfig config;
    config.ram_cap_bytes = ram_pages * paramesh::kPageSize;
    config.spill_budget_bytes = spill_pages * paramesh::kPageSize;
    config.job = paramesh::JobId{7};
    config.spill_dir = "/tmp";
    auto opened = paramesh::store_open(config, told);
    REQUIRE(opened.ok());
    return std::move(opened).value();
}

PageArray& out_of_use() {
    static PageArray page{};
    return page;
}

// A page whose every byte tells which page it is.
PageArray filled(std::uint64_t n) {
    PageArray page{};
    page.fill(static_cast<std::byte>(n * 7 + 1));
    return page;
}

constexpr std::size_t kSegments = 2048;  // a full 4 GiB region

std::vector<Slot> place(const std::vector<PlacementMember>& members) {
    std::vector<Slot> homes(kSegments);
    REQUIRE(paramesh::store_place_segments(members, homes).ok());
    return homes;
}

std::ptrdiff_t count(const std::vector<Slot>& homes, std::uint8_t slot) {
    return std::count(homes.begin(), homes.end(), Slot{slot});
}

// Three nodes with slots 0, 1, 2; node 5 contributes twice the RAM of the others.
std::vector<PlacementMember> three() {
    return {{NodeId{3}, Slot{0}, 2ULL << 30U},
            {NodeId{5}, Slot{1}, 4ULL << 30U},
            {NodeId{9}, Slot{2}, 2ULL << 30U}};
}

}  // namespace

TEST_CASE("the RAM store returns what was put, replaces, drops and honours its cap") {
    NoEvents events;
    paramesh::StoreConfig config;
    config.ram_cap_bytes = 2 * paramesh::kPageSize;
    auto opened = paramesh::store_open(config, events);
    REQUIRE(opened.ok());
    paramesh::HomeStore& store = *opened.value();
    PageArray ones{};
    ones.fill(std::byte{1});
    PageArray twos{};
    twos.fill(std::byte{2});
    PageArray out{};

    CHECK(store.get(PageId{4}, out).value() == paramesh::StoreLookup::kAbsent);
    REQUIRE(store.put(PageId{4}, ones).ok());
    REQUIRE(store.put(PageId{4}, twos).ok());  // replaces; still one page
    CHECK(store.ram_bytes() == paramesh::kPageSize);
    CHECK(store.get(PageId{4}, out).value() == paramesh::StoreLookup::kCopied);
    CHECK(out == twos);

    REQUIRE(store.put(PageId{5}, ones).ok());
    CHECK(store.put(PageId{6}, ones).error().code ==
          Errc::kNoSpace);  // a third page is over the cap
    store.drop(PageId{5});
    CHECK(store.put(PageId{6}, ones).ok());
    CHECK(store.get(PageId{5}, out).value() == paramesh::StoreLookup::kAbsent);
    CHECK(store.load(PageId{4}).error().code == Errc::kUnsupported);
}

TEST_CASE("a full store spills the cold page, says so, and reads it back on load") {
    Told told;
    const auto store = spilling_store(2, 4, told);
    PageArray out{};

    REQUIRE(store->put(PageId{1}, filled(1)).ok());
    REQUIRE(store->put(PageId{2}, filled(2)).ok());  // RAM is full: one page is spilled ahead
    CHECK(told.spilled(1) == std::vector<PageId>{PageId{1}});  // CLOCK: the one put first
    CHECK(store->ram_bytes() == paramesh::kPageSize);
    CHECK(store->get(PageId{1}, out).value() == paramesh::StoreLookup::kSpilled);
    CHECK(store->get(PageId{2}, out).value() == paramesh::StoreLookup::kCopied);
    CHECK(out == filled(2));

    told.hold();  // the I/O thread stops once page 1 is back, until let_go() below
    REQUIRE(store->load(PageId{1}).ok());  // there is room: it comes back beside page 2
    CHECK(told.loaded(1) == std::vector<PageId>{PageId{1}});
    CHECK(store->get(PageId{1}, out).value() == paramesh::StoreLookup::kCopied);
    CHECK(out == filled(1));
    CHECK(store->ram_bytes() == 2 * paramesh::kPageSize);

    // RAM is full and nothing is on its way out: a new page is told to wait, and is taken
    // once a page has been spilled. The store never holds a third page in RAM.
    CHECK(store->put(PageId{3}, filled(3)).error().code == Errc::kState);
    CHECK(store->ram_bytes() == 2 * paramesh::kPageSize);
    told.let_go();  // now the page asked for is spilled
    REQUIRE(told.spilled(2).size() == 2);
    REQUIRE(store->put(PageId{3}, filled(3)).ok());
    CHECK(store->ram_bytes() <= 2 * paramesh::kPageSize);

    REQUIRE(store->load(PageId{2}).ok());  // already in RAM, or read back: either way it is told
    REQUIRE(store->load(PageId{2}).ok());
    CHECK_FALSE(told.loaded(2).empty());
    CHECK(store->load(PageId{9}).error().code == Errc::kNotFound);
}

TEST_CASE("the store refuses a page only when RAM and the spill budget are both used up") {
    Told told;
    const auto store = spilling_store(1, 1, told);
    REQUIRE(store->put(PageId{1}, filled(1)).ok());
    REQUIRE(told.spilled(1).size() == 1);  // RAM empty, the file full
    REQUIRE(store->put(PageId{2}, filled(2)).ok());
    CHECK(store->put(PageId{3}, filled(3)).error().code == Errc::kNoSpace);
    REQUIRE(store->put(PageId{2}, filled(4)).ok());  // replacing a page in RAM needs no room

    store->drop(PageId{1});  // its place in the file is free again
    CHECK(store->get(PageId{1}, out_of_use()).value() == paramesh::StoreLookup::kAbsent);
    CHECK(store->put(PageId{3}, filled(3)).error().code == Errc::kState);
    REQUIRE(told.spilled(2).size() == 2);
    REQUIRE(store->put(PageId{3}, filled(3)).ok());
}

TEST_CASE("forty pages through four places in RAM: every page comes back as it was put") {
    Told told;
    constexpr std::uint64_t kPages = 40;
    constexpr std::uint64_t kRam = 4;
    const auto store = spilling_store(kRam, kPages, told);
    std::size_t spills = 0;
    for (std::uint64_t n = 0; n < kPages; n++) {
        paramesh::Result<void> put = store->put(PageId{n}, filled(n));
        while (!put.ok()) {
            REQUIRE(put.error().code == Errc::kState);
            spills++;  // wait for one more
            REQUIRE(told.spilled(spills).size() >= spills);
            put = store->put(PageId{n}, filled(n));
        }
        spills = told.spilled(0).size();
        CHECK(store->ram_bytes() <= kRam * paramesh::kPageSize);
    }
    PageArray out{};
    std::size_t loads = 0;
    for (std::uint64_t round = 0; round < 2; round++) {
        for (std::uint64_t n = 0; n < kPages; n++) {
            const PageId page{round == 0 ? n : kPages - 1 - n};
            paramesh::StoreLookup got = store->get(page, out).value();
            if (got == paramesh::StoreLookup::kSpilled) {
                loads = told.loaded(0).size();
                REQUIRE(store->load(page).ok());  // RAM is full: a cold page takes its place
                REQUIRE(told.loaded(loads + 1).size() > loads);
                got = store->get(page, out).value();
            }
            REQUIRE(got == paramesh::StoreLookup::kCopied);
            CHECK(out == filled(page.value));
            CHECK(store->ram_bytes() <= kRam * paramesh::kPageSize);
        }
    }
    CHECK(told.spilled(0).size() > kPages - kRam);  // loading spilled others
}

TEST_CASE("placement is the same on every node: it does not depend on the order of the members") {
    const std::vector<Slot> homes = place(three());
    CHECK(place({three()[2], three()[0], three()[1]}) == homes);
    CHECK(place({three()[1], three()[2], three()[0]}) == homes);
    CHECK(place(three()) == homes);  // and not on when it is computed
}

TEST_CASE("removing one node moves only that node's segments") {
    const std::vector<Slot> before = place(three());
    const std::vector<Slot> after = place({three()[0], three()[2]});  // node 5, slot 1, has left
    std::size_t moved = 0;
    for (std::size_t s = 0; s < kSegments; s++) {
        if (before[s] == Slot{1}) {
            CHECK(after[s] != Slot{1});
            moved++;
        } else {
            CHECK(after[s] == before[s]);
        }
    }
    CHECK(moved == static_cast<std::size_t>(count(before, 1)));
}

TEST_CASE(
    "a node that contributes more RAM is home for more segments, and one with none for none") {
    const std::vector<Slot> homes = place(three());
    // Weights 2 : 4 : 2, so about 512 : 1024 : 512 of 2,048.
    CHECK(count(homes, 1) > 900);
    CHECK(count(homes, 1) < 1150);
    CHECK(count(homes, 0) > 420);
    CHECK(count(homes, 2) > 420);

    std::vector<PlacementMember> with_guest = three();
    with_guest.push_back({NodeId{12}, Slot{3}, 0});  // joined mid-run: hosts no segment
    CHECK(place(with_guest) == homes);

    std::vector<Slot> none(4);
    const std::vector<PlacementMember> no_ram = {{NodeId{3}, Slot{0}, 0}};
    CHECK(paramesh::store_place_segments(no_ram, none).error().code == Errc::kInvalidArgument);
}

TEST_CASE(
    "the local cache gives up pages oldest first, skips discarded ones, and re-queues kept ones") {
    auto opened = paramesh::cache_open(2 * paramesh::kPageSize);
    REQUIRE(opened.ok());
    paramesh::LocalCache& cache = *opened.value();
    const auto page = [](std::uint64_t n) { return paramesh::PageId{n}; };

    cache.note_installed(page(1));
    cache.note_installed(page(2));
    CHECK_FALSE(cache.over_budget());
    cache.note_installed(page(3));
    CHECK(cache.over_budget());
    CHECK(cache.full());
    CHECK(cache.next_victim() == page(1));  // the oldest
    CHECK_FALSE(cache.over_budget());       // an offered page no longer counts here ...
    CHECK(cache.full());                    // ... but does here, until it is discarded

    cache.note_discarded(page(2));  // gone some other way (an INV)
    cache.note_installed(page(4));
    cache.note_installed(page(5));
    CHECK(cache.over_budget());
    CHECK(cache.next_victim() == page(3));  // 2 is skipped
    cache.note_installed(page(3));          // the node machine kept 3: it goes to the back
    CHECK(cache.over_budget());
    CHECK(cache.next_victim() == page(4));
    CHECK(cache.next_victim() == page(5));
    CHECK(cache.next_victim() == page(3));
    CHECK(cache.next_victim() == std::nullopt);

    CHECK(cache.full());
    cache.note_discarded(page(1));  // 1's WRITEBACK_ACK came
    CHECK(cache.full());            // 3, 4 and 5 are still mapped
    cache.note_discarded(page(3));
    cache.note_discarded(page(4));
    cache.note_discarded(page(5));
    cache.note_installed(page(1));
    CHECK(cache.next_victim() == page(1));  // installed again: offered again

    auto unlimited = paramesh::cache_open(0);  // no cap
    for (std::uint64_t n = 0; n < 1000; n++) {
        unlimited.value()->note_installed(page(n));
    }
    CHECK_FALSE(unlimited.value()->over_budget());
    CHECK_FALSE(unlimited.value()->full());

    auto tiny = paramesh::cache_open(1024);  // less than a page: one page, not "no cap"
    tiny.value()->note_installed(page(1));
    CHECK_FALSE(tiny.value()->full());
    tiny.value()->note_installed(page(2));
    CHECK(tiny.value()->full());
}
