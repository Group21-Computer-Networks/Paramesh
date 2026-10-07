// The job data plane: one TCP connection per pair of job processes, frames in and out,
// heartbeats, the reply timer, and timers for others.
//
// Interface file drafted by M0-7; see docs/INTERNAL_API.md, section 7. Implemented by M1-2.
#ifndef PARAMESH_NET_TRANSPORT_H
#define PARAMESH_NET_TRANSPORT_H

#include "platform/ids.h"
#include "platform/result.h"
#include "wire/frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace paramesh {

struct Endpoint {
    std::uint32_t ipv4 = 0;  // host byte order
    std::uint16_t port = 0;
};

// What this process says about itself in JOIN_JOB, and what a peer said.
struct JoinInfo {
    NodeId node;
    std::uint8_t role = 0;  // 1 launcher, 2 worker
    std::uint16_t listen_port = 0;
    std::uint32_t pid = 0;
    std::array<std::byte, 32> binary_hash{};
};

struct TimerId {
    std::uint64_t value = 0;
    friend auto operator<=>(const TimerId&, const TimerId&) = default;
};

// Whether a sent frame is a request that the 2 s reply timer covers
// (docs/PROTOCOL.md, section 11).
enum class ReplyTimer : std::uint8_t { kNone, kTimed };

// The receive callbacks. Implemented by src/lib/, and by src/rt/ tests with a fake. Every
// call is made on the network thread and must not block.
class NetHandler {
public:
    NetHandler() = default;
    NetHandler(const NetHandler&) = delete;
    NetHandler& operator=(const NetHandler&) = delete;
    NetHandler(NetHandler&&) = delete;
    NetHandler& operator=(NetHandler&&) = delete;
    virtual ~NetHandler() = default;

    // A connection to the peer is up and its JOIN_JOB has arrived.
    virtual void on_peer_joined(const JoinInfo& peer) noexcept = 0;

    // A whole frame arrived and passed the checks of docs/PROTOCOL.md, section 3. `payload`
    // is valid only during the call. Frames from one peer are delivered in the order sent.
    // HEARTBEAT and JOIN_JOB are handled by the transport and are not delivered.
    virtual void on_frame(const FrameHeader& header,
                          std::span<const std::byte> payload) noexcept = 0;

    // The peer is gone: 3 s of silence, a reset, an unexpected close, or a malformed frame
    // (Errc::kProtocol). Not called for a peer removed with remove_peer().
    virtual void on_peer_lost(NodeId peer, Errc why) noexcept = 0;

    // A timed request got no reply within 2 s, was sent again with RETRY, and got none in
    // 2 s more.
    virtual void on_reply_timeout(NodeId peer, ReqId req) noexcept = 0;

    // A timer started with start_timer() fired.
    virtual void on_timer(TimerId timer, Nanos now) noexcept = 0;
};

// The HLD's net_send() is send().
class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    // Starts accepting connections and returns the port chosen when `local.port` is 0.
    virtual Result<std::uint16_t> listen(Endpoint local) = 0;

    // Makes the peer known. If this node's ID is the lower, connects to it; otherwise waits
    // for it to connect. on_peer_joined follows.
    virtual Result<void> add_peer(NodeId peer, Endpoint remote) = 0;

    // Closes the connection to a peer that has left in order. Not reported as lost.
    virtual void remove_peer(NodeId peer) = 0;

    // Queues one frame for the peer and returns; may be called from any thread. The transport
    // fills in the header's job, src, payload_len and payload_crc. Frames to one peer leave in
    // the order send() was called. A reply is recognised by its req: it stops the reply timer
    // of the request with that number sent to that peer.
    // Errc::kNotFound for an unknown peer, Errc::kInvalidArgument for a payload over
    // kMaxPayload.
    virtual Result<void> send(NodeId to, const FrameHeader& header,
                              std::span<const std::byte> payload, ReplyTimer timer) = 0;

    // One-shot timer; on_timer follows after `delay`. May be called from any thread.
    virtual TimerId start_timer(Nanos delay) = 0;
    virtual void cancel_timer(TimerId timer) = 0;

    // The time on the clock that on_timer reports.
    [[nodiscard]] virtual Nanos now() const noexcept = 0;

    // Runs the network thread's loop on the calling thread until stop(). It waits only on
    // epoll.
    virtual Result<void> run() = 0;
    // May be called from any thread.
    virtual void stop() noexcept = 0;
};

struct TransportConfig {
    JobId job;
    JoinInfo self;
};

// `checksum` and `handler` must outlive the transport.
Result<std::unique_ptr<Transport>> net_open(const TransportConfig& config, const Checksum& checksum,
                                            NetHandler& handler);

}  // namespace paramesh

#endif  // PARAMESH_NET_TRANSPORT_H
