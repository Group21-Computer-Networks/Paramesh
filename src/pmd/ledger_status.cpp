#include "pmd/ledger_status.h"

#include "pmd/scheduler.h"

#include <algorithm>
#include <set>

namespace paramesh {

namespace {

std::uint64_t chunks_of(const WorkSide& side) {
    std::uint64_t chunks = 0;
    for (const auto& [task, work] : side.tasks) {
        chunks += work.chunks;
    }
    return chunks;
}

std::uint64_t seconds_of(double segment_seconds) {
    return static_cast<std::uint64_t>(std::max(0.0, segment_seconds));
}

JsonValue counts_json(std::uint64_t chunks_for, std::uint64_t chunks_by, std::uint64_t segsec_for,
                      std::uint64_t segsec_by) {
    return JsonValue{
        JsonObject{{"chunks_for_peer", JsonValue{static_cast<std::int64_t>(chunks_for)}},
                   {"chunks_by_peer", JsonValue{static_cast<std::int64_t>(chunks_by)}},
                   {"segsec_for_peer", JsonValue{static_cast<std::int64_t>(segsec_for)}},
                   {"segsec_by_peer", JsonValue{static_cast<std::int64_t>(segsec_by)}}}};
}

}  // namespace

LedgerTotal ledger_totals(const Ledger& ledger, NodeId peer) {
    LedgerTotal total{peer, 0, 0, 0, 0};
    double segsec_for = 0;
    double segsec_by = 0;
    if (const auto jobs = ledger.data().find(peer.value); jobs != ledger.data().end()) {
        for (const auto& [job, work] : jobs->second) {
            total.chunks_for_peer += chunks_of(work.for_peer);
            total.chunks_by_peer += chunks_of(work.by_peer);
            segsec_for += work.for_peer.segment_seconds;
            segsec_by += work.by_peer.segment_seconds;
        }
    }
    total.segsec_for_peer = seconds_of(segsec_for);
    total.segsec_by_peer = seconds_of(segsec_by);
    return total;
}

LedgerSyncEntry ledger_job_counts(const Ledger& ledger, NodeId peer, JobId job) {
    LedgerSyncEntry entry{job, 0, 0, 0, 0};
    const auto jobs = ledger.data().find(peer.value);
    if (jobs == ledger.data().end()) {
        return entry;
    }
    const auto work = jobs->second.find(job.value);
    if (work == jobs->second.end()) {
        return entry;
    }
    entry.chunks_for_peer = chunks_of(work->second.for_peer);
    entry.chunks_by_peer = chunks_of(work->second.by_peer);
    entry.segsec_for_peer = seconds_of(work->second.for_peer.segment_seconds);
    entry.segsec_by_peer = seconds_of(work->second.by_peer.segment_seconds);
    return entry;
}

bool counts_disagree(std::uint64_t mine, std::uint64_t theirs) {
    const std::uint64_t larger = std::max(mine, theirs);
    const std::uint64_t gap = larger - std::min(mine, theirs);
    // A gap of one is rounding: segment-seconds are whole seconds, and each side stops counting
    // when its own process ends.
    return gap > 1 && gap * 10 > larger;
}

bool totals_disagree(const LedgerTotal& mine, const LedgerTotal& theirs) {
    return counts_disagree(mine.chunks_for_peer, theirs.chunks_by_peer) ||
           counts_disagree(mine.chunks_by_peer, theirs.chunks_for_peer) ||
           counts_disagree(mine.segsec_for_peer, theirs.segsec_by_peer) ||
           counts_disagree(mine.segsec_by_peer, theirs.segsec_for_peer);
}

bool job_disagrees(const LedgerSyncEntry& mine, const LedgerSyncEntry& theirs) {
    return totals_disagree({NodeId{}, mine.chunks_for_peer, mine.chunks_by_peer,
                            mine.segsec_for_peer, mine.segsec_by_peer},
                           {NodeId{}, theirs.chunks_for_peer, theirs.chunks_by_peer,
                            theirs.segsec_for_peer, theirs.segsec_by_peer});
}

JsonValue ledger_status(const Ledger& ledger, const Announced& announced, NodeId self,
                        double scale) {
    // Every peer in the ledger, and every peer that announced totals, even with no work here.
    std::set<std::uint16_t> known;
    for (const auto& [peer_id, jobs] : ledger.data()) {
        known.insert(peer_id);
    }
    for (const auto& [peer_id, total] : announced.totals) {
        known.insert(peer_id);
    }
    JsonArray peers;
    for (const std::uint16_t peer_id : known) {
        const NodeId peer{peer_id};
        const std::map<std::uint32_t, JobWork> none;
        const auto found = ledger.data().find(peer_id);
        const auto& jobs = found == ledger.data().end() ? none : found->second;
        const LedgerTotal mine = ledger_totals(ledger, peer);
        JsonObject entry{
            {"peer", JsonValue{std::int64_t{peer_id}}},
            {"totals", counts_json(mine.chunks_for_peer, mine.chunks_by_peer, mine.segsec_for_peer,
                                   mine.segsec_by_peer)},
            {"balance", JsonValue{ledger.balance(peer)}},
            {"weight", JsonValue{slot_weight(ledger, peer, scale)}},
            {"pending_indexes", JsonValue{static_cast<std::int64_t>(ledger.pending_indexes(peer))}},
            {"announced", JsonValue{nullptr}},
            {"disagree", JsonValue{false}}};
        if (const auto said = announced.totals.find(peer_id); said != announced.totals.end()) {
            const LedgerTotal& t = said->second;
            entry["announced"] = counts_json(t.chunks_for_peer, t.chunks_by_peer, t.segsec_for_peer,
                                             t.segsec_by_peer);
            entry["disagree"] = JsonValue{totals_disagree(mine, t)};
        }
        JsonArray by_job;
        for (const auto& [job_id, work] : jobs) {
            const LedgerSyncEntry c = ledger_job_counts(ledger, peer, JobId{job_id});
            JsonObject job{{"job", JsonValue{std::int64_t{job_id}}},
                           {"counts", counts_json(c.chunks_for_peer, c.chunks_by_peer,
                                                  c.segsec_for_peer, c.segsec_by_peer)},
                           {"credit_given", JsonValue{work.by_peer.credit}},
                           {"credit_taken", JsonValue{work.for_peer.credit}},
                           {"announced", JsonValue{nullptr}},
                           {"disagree", JsonValue{false}}};
            if (const auto said = announced.jobs.find({peer_id, job_id});
                said != announced.jobs.end()) {
                const LedgerSyncEntry& t = said->second;
                job["announced"] = counts_json(t.chunks_for_peer, t.chunks_by_peer,
                                               t.segsec_for_peer, t.segsec_by_peer);
                job["disagree"] = JsonValue{job_disagrees(c, t)};
            }
            by_job.push_back(JsonValue{std::move(job)});
        }
        entry["jobs"] = JsonValue{std::move(by_job)};
        peers.push_back(JsonValue{std::move(entry)});
    }
    return JsonValue{JsonObject{{"node", JsonValue{std::int64_t{self.value}}},
                                {"peers", JsonValue{std::move(peers)}}}};
}

}  // namespace paramesh
