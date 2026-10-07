// src/rt/: the bump allocator behind pm_malloc.

#include "rt/region_allocator.h"

#include <doctest/doctest.h>

TEST_CASE(
    "small allocations are 16-byte aligned; a page or more is page-aligned and owns its pages") {
    paramesh::RegionAllocator region{paramesh::kSegmentSize};
    CHECK(region.allocate(1) == 0U);
    CHECK(region.allocate(24) == 16U);
    CHECK(region.allocate(8) == 48U);  // 16 + 24 = 40, rounded up to 48

    CHECK(region.allocate(4096) == 4096U);  // the next page boundary
    CHECK(region.allocate(5000) == 8192U);  // takes two pages
    CHECK(region.allocate(8) == 16384U);    // does not share them
    CHECK(region.allocate(4096) == 20480U);
}

TEST_CASE("the allocator refuses zero bytes and anything past the end of the region") {
    paramesh::RegionAllocator region{2 * paramesh::kPageSize};
    CHECK_FALSE(region.allocate(0).has_value());
    CHECK_FALSE(region.allocate(3 * paramesh::kPageSize).has_value());
    CHECK(region.allocate(paramesh::kPageSize) == 0U);
    CHECK(region.allocate(paramesh::kPageSize) == paramesh::kPageSize);
    CHECK_FALSE(region.allocate(1).has_value());      // full
    CHECK_FALSE(region.allocate(~0ULL).has_value());  // no overflow
}
