#include "pmd/commit.h"

#include "platform/ids.h"

#include <algorithm>

namespace paramesh {

namespace {

std::uint64_t up(std::uint64_t bytes, std::uint64_t unit) noexcept {
    return (bytes + unit - 1) / unit * unit;
}

// mem.cache_share is a share of the node's cap, up to the region: a cache smaller than what a loop
// reads sends the same pages over the network again and again. matmul 1024 took 122 s on two
// laptops with gigabytes free, when its cache was a share of the job's own small commitment.
void room_for_cache(JobCommit& commit, std::uint64_t region, std::uint64_t free_ram,
                    std::uint64_t cache_share, std::uint64_t cap_ram) noexcept {
    const std::uint64_t wanted = std::min(region, cap_ram / 100 * cache_share);
    const std::uint64_t spare = free_ram > commit.ram ? free_ram - commit.ram : 0;
    const std::uint64_t more =
        std::min(wanted > commit.cache ? wanted - commit.cache : 0, spare) / kPageSize * kPageSize;
    commit.ram += more;
    commit.cache += more;
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
                                    std::uint64_t cache_share, std::uint64_t cap_ram) noexcept {
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
    room_for_cache(commit, region, free_ram, cache_share, cap_ram);
    return commit;
}

std::optional<JobCommit> joiner_commit(std::uint64_t region, std::uint16_t nodes,
                                       std::uint64_t free_ram, std::uint64_t cache_share,
                                       std::uint64_t cap_ram) noexcept {
    const std::uint64_t cache = std::max<std::uint64_t>(
        up(job_need(region, nodes) * cache_share / 100, kPageSize), kPageSize);
    if (cache > free_ram) {
        return std::nullopt;
    }
    JobCommit commit{cache, 0, cache, true};
    room_for_cache(commit, region, free_ram, cache_share, cap_ram);
    return commit;
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
