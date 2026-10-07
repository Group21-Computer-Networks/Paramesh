// src/store/: the RAM home store against its contract, and home placement.

#include "store/home_store.h"
#include "store/placement.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
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
