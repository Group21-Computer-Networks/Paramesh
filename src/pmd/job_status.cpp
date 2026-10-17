#include "pmd/job_status.h"

#include <chrono>
#include <cstddef>
#include <utility>

namespace paramesh {

namespace {

double per_second(std::uint64_t now, std::uint64_t before, Nanos seconds) {
    const double dt = std::chrono::duration<double>(seconds).count();
    return dt <= 0 || now < before ? 0 : static_cast<double>(now - before) / dt;
}

constexpr double kMbPerPage = static_cast<double>(kPageSize) / 1e6;

// The upper bound in microseconds of the bucket that holds the `share` (0 to 1) of all faults:
// a percentile, as close as power-of-two buckets allow.
std::int64_t percentile_us(const LStatsPayload& stats, double share) {
    std::uint64_t total = 0;
    for (const std::uint32_t count : stats.latency) {
        total += count;
    }
    if (total == 0) {
        return 0;
    }
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kLatencyBuckets; i++) {
        seen += stats.latency.at(i);
        if (static_cast<double>(seen) >= share * static_cast<double>(total)) {
            return std::int64_t{1} << i;
        }
    }
    return std::int64_t{1} << (kLatencyBuckets - 1);
}

}  // namespace

void JobStats::take(LStatsPayload report, Nanos at) {
    before = std::move(now);
    before_at = now_at;
    now = std::move(report);
    now_at = at;
}

JsonValue job_rates(const JobView& view) {
    const JobStats empty;
    const JobStats& s = view.stats != nullptr ? *view.stats : empty;
    const Nanos dt = s.now_at - s.before_at;
    return JsonValue{JsonObject{
        {"job", JsonValue{std::int64_t{view.job.value}}},
        {"faults_per_s", JsonValue{per_second(s.now.faults, s.before.faults, dt)}},
        {"mb_in_per_s", JsonValue{per_second(s.now.pages_in, s.before.pages_in, dt) * kMbPerPage}},
        {"mb_out_per_s",
         JsonValue{per_second(s.now.pages_out, s.before.pages_out, dt) * kMbPerPage}},
        {"slots", JsonValue{std::int64_t{view.slots}}}}};
}

JsonValue job_status(const JobView& view) {
    const JobStats empty;
    const LStatsPayload& now = view.stats != nullptr ? view.stats->now : empty.now;
    JsonArray buckets;
    for (std::size_t i = 0; i < kLatencyBuckets; i++) {
        buckets.push_back(JsonValue{JsonObject{
            {"below_us",
             JsonValue{i + 1 < kLatencyBuckets ? std::int64_t{1} << i : std::int64_t{-1}}},
            {"count", JsonValue{static_cast<std::int64_t>(now.latency.at(i))}}}});
    }
    JsonObject entry = std::get<JsonObject>(job_rates(view).v);
    entry["role"] = JsonValue{std::string{view.launcher ? "launcher" : "worker"}};
    entry["launcher"] = JsonValue{std::int64_t{view.launcher_node.value}};
    entry["faults"] = JsonValue{static_cast<std::int64_t>(now.faults)};
    entry["pages_in"] = JsonValue{static_cast<std::int64_t>(now.pages_in)};
    entry["pages_out"] = JsonValue{static_cast<std::int64_t>(now.pages_out)};
    entry["retries"] = JsonValue{static_cast<std::int64_t>(now.retries)};
    entry["fault_latency"] = JsonValue{JsonObject{{"buckets", JsonValue{std::move(buckets)}},
                                                  {"p50_us", JsonValue{percentile_us(now, 0.5)}},
                                                  {"p99_us", JsonValue{percentile_us(now, 0.99)}}}};
    return JsonValue{std::move(entry)};
}

JsonValue pages_status(const JobView& view, NodeId self) {
    JsonArray segments;
    if (view.stats != nullptr) {
        for (const LStatsSegment& s : view.stats->now.segments) {
            segments.push_back(
                JsonValue{JsonObject{{"segment", JsonValue{std::int64_t{s.segment.value}}},
                                     {"home", JsonValue{std::int64_t{s.home.value}}},
                                     {"home_here", JsonValue{s.home == self}},
                                     {"shared", JsonValue{std::int64_t{s.shared}}},
                                     {"modified", JsonValue{std::int64_t{s.modified}}}}});
        }
    }
    return JsonValue{JsonObject{{"job", JsonValue{std::int64_t{view.job.value}}},
                                {"node", JsonValue{std::int64_t{self.value}}},
                                {"segments", JsonValue{std::move(segments)}}}};
}

std::string sse_event(const std::string& name, const std::string& json) {
    return "event: " + name + "\ndata: " + json + "\n\n";
}

}  // namespace paramesh
