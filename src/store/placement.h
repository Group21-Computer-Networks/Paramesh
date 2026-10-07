// Home placement: which member is home for each segment.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 6. Implemented by M1-6.
#ifndef PARAMESH_STORE_PLACEMENT_H
#define PARAMESH_STORE_PLACEMENT_H

#include "platform/ids.h"
#include "platform/result.h"

#include <cstdint>
#include <span>

namespace paramesh {

struct PlacementMember {
    NodeId node;
    Slot slot;
    std::uint64_t ram_weight = 0;  // bytes of RAM contributed; 0 means it hosts no segment
};

// RAM-weighted rendezvous hashing. For each segment i, homes[i] becomes the slot of its home.
// The result depends only on the members' node IDs and weights and on the segment number, so
// it is the same on every node, and removing a member moves only that member's segments.
// Errc::kInvalidArgument if no member has a weight above 0.
Result<void> store_place_segments(std::span<const PlacementMember> members, std::span<Slot> homes);

}  // namespace paramesh

#endif  // PARAMESH_STORE_PLACEMENT_H
