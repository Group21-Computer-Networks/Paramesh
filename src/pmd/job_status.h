// The jobs on this node as the status API shows them (M6-1): what each job process said in its
// last two L_STATS, as counts, rates and a fault-latency histogram, and the pages it holds per
// segment. docs/STATUS_API.md documents the JSON.
#ifndef PARAMESH_PMD_JOB_STATUS_H
#define PARAMESH_PMD_JOB_STATUS_H

#include "platform/clock.h"
#include "platform/ids.h"
#include "platform/json.h"
#include "wire/payloads.h"

#include <cstdint>
#include <string>

namespace paramesh {

// A job process's last two reports, for rates.
struct JobStats {
    LStatsPayload now;
    LStatsPayload before;
    Nanos now_at{};
    Nanos before_at{};
    void take(LStatsPayload report, Nanos at);
};

// One job process on this node.
struct JobView {
    JobId job;
    bool launcher = false;  // the job's launcher runs here; else a worker of it
    NodeId launcher_node;
    std::uint16_t slots = 0;
    const JobStats* stats = nullptr;
};

// GET /v1/jobs: one entry.
JsonValue job_status(const JobView& view);
// GET /v1/jobs/{id}/pages.
JsonValue pages_status(const JobView& view, NodeId self);
// Rates of one job for the stats event of /v1/events.
JsonValue job_rates(const JobView& view);

// A server-sent event: "event: <name>\ndata: <json>\n\n".
std::string sse_event(const std::string& name, const std::string& json);

}  // namespace paramesh

#endif  // PARAMESH_PMD_JOB_STATUS_H
