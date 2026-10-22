// Predictive page prefetching (X-prefetch): from the read faults it is told of, guesses the
// pages the application reads next, so that they are fetched before it faults on them.
// Pure: no clock, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace paramesh {

class Prefetcher {
public:
    // `depth`: how many pages at most a stream runs ahead (prefetch.depth; 0 guesses nothing).
    // `streams`: how many strides are followed at once (prefetch.streams).
    Prefetcher(std::uint32_t depth, std::uint32_t streams);

    // A read fault on `page`: the pages to fetch, nearest first. A stream starts after three
    // faults one stride apart; each fault inside what it fetched doubles how far it runs ahead.
    std::vector<std::uint64_t> on_fault(std::uint64_t page);

private:
    struct Stream {
        std::int64_t last;    // the newest fault in it
        std::int64_t stride;  // pages between two of its faults, never 0
        std::int64_t end;     // the furthest page asked for
        std::uint32_t depth;  // strides it runs ahead
    };
    static std::vector<std::uint64_t> run_ahead(Stream& s, std::int64_t page);

    std::uint32_t depth_;
    std::size_t streams_max_;
    std::vector<Stream> streams_;      // least recently used first
    std::deque<std::int64_t> recent_;  // the last faults, newest last
};

}  // namespace paramesh
