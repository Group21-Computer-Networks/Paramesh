#include "lib/prefetch.h"

#include <algorithm>

namespace paramesh {

Prefetcher::Prefetcher(std::uint32_t depth, std::uint32_t streams)
    : depth_{depth}, streams_max_{std::max<std::size_t>(1, streams)} {}

std::vector<std::uint64_t> Prefetcher::on_fault(std::uint64_t page) {
    if (depth_ == 0) {
        return {};
    }
    const auto p = static_cast<std::int64_t>(page);
    std::vector<std::uint64_t> fetch;
    // A fault the stream led to: on its stride, past its last fault and at most one stride past
    // what it asked for. The pages before it were read, so the stream runs further ahead.
    const auto in_stream = [p](const Stream& s) {
        const std::int64_t steps = (p - s.last) / s.stride;
        return (p - s.last) % s.stride == 0 && steps >= 1 &&
               steps <= (s.end - s.last) / s.stride + 1;
    };
    if (const auto found = std::find_if(streams_.begin(), streams_.end(), in_stream);
        found != streams_.end()) {
        Stream s = *found;
        streams_.erase(found);
        s.depth = std::min(depth_, 2 * s.depth);
        fetch = run_ahead(s, p);
        streams_.push_back(s);
    } else {
        // A new stream: two earlier faults one stride apart, and this one a stride on.
        for (auto r = recent_.rbegin(); r != recent_.rend(); ++r) {
            const std::int64_t stride = p - *r;
            if (stride != 0 &&
                std::find(recent_.begin(), recent_.end(), *r - stride) != recent_.end()) {
                if (streams_.size() == streams_max_) {
                    streams_.erase(streams_.begin());
                }
                Stream s{p, stride, p, std::min<std::uint32_t>(depth_, 2)};
                fetch = run_ahead(s, p);
                streams_.push_back(s);
                break;
            }
        }
    }
    recent_.push_back(p);
    if (recent_.size() > 4 * streams_max_) {  // room for each stream's last few faults
        recent_.pop_front();
    }
    return fetch;
}

// The pages from past what `s` asked for up to `depth` strides beyond `page`, which becomes its
// newest fault.
std::vector<std::uint64_t> Prefetcher::run_ahead(Stream& s, std::int64_t page) {
    std::vector<std::uint64_t> fetch;
    const std::int64_t past = s.stride > 0 ? std::max(s.end, page) : std::min(s.end, page);
    const std::int64_t end = page + s.stride * s.depth;
    for (std::int64_t next = past + s.stride; s.stride > 0 ? next <= end : next >= end;
         next += s.stride) {
        if (next < 0) {
            break;
        }
        fetch.push_back(static_cast<std::uint64_t>(next));
    }
    s.last = page;
    s.end = end;
    return fetch;
}

}  // namespace paramesh
