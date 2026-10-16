// What this node announces from its ledger and what it makes of its peers' announcements
// (M5-4): raw totals on HELLO, per-job counts in LEDGER_SYNC, the disagreement alarm and
// GET /v1/ledger. What peers announce is shown and compared, and never goes into the ledger, a
// weight, a quota or an admission decision.
#ifndef PARAMESH_PMD_LEDGER_STATUS_H
#define PARAMESH_PMD_LEDGER_STATUS_H

#include "pmd/ledger.h"
#include "wire/payloads.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace paramesh {

// This node's raw totals with `peer` over all jobs, segment-seconds rounded down.
LedgerTotal ledger_totals(const Ledger& ledger, NodeId peer);
// The same for one job.
LedgerSyncEntry ledger_job_counts(const Ledger& ledger, NodeId peer, JobId job);

// Two raw counts disagree when they differ by more than 10% of the larger, and by more than 1.
bool counts_disagree(std::uint64_t mine, std::uint64_t theirs);
// `mine` are this node's counts with a peer, `theirs` that peer's with this node: what one did
// for the other is compared with what the other says it got.
bool totals_disagree(const LedgerTotal& mine, const LedgerTotal& theirs);
bool job_disagrees(const LedgerSyncEntry& mine, const LedgerSyncEntry& theirs);

// What peers said: their totals from HELLO by peer, and their counts per job from LEDGER_SYNC
// by (peer, job).
struct Announced {
    std::map<std::uint16_t, LedgerTotal> totals;
    std::map<std::pair<std::uint16_t, std::uint32_t>, LedgerSyncEntry> jobs;
};

// GET /v1/ledger: for each peer, this node's totals, credits, balance and weight, the peer's
// announced totals and whether they disagree, and the same for each job.
JsonValue ledger_status(const Ledger& ledger, const Announced& announced, NodeId self,
                        double scale);

}  // namespace paramesh

#endif  // PARAMESH_PMD_LEDGER_STATUS_H
