// RAM-weighted rendezvous hashing: segment s goes to the member n with the highest
// -w(n) / ln(h(s, n) / 2^64), where w is the RAM the member contributes (the HLD's formula),
// among the members not yet home for their bound of segments (X-node-commit).

#include "store/placement.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace paramesh {
namespace {

// h(s, n): the 64-bit finaliser of SplitMix64 over the segment number and the node ID.
constexpr std::uint64_t mix(std::uint64_t segment, NodeId node) noexcept {
    std::uint64_t x = (segment << 16U) ^ node.value;
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

}  // namespace

Result<void> store_place_segments(std::span<const PlacementMember> members, std::span<Slot> homes,
                                  std::span<const NodeId> keep) {
    std::uint64_t total = 0;
    for (const PlacementMember& member : members) {
        total += member.ram_weight;
    }
    if (total == 0) {
        return Error{Errc::kInvalidArgument, 0, "place segments: no member contributes RAM"};
    }
    // Each member's bound: its weighted part of the segments, rounded up, so the bounds add up
    // to at least every segment and a node commits to no more than its even share.
    std::vector<std::uint64_t> bound(members.size());
    std::vector<std::uint64_t> load(members.size(), 0);
    for (std::size_t m = 0; m < members.size(); m++) {
        bound[m] = (homes.size() * members[m].ram_weight + total - 1) / total;
    }
    std::vector<bool> placed(homes.size(), false);
    for (std::size_t segment = 0; segment < keep.size() && segment < homes.size(); segment++) {
        for (std::size_t m = 0; m < members.size(); m++) {
            if (members[m].node == keep[segment] && load[m] < bound[m]) {
                homes[segment] = members[m].slot;
                load[m]++;
                placed[segment] = true;
            }
        }
    }
    for (std::size_t segment = 0; segment < homes.size(); segment++) {
        if (placed[segment]) {
            continue;
        }
        std::size_t best = members.size();
        double best_score = 0;
        for (std::size_t m = 0; m < members.size(); m++) {
            if (members[m].ram_weight == 0 || load[m] >= bound[m]) {
                continue;
            }
            // u is in (0, 1), so ln(u) < 0 and the score is positive; more RAM, higher score.
            const double u =
                (static_cast<double>(mix(segment, members[m].node)) + 1.0) / 18446744073709551618.0;
            const double score = -static_cast<double>(members[m].ram_weight) / std::log(u);
            // A tie goes to the lower node ID, so the result does not depend on the members' order.
            if (best == members.size() || score > best_score ||
                (score == best_score && members[m].node < members[best].node)) {
                best = m;
                best_score = score;
            }
        }
        homes[segment] = members[best].slot;
        load[best]++;
    }
    return {};
}

bool store_may_rehome(std::span<const Slot> homes, Slot to, std::size_t members) noexcept {
    if (members == 0) {
        return false;
    }
    const auto homed = static_cast<std::size_t>(
        std::count_if(homes.begin(), homes.end(), [to](Slot home) { return home == to; }));
    return homed + 1 <= 2 * ((homes.size() + members - 1) / members);
}

}  // namespace paramesh
