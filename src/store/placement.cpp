// RAM-weighted rendezvous hashing: segment s goes to the member n with the highest
// -w(n) / ln(h(s, n) / 2^64), where w is the RAM the member contributes (the HLD's formula).

#include "store/placement.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

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

Result<void> store_place_segments(std::span<const PlacementMember> members, std::span<Slot> homes) {
    for (std::size_t segment = 0; segment < homes.size(); segment++) {
        const PlacementMember* best = nullptr;
        double best_score = 0;
        for (const PlacementMember& member : members) {
            if (member.ram_weight == 0) {
                continue;
            }
            // u is in (0, 1), so ln(u) < 0 and the score is positive; more RAM, higher score.
            const double u =
                (static_cast<double>(mix(segment, member.node)) + 1.0) / 18446744073709551618.0;
            const double score = -static_cast<double>(member.ram_weight) / std::log(u);
            // A tie goes to the lower node ID, so the result does not depend on the members' order.
            if (best == nullptr || score > best_score ||
                (score == best_score && member.node < best->node)) {
                best = &member;
                best_score = score;
            }
        }
        if (best == nullptr) {
            return Error{Errc::kInvalidArgument, 0, "place segments: no member contributes RAM"};
        }
        homes[segment] = best->slot;
    }
    return {};
}

}  // namespace paramesh
