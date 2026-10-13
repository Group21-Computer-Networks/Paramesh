#include "pmd/peers.h"

#include <algorithm>

namespace paramesh {

PeerTable::Heard PeerTable::hello(NodeId from, std::uint32_t address, const HelloPayload& hello,
                                  Nanos now) {
    if (from == self_) {
        return hello.instance == instance_ ? Heard::kOwn : Heard::kClash;
    }
    const auto known = std::find_if(peers_.begin(), peers_.end(),
                                    [from](const Peer& p) { return p.node == from; });
    if (known == peers_.end()) {
        peers_.push_back(Peer{from, address, hello, now});
        return Heard::kNew;
    }
    // One node ID, another instance, another address: two daemons with one ID. The first one
    // heard keeps it. The same address is the same machine, restarted.
    if (known->hello.instance != hello.instance && known->address != address) {
        return Heard::kClash;
    }
    *known = Peer{from, address, hello, now};
    return Heard::kKnown;
}

void PeerTable::bye(NodeId from, std::uint32_t instance) {
    std::erase_if(peers_,
                  [&](const Peer& p) { return p.node == from && p.hello.instance == instance; });
}

std::vector<Peer> PeerTable::expire(Nanos now) {
    std::vector<Peer> gone;
    std::erase_if(peers_, [&](const Peer& p) {
        const bool silent = now - p.heard > kRemoveAfter * kBeaconEvery;
        if (silent) {
            gone.push_back(p);
        }
        return silent;
    });
    return gone;
}

}  // namespace paramesh
