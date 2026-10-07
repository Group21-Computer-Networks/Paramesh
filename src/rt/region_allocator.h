// The bump allocator behind pm_malloc. Private to src/rt/: the runtime's allocate() calls it.
//
// It hands out offsets into the shared region and never takes one back. An allocation of a
// page or more starts on a page boundary and owns its pages to the end; a smaller one is
// aligned to 16 bytes (include/paramesh.h, pm_malloc). Launcher's main thread only.
#ifndef PARAMESH_RT_REGION_ALLOCATOR_H
#define PARAMESH_RT_REGION_ALLOCATOR_H

#include "platform/ids.h"

#include <cstdint>
#include <optional>

namespace paramesh {

class RegionAllocator {
public:
    explicit RegionAllocator(std::uint64_t region_bytes) noexcept : end_(region_bytes) {}

    // The offset of `bytes` fresh bytes from the region's base, or nothing if `bytes` is 0 or
    // the region is full.
    std::optional<std::uint64_t> allocate(std::uint64_t bytes) noexcept {
        const bool large = bytes >= kPageSize;
        const std::uint64_t align = large ? kPageSize : 16;
        const std::uint64_t start = (next_ + align - 1) / align * align;
        if (bytes == 0 || start > end_ || bytes > end_ - start) {
            return std::nullopt;
        }
        next_ = start + bytes;
        if (large) {
            next_ = (next_ + kPageSize - 1) / kPageSize * kPageSize;
        }
        return start;
    }

private:
    std::uint64_t next_ = 0;
    std::uint64_t end_;
};

}  // namespace paramesh

#endif  // PARAMESH_RT_REGION_ALLOCATOR_H
