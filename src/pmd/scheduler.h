// Worker slots on this node (M5-3): how many of its threads each job on it may use.
//
// A job left without a slot at the last share-outs gets one first, the longest waiting first,
// so none waits longer than it must while slots exist. Then this node's own jobs take what they
// want. The rest go one at a time to the guest job with the lowest dominant share divided by
// its weight, the share being the larger of its part of this node's slots and its part of the
// home-store RAM. The weight comes from this node's own ledger: w = max(0.25, 1 + balance /
// scale).
#ifndef PARAMESH_PMD_SCHEDULER_H
#define PARAMESH_PMD_SCHEDULER_H

#include "pmd/ledger.h"

#include <cstdint>
#include <vector>

namespace paramesh {

struct SlotDemand {
    bool own = false;          // launched on this node
    std::uint16_t want = 0;    // the most threads it can use here
    double ram_share = 0;      // its part of this node's home-store RAM, 0 to 1
    double weight = 1;         // from slot_weight(); unused for own jobs
    std::uint32_t waited = 0;  // share-outs in a row it had no slot at, while it wanted one
};

// w = max(0.25, 1 + balance / scale), the balance being the peer's in this node's ledger.
double slot_weight(const LedgerView& ledger, NodeId peer, double scale);

// The slots of each job, in the order given. Their sum is at most `capacity`.
std::vector<std::uint16_t> allocate_slots(std::uint16_t capacity,
                                          const std::vector<SlotDemand>& jobs);

}  // namespace paramesh

#endif  // PARAMESH_PMD_SCHEDULER_H
