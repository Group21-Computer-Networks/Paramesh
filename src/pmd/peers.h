// The peers a daemon hears from: docs/PROTOCOL.md, section 5. Pure: the caller hands in each
// beacon and the time, so the rules are tested with no socket and no clock.
#ifndef PARAMESH_PMD_PEERS_H
#define PARAMESH_PMD_PEERS_H

#include "platform/ids.h"
#include "wire/payloads.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace paramesh {

inline constexpr Nanos kBeaconEvery = std::chrono::seconds{1};  // fixed by the design
inline constexpr int kSuspectAfter = 3;                         // missed beacons
inline constexpr int kRemoveAfter = 5;

struct Peer {
    NodeId node;
    std::uint32_t address = 0;  // IPv4 of its beacons' source, host order
    HelloPayload hello;         // the last beacon
    Nanos heard{};              // when it came
};

class PeerTable {
public:
    PeerTable(NodeId self, std::uint32_t instance) : self_(self), instance_(instance) {}

    enum class Heard : std::uint8_t {
        kOwn,    // this daemon's own beacon, come back
        kNew,    // a node not in the table
        kKnown,  // a node in the table, now refreshed (or restarted at the same address)
        kClash,  // another daemon uses a node ID this table already has, or this daemon's own
    };
    Heard hello(NodeId from, std::uint32_t address, const HelloPayload& hello, Nanos now);
    // BYE: the peer leaves the table now. Ignored unless the instance is the one known.
    void bye(NodeId from, std::uint32_t instance);
    // Takes out the peers silent for kRemoveAfter beacons and returns them.
    std::vector<Peer> expire(Nanos now);

    [[nodiscard]] static bool suspect(const Peer& peer, Nanos now) noexcept {
        return now - peer.heard > kSuspectAfter * kBeaconEvery;
    }
    [[nodiscard]] const std::vector<Peer>& peers() const noexcept { return peers_; }

private:
    NodeId self_;
    std::uint32_t instance_;
    std::vector<Peer> peers_;  // ponytail: a vector; a pool has a handful of nodes
};

}  // namespace paramesh

#endif  // PARAMESH_PMD_PEERS_H
