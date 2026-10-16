#include "pmd/scheduler.h"

#include <algorithm>
#include <cstddef>

namespace paramesh {

double slot_weight(const LedgerView& ledger, NodeId peer, double scale) {
    return std::max(0.25, 1.0 + ledger.balance(peer) / scale);
}

std::vector<std::uint16_t> allocate_slots(std::uint16_t capacity,
                                          const std::vector<SlotDemand>& jobs) {
    std::vector<std::uint16_t> slots(jobs.size(), 0);
    std::uint16_t left = capacity;
    // Jobs left without a slot come first, the longest waiting first: one slot each.
    std::vector<std::size_t> waiting;
    for (std::size_t i = 0; i < jobs.size(); i++) {
        if (jobs[i].waited > 0 && jobs[i].want > 0) {
            waiting.push_back(i);
        }
    }
    std::stable_sort(waiting.begin(), waiting.end(),
                     [&](std::size_t a, std::size_t b) { return jobs[a].waited > jobs[b].waited; });
    for (std::size_t i = 0; i < waiting.size() && left > 0; i++, left--) {
        slots[waiting[i]] = 1;
    }
    // Then this node's own jobs take what they want.
    for (std::size_t i = 0; i < jobs.size() && left > 0; i++) {
        if (jobs[i].own && slots[i] < jobs[i].want) {
            const auto take =
                static_cast<std::uint16_t>(std::min<int>(left, jobs[i].want - slots[i]));
            slots[i] = static_cast<std::uint16_t>(slots[i] + take);
            left = static_cast<std::uint16_t>(left - take);
        }
    }
    // ponytail: one slot at a time over every guest, O(slots x jobs); fine for tens of each.
    while (left > 0) {
        std::size_t best = jobs.size();
        double lowest = 0;
        for (std::size_t i = 0; i < jobs.size(); i++) {
            if (jobs[i].own || slots[i] >= jobs[i].want) {
                continue;
            }
            const double share =
                std::max(static_cast<double>(slots[i]) / capacity, jobs[i].ram_share);
            const double key = share / jobs[i].weight;
            if (best == jobs.size() || key < lowest) {
                best = i;
                lowest = key;
            }
        }
        if (best == jobs.size()) {
            break;  // nobody wants more
        }
        slots[best]++;
        left--;
    }
    return slots;
}

}  // namespace paramesh
