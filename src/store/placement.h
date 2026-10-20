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

// Bounded RAM-weighted rendezvous hashing. For each segment i, homes[i] becomes the slot of its
// home. No member is home for more than ceil(segments x its weight / all the weights), the
// even share its node commits to (docs/PROTOCOL.md, SPAWN_REQ). A segment whose `keep` entry
// names a member with room stays there; the others go, in segment order, to the member with
// room that scores highest. The result depends only on the members' node IDs and weights, the
// segment number and `keep`, so it is the same on every node. With `keep` the current homes,
// removing a member moves only that member's segments.
// Errc::kInvalidArgument if no member has a weight above 0.
Result<void> store_place_segments(std::span<const PlacementMember> members, std::span<Slot> homes,
                                  std::span<const NodeId> keep = {});

}  // namespace paramesh

#endif  // PARAMESH_STORE_PLACEMENT_H
