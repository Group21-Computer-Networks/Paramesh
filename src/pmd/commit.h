// What a node commits to one job (docs/PROTOCOL.md, SPAWN_REQ): its even share of the region,
// the region's segments over the N nodes the job asked for, rounded up, divided by the 90%
// admission headroom. RAM comes first; spill covers the rest, and enough of it that the home
// store, which gets the RAM less mem.cache_share, holds the whole share. A node that joins a
// running job commits only a local cache. A commitment grows when the node is made home for
// more than it holds, after a leave or when the job runs on fewer nodes than it asked for.
#ifndef PARAMESH_PMD_COMMIT_H
#define PARAMESH_PMD_COMMIT_H

#include <cstdint>
#include <optional>

namespace paramesh {

struct JobCommit {
    std::uint64_t ram = 0;
    std::uint64_t spill = 0;
    std::uint64_t cache = 0;  // the part of `ram` that is the job's local cache
    bool no_homes = false;    // joined mid-run: all of `ram` is the cache
};

// The share of a region of `region` bytes on each of `nodes` nodes, in whole segments.
std::uint64_t job_share(std::uint64_t region, std::uint16_t nodes) noexcept;
// The share divided by the 90% headroom, in whole pages: the least a node commits.
std::uint64_t job_need(std::uint64_t region, std::uint16_t nodes) noexcept;
// The commitment out of what is free, or nothing when it does not fit.
std::optional<JobCommit> job_commit(std::uint64_t region, std::uint16_t nodes,
                                    std::uint64_t free_ram, std::uint64_t free_spill,
                                    std::uint64_t cache_share) noexcept;
// A node joining a running job: a cache the size of the others' and no spill, or nothing when
// it does not fit the free RAM.
std::optional<JobCommit> joiner_commit(std::uint64_t region, std::uint16_t nodes,
                                       std::uint64_t free_ram, std::uint64_t cache_share) noexcept;
// Grows `commit` until its home store, the RAM less the cache plus the spill, holds `home`
// bytes: RAM first, then spill. False, and `commit` as it was, when what is free falls short.
bool grow_commit(JobCommit& commit, std::uint64_t home, std::uint64_t free_ram,
                 std::uint64_t free_spill) noexcept;

}  // namespace paramesh

#endif  // PARAMESH_PMD_COMMIT_H
