// The numbers the design fixes and the ID types every directory shares.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 2.
#ifndef PARAMESH_PLATFORM_IDS_H
#define PARAMESH_PLATFORM_IDS_H

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace paramesh {

inline constexpr std::size_t kPageSize = 4096;
inline constexpr std::size_t kPagesPerSegment = 512;
inline constexpr std::uint64_t kSegmentSize = kPageSize * kPagesPerSegment;  // 2 MiB
inline constexpr std::uint64_t kRegionBase = 0x600000000000ULL;
inline constexpr std::uint64_t kRegionMaxBytes = 4ULL << 30U;
inline constexpr std::size_t kMaxNodes = 8;   // nodes in one job
inline constexpr std::size_t kMaxSlots = 64;  // bits in a copyset

// Each ID is its own type, so a page number cannot be passed where a node number is meant.
struct NodeId {
    std::uint16_t value = 0;
    friend auto operator<=>(const NodeId&, const NodeId&) = default;
};
inline constexpr NodeId kNoNode{0};
inline constexpr NodeId kAllNodes{0xFFFF};

// (address - kRegionBase) / kPageSize.
struct PageId {
    std::uint64_t value = 0;
    friend auto operator<=>(const PageId&, const PageId&) = default;
};

struct SegmentId {
    std::uint32_t value = 0;
    friend auto operator<=>(const SegmentId&, const SegmentId&) = default;
};

// Matches a reply to its request. 0 means "none".
struct ReqId {
    std::uint64_t value = 0;
    friend auto operator<=>(const ReqId&, const ReqId&) = default;
};
inline constexpr ReqId kNoReq{0};

struct JobId {
    std::uint32_t value = 0;
    friend auto operator<=>(const JobId&, const JobId&) = default;
};

// Version of the segment map.
struct Epoch {
    std::uint32_t value = 0;
    friend auto operator<=>(const Epoch&, const Epoch&) = default;
};

// A member's bit number in every copyset of a job, 0 to kMaxSlots - 1.
struct Slot {
    std::uint8_t value = 0;
    friend auto operator<=>(const Slot&, const Slot&) = default;
};

constexpr SegmentId segment_of(PageId page) noexcept {
    return SegmentId{static_cast<std::uint32_t>(page.value / kPagesPerSegment)};
}
constexpr std::uint64_t address_of(PageId page) noexcept {
    return kRegionBase + page.value * kPageSize;
}
// address must lie inside the region.
constexpr PageId page_of(std::uint64_t address) noexcept {
    return PageId{(address - kRegionBase) / kPageSize};
}

// One page of bytes, borrowed from the caller for the length of the call.
using PageView = std::span<const std::byte, kPageSize>;
using PageBuffer = std::span<std::byte, kPageSize>;

// A point in time or a duration, in nanoseconds on one monotonic clock. Only differences mean
// anything. Code in src/coh/ never reads a clock; it is handed these.
using Nanos = std::chrono::nanoseconds;

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_IDS_H
