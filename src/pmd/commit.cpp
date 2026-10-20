#include "pmd/commit.h"

#include "platform/ids.h"

#include <algorithm>

namespace paramesh {

namespace {

std::uint64_t up(std::uint64_t bytes, std::uint64_t unit) noexcept {
    return (bytes + unit - 1) / unit * unit;
}

}  // namespace

std::uint64_t job_share(std::uint64_t region, std::uint16_t nodes) noexcept {
    const std::uint64_t segments =
        std::max<std::uint64_t>(1, up(region, kSegmentSize) / kSegmentSize);
    return up(segments, std::max<std::uint16_t>(nodes, 1)) / std::max<std::uint16_t>(nodes, 1) *
           kSegmentSize;
}

std::uint64_t job_need(std::uint64_t region, std::uint16_t nodes) noexcept {
    return up(up(job_share(region, nodes) * 10, 9) / 9, kPageSize);  // docs/PLAN.md: 90%
}

std::optional<JobCommit> job_commit(std::uint64_t region, std::uint16_t nodes,
                                    std::uint64_t free_ram, std::uint64_t free_spill,
                                    std::uint64_t cache_share) noexcept {
    const std::uint64_t share = job_share(region, nodes);
    const std::uint64_t need = job_need(region, nodes);
    JobCommit commit;
    commit.ram = std::min(free_ram, need) / kPageSize * kPageSize;
    const std::uint64_t home = commit.ram - commit.ram * cache_share / 100;
    commit.spill = up(std::max(share > home ? share - home : 0, need - commit.ram), kPageSize);
    commit.cache = commit.ram - home;
    if (commit.ram == 0 || commit.spill > free_spill) {
        return std::nullopt;
    }
    return commit;
}

std::optional<JobCommit> joiner_commit(std::uint64_t region, std::uint16_t nodes,
                                       std::uint64_t free_ram, std::uint64_t cache_share) noexcept {
    const std::uint64_t cache = std::max<std::uint64_t>(
        up(job_need(region, nodes) * cache_share / 100, kPageSize), kPageSize);
    if (cache > free_ram) {
        return std::nullopt;
    }
    return JobCommit{cache, 0, cache, true};
}

bool grow_commit(JobCommit& commit, std::uint64_t home, std::uint64_t free_ram,
                 std::uint64_t free_spill) noexcept {
    const std::uint64_t room = commit.ram - commit.cache + commit.spill;
    if (home <= room) {
        return true;
    }
    const std::uint64_t ram = std::min(free_ram, home - room) / kPageSize * kPageSize;
    const std::uint64_t spill = up(home - room - ram, kPageSize);
    if (spill > free_spill) {
        return false;
    }
    commit.ram += ram;
    commit.spill += spill;
    return true;
}

}  // namespace paramesh
