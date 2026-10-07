// Transport over epoll and non-blocking TCP (docs/INTERNAL_API.md, section 7; docs/PROTOCOL.md,
// sections 2, 3 and 11).
//
// One thread, the caller of run(), does all socket work and makes every NetHandler call. It
// waits only in epoll_wait. Other threads reach it through send(), the timer calls and stop(),
// which put work in mutex-guarded queues and wake it with an eventfd. No lock is held while a
// handler runs, so a handler may call back into the transport.

#include "net/transport.h"
#include "wire/payloads.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace paramesh {
namespace {

using Bytes = std::vector<std::byte>;

// docs/PROTOCOL.md, section 11.
constexpr Nanos kHeartbeatEvery = std::chrono::seconds{1};
constexpr Nanos kSilenceLimit = std::chrono::seconds{3};
constexpr Nanos kReplyTimeout = std::chrono::seconds{2};
constexpr Nanos kConnectRetry = std::chrono::milliseconds{100};
// ponytail: time-based work is checked at least this often; a timer heap if that costs too much.
constexpr Nanos kTick = std::chrono::milliseconds{50};
constexpr std::size_t kReadChunk = 64UL * 1024;
constexpr std::size_t kFlagsOffset = 6;

Nanos steady_now() noexcept {
    return std::chrono::steady_clock::now().time_since_epoch();
}

// The opcodes docs/PROTOCOL.md, section 4, marks "is a reply": only these stop a reply timer.
constexpr bool is_reply(Opcode op) noexcept {
    switch (op) {
        case Opcode::kSegMapAck:
        case Opcode::kReadData:
        case Opcode::kWriteGrant:
        case Opcode::kUpgradeGrant:
        case Opcode::kRedirect:
        case Opcode::kBusyRetry:
        case Opcode::kInvAck:
        case Opcode::kFetchData:
        case Opcode::kWritebackAck:
        case Opcode::kTaskAssign:
        case Opcode::kNoTask:
        case Opcode::kLockGrant:
        case Opcode::kBarrierRelease:
        case Opcode::kAtomicResult:
        case Opcode::kSegMigrateDone:
        case Opcode::kJobEnd:
            return true;
        default:
            return false;
    }
}

// Owns a file descriptor. ponytail: the same few lines as in src/mem/; one shared header in
// src/platform/ when a third user appears.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

struct Conn {
    Fd fd;
    NodeId peer;               // kNoNode until an accepted connection's JOIN_JOB arrives
    bool connecting = false;   // a connect() is in progress
    bool joined = false;       // the peer's JOIN_JOB has arrived
    Bytes in;                  // received bytes not yet made into frames
    std::deque<Bytes> out;     // whole frames waiting to be written
    std::size_t out_done = 0;  // bytes of out.front() already written
    Nanos heard{};             // when the peer was last heard from
    Nanos beat{};              // when the last heartbeat was sent
};

struct Pending {
    Nanos deadline{};
    bool retried = false;
    Bytes frame;
};

sockaddr_in address_of(Endpoint endpoint) noexcept {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(endpoint.ipv4);
    addr.sin_port = htons(endpoint.port);
    return addr;
}

class EpollTransport final : public Transport {
public:
    EpollTransport(const TransportConfig& config, const Checksum& checksum, NetHandler& handler,
                   Fd epoll, Fd wake) noexcept
        : config_(config),
          checksum_(&checksum),
          handler_(&handler),
          epoll_(std::move(epoll)),
          wake_(std::move(wake)) {}

    Result<std::uint16_t> listen(Endpoint local) override {
        Fd fd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
        const int on = 1;
        sockaddr_in addr = address_of(local);
        socklen_t len = sizeof addr;
        if (!fd.valid() || ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
            ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(fd.get(), SOMAXCONN) != 0 ||
            ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
            !watch(fd.get(), EPOLLIN)) {
            return Error{Errc::kIo, errno, "listen for data-plane connections"};
        }
        listener_ = std::move(fd);
        return ntohs(addr.sin_port);
    }

    Result<void> add_peer(NodeId peer, Endpoint remote) override {
        if (peer == kNoNode || peer == kAllNodes || peer == config_.self.node) {
            return Error{Errc::kInvalidArgument, 0, "add a peer: not a peer's node ID"};
        }
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            known_[peer.value] = remote;
        }
        wake();
        return {};
    }

    void remove_peer(NodeId peer) override {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            known_.erase(peer.value);
            outbox_.erase(peer.value);
            removed_.push_back(peer);
            std::erase_if(pending_,
                          [peer](const auto& entry) { return entry.first.first == peer.value; });
        }
        wake();
    }

    Result<void> send(NodeId to, const FrameHeader& header, std::span<const std::byte> payload,
                      ReplyTimer timer) override {
        if (payload.size() > kMaxPayload) {
            return Error{Errc::kInvalidArgument, 0, "send a frame: payload over the limit"};
        }
        Bytes frame = make_frame(header, to, payload);
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (!known_.contains(to.value)) {
                return Error{Errc::kNotFound, 0, "send a frame: unknown peer"};
            }
            if (timer == ReplyTimer::kTimed) {
                pending_[{to.value, header.req.value}] =
                    Pending{steady_now() + kReplyTimeout, false, frame};
            }
            outbox_[to.value].push_back(std::move(frame));
        }
        wake();
        return {};
    }

    TimerId start_timer(Nanos delay) override {
        TimerId id;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            id = TimerId{++last_timer_};
            timers_.emplace(steady_now() + delay, id);
        }
        wake();
        return id;
    }

    void cancel_timer(TimerId timer) override {
        const std::lock_guard<std::mutex> lock{mutex_};
        std::erase_if(timers_, [timer](const auto& entry) { return entry.second == timer; });
    }

    [[nodiscard]] Nanos now() const noexcept override { return steady_now(); }

    Result<void> run() override {
        std::array<epoll_event, 64> events{};
        for (;;) {
            const int n = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()),
                                       wait_ms());
            if (n < 0 && errno != EINTR) {
                return Error{Errc::kIo, errno, "wait for network events"};
            }
            for (int i = 0; i < std::max(n, 0); i++) {
                on_event(events.at(static_cast<std::size_t>(i)));
            }
            if (!housekeeping()) {
                return {};
            }
        }
    }

    void stop() noexcept override {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            stopping_ = true;
        }
        wake();
    }

private:
    // ---- any thread ------------------------------------------------------------------

    void wake() const noexcept {
        const std::uint64_t one = 1;
        const ssize_t written = ::write(wake_.get(), &one, sizeof one);
        static_cast<void>(written);
    }

    [[nodiscard]] Bytes make_frame(FrameHeader header, NodeId to,
                                   std::span<const std::byte> payload) const {
        header.job = config_.job;
        header.src = config_.self.node;
        header.dst = to;
        header.payload_len = static_cast<std::uint32_t>(payload.size());
        header.payload_crc = wire_payload_crc(*checksum_, payload);
        Bytes frame(kFrameHeaderSize + payload.size());
        wire_encode_header(header,
                           std::span<std::byte, kFrameHeaderSize>{frame.data(), kFrameHeaderSize});
        std::copy(payload.begin(), payload.end(), frame.begin() + kFrameHeaderSize);
        return frame;
    }

    // ---- network thread --------------------------------------------------------------

    bool watch(int fd, std::uint32_t events) const noexcept {
        epoll_event event{};
        event.events = events;
        event.data.fd = fd;
        return ::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &event) == 0 ||
               (errno == EEXIST && ::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, fd, &event) == 0);
    }

    // How long epoll_wait may sleep: to the next tick or the next timer, whichever is first.
    int wait_ms() {
        const std::lock_guard<std::mutex> lock{mutex_};
        Nanos wait = kTick;
        if (!timers_.empty()) {
            wait = std::clamp(timers_.begin()->first - steady_now(), Nanos{0}, kTick);
        }
        return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(wait).count());
    }

    Conn* conn_of(NodeId peer) {
        const auto found = peer_fd_.find(peer.value);
        return found == peer_fd_.end() ? nullptr : &conns_.at(found->second);
    }

    void on_event(const epoll_event& event) {
        const int fd = event.data.fd;
        if (fd == wake_.get()) {
            std::uint64_t count = 0;
            const ssize_t got = ::read(fd, &count, sizeof count);
            static_cast<void>(got);
            return;
        }
        if (fd == listener_.get()) {
            accept_all();
            return;
        }
        const auto found = conns_.find(fd);
        if (found == conns_.end()) {
            return;  // closed earlier in this batch of events
        }
        Conn& conn = found->second;
        if (conn.connecting) {
            finish_connect(conn);
            return;
        }
        if ((event.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0 && !read_from(conn)) {
            return;  // the connection was dropped
        }
        if ((event.events & EPOLLOUT) != 0) {
            flush(conn);
        }
    }

    void adopt(Fd fd, NodeId peer, bool connecting) {
        const int on = 1;
        ::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        const int raw = fd.get();
        Conn& conn = conns_[raw];
        conn.fd = std::move(fd);
        conn.peer = peer;
        conn.connecting = connecting;
        conn.heard = conn.beat = steady_now();
        if (peer != kNoNode) {
            peer_fd_[peer.value] = raw;
        }
        if (!connecting) {
            greet(conn);
        }
        watch(raw, connecting ? EPOLLOUT : EPOLLIN);
    }

    // JOIN_JOB is the first frame each side sends on a connection.
    void greet(Conn& conn) {
        JoinJobPayload join;
        join.role = config_.self.role;
        join.listen_port = config_.self.listen_port;
        join.pid = config_.self.pid;
        join.binary_hash = config_.self.binary_hash;
        std::array<std::byte, 40> payload{};
        const Result<std::size_t> size = wire_encode(join, payload);
        FrameHeader header;
        header.opcode = Opcode::kJoinJob;
        conn.out.push_back(
            make_frame(header, conn.peer, std::span{payload}.first(size.ok() ? size.value() : 0)));
        flush(conn);
    }

    void accept_all() {
        for (;;) {
            Fd fd{::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
            if (!fd.valid()) {
                return;
            }
            adopt(std::move(fd), kNoNode, false);
        }
    }

    void start_connect(NodeId peer, Endpoint remote) {
        Fd fd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
        const sockaddr_in addr = address_of(remote);
        if (!fd.valid()) {
            return;
        }
        const int rc = ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
        if (rc != 0 && errno != EINPROGRESS) {
            return;  // tried again after kConnectRetry
        }
        adopt(std::move(fd), peer, rc != 0);
    }

    void finish_connect(Conn& conn) {
        int error = 0;
        socklen_t len = sizeof error;
        ::getsockopt(conn.fd.get(), SOL_SOCKET, SO_ERROR, &error, &len);
        if (error != 0) {
            drop(conn, false, Errc::kIo);  // the peer is not listening yet; retried
            return;
        }
        conn.connecting = false;
        conn.heard = conn.beat = steady_now();
        greet(conn);
        watch(conn.fd.get(), EPOLLIN);
    }

    // Closes a connection. If the peer had joined and `report` is set, the handler is told.
    void drop(Conn& conn, bool report, Errc why) {
        const NodeId peer = conn.peer;
        const bool tell = report && conn.joined;
        const int fd = conn.fd.get();
        if (peer != kNoNode && peer_fd_.contains(peer.value) && peer_fd_.at(peer.value) == fd) {
            peer_fd_.erase(peer.value);
        }
        conns_.erase(fd);  // closes the socket, which also removes it from epoll
        if (tell) {
            {
                const std::lock_guard<std::mutex> lock{mutex_};
                known_.erase(peer.value);
                outbox_.erase(peer.value);
                std::erase_if(pending_, [peer](const auto& entry) {
                    return entry.first.first == peer.value;
                });
            }
            handler_->on_peer_lost(peer, why);
        }
    }

    // Reads what has arrived and delivers every whole frame. False if the connection was dropped.
    bool read_from(Conn& conn) {
        // A bounded number of reads per event, so one busy peer cannot grow the buffer without end;
        // epoll reports the socket again if more is waiting.
        for (int reads = 0; reads < 32; reads++) {
            const std::size_t have = conn.in.size();
            conn.in.resize(have + kReadChunk);
            const ssize_t got = ::read(conn.fd.get(), conn.in.data() + have, kReadChunk);
            conn.in.resize(have + static_cast<std::size_t>(std::max<ssize_t>(got, 0)));
            if (got > 0) {
                continue;
            }
            if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
                break;
            }
            drop(conn, true, Errc::kClosed);  // end of stream, a reset, or another error
            return false;
        }
        conn.heard = steady_now();
        return deliver(conn);
    }

    // Takes whole frames off the front of conn.in. False if the connection was dropped.
    bool deliver(Conn& conn) {
        const int fd = conn.fd.get();
        std::size_t at = 0;
        while (conn.in.size() - at >= kFrameHeaderSize) {
            const std::span<const std::byte> rest{conn.in.data() + at, conn.in.size() - at};
            const Result<FrameHeader> header = wire_decode_header(rest.first<kFrameHeaderSize>());
            if (!header.ok()) {
                drop(conn, true, Errc::kProtocol);
                return false;
            }
            const std::size_t whole = kFrameHeaderSize + header.value().payload_len;
            if (rest.size() < whole) {
                break;
            }
            const std::span<const std::byte> payload =
                rest.subspan(kFrameHeaderSize, header.value().payload_len);
            if (!accept_frame(conn, header.value(), payload)) {
                drop(conn, true, Errc::kProtocol);
                return false;
            }
            at += whole;
            if (!conns_.contains(fd)) {
                return false;  // a handler removed this peer
            }
        }
        conn.in.erase(conn.in.begin(), conn.in.begin() + static_cast<std::ptrdiff_t>(at));
        return true;
    }

    // Checks one frame and hands it on. False if it breaks the protocol.
    bool accept_frame(Conn& conn, const FrameHeader& header, std::span<const std::byte> payload) {
        if (!wire_check_payload(*checksum_, header, payload).ok() || header.job != config_.job ||
            (header.dst != config_.self.node && header.dst != kNoNode)) {
            return false;
        }
        if (!conn.joined) {
            const Result<JoinJobPayload> join = wire_decode_join_job(payload);
            // An accepted connection must come from a lower node ID we hold no connection to.
            const bool expected = conn.peer == kNoNode
                                      ? header.src.value < config_.self.node.value &&
                                            !peer_fd_.contains(header.src.value)
                                      : header.src == conn.peer;
            if (header.opcode != Opcode::kJoinJob || !join.ok() || !expected ||
                header.src == kNoNode) {
                return false;
            }
            conn.peer = header.src;
            conn.joined = true;
            peer_fd_[conn.peer.value] = conn.fd.get();
            {
                const std::lock_guard<std::mutex> lock{mutex_};
                known_.try_emplace(conn.peer.value);
            }
            handler_->on_peer_joined(JoinInfo{conn.peer, join.value().role,
                                              join.value().listen_port, join.value().pid,
                                              join.value().binary_hash});
            return true;
        }
        if (header.src != conn.peer) {
            return false;
        }
        if (is_reply(header.opcode)) {
            const std::lock_guard<std::mutex> lock{mutex_};
            pending_.erase({conn.peer.value, header.req.value});
        }
        if (header.opcode != Opcode::kHeartbeat && header.opcode != Opcode::kJoinJob) {
            handler_->on_frame(header, payload);
        }
        return true;
    }

    // Writes as much of conn.out as the socket takes, header and payload of a frame together.
    void flush(Conn& conn) {
        while (!conn.out.empty()) {
            std::array<iovec, 16> vec{};
            std::size_t count = 0;
            for (auto it = conn.out.begin(); it != conn.out.end() && count < vec.size();
                 ++it, ++count) {
                const std::size_t skip = count == 0 ? conn.out_done : 0;
                vec.at(count).iov_base = it->data() + skip;
                vec.at(count).iov_len = it->size() - skip;
            }
            ssize_t wrote = ::writev(conn.fd.get(), vec.data(), static_cast<int>(count));
            if (wrote < 0) {
                break;  // would block, or an error that the next read reports
            }
            while (wrote > 0) {
                const std::size_t left = conn.out.front().size() - conn.out_done;
                if (static_cast<std::size_t>(wrote) < left) {
                    conn.out_done += static_cast<std::size_t>(wrote);
                    break;
                }
                wrote -= static_cast<ssize_t>(left);
                conn.out.pop_front();
                conn.out_done = 0;
            }
        }
        watch(conn.fd.get(), conn.out.empty() ? EPOLLIN : EPOLLIN | EPOLLOUT);
    }

    // Everything driven by time or by other threads. False when stop() was called.
    bool housekeeping() {
        const Nanos now = steady_now();
        std::vector<NodeId> removed;
        std::vector<std::pair<NodeId, Endpoint>> to_connect;
        std::vector<std::pair<NodeId, ReqId>> timed_out;
        std::vector<TimerId> fired;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (stopping_) {
                return false;
            }
            removed.swap(removed_);
            for (const auto& [peer, remote] : known_) {
                if (config_.self.node.value < peer && !peer_fd_.contains(peer) &&
                    now >= retry_at_[peer]) {
                    to_connect.emplace_back(NodeId{peer}, remote);
                    retry_at_[peer] = now + kConnectRetry;
                }
            }
            // Frames queued by send() move to their connection once the peer has joined.
            for (auto& [peer, frames] : outbox_) {
                Conn* conn = conn_of(NodeId{peer});
                if (conn != nullptr && conn->joined) {
                    std::move(frames.begin(), frames.end(), std::back_inserter(conn->out));
                    frames.clear();
                }
            }
            for (auto it = pending_.begin(); it != pending_.end();) {
                Pending& request = it->second;
                if (now < request.deadline) {
                    ++it;
                } else if (!request.retried) {
                    request.retried = true;
                    request.deadline = now + kReplyTimeout;
                    request.frame.at(kFlagsOffset + 1) |= static_cast<std::byte>(kFlagRetry);
                    outbox_[it->first.first].push_front(request.frame);
                    ++it;
                } else {
                    timed_out.emplace_back(NodeId{it->first.first}, ReqId{it->first.second});
                    it = pending_.erase(it);
                }
            }
            while (!timers_.empty() && timers_.begin()->first <= now) {
                fired.push_back(timers_.begin()->second);
                timers_.erase(timers_.begin());
            }
        }

        for (const NodeId peer : removed) {
            if (Conn* conn = conn_of(peer)) {
                drop(*conn, false, Errc::kClosed);
            }
        }
        for (const auto& [peer, remote] : to_connect) {
            start_connect(peer, remote);
        }
        std::vector<int> silent;
        for (auto& [fd, conn] : conns_) {
            if (conn.connecting) {
                continue;
            }
            if (now - conn.heard >= kSilenceLimit) {
                silent.push_back(fd);
                continue;
            }
            if (conn.joined && now - conn.beat >= kHeartbeatEvery) {
                FrameHeader beat;
                beat.opcode = Opcode::kHeartbeat;
                conn.out.push_back(make_frame(beat, conn.peer, {}));
                conn.beat = now;
            }
            flush(conn);
        }
        for (const int fd : silent) {
            if (conns_.contains(fd)) {
                drop(conns_.at(fd), true, Errc::kClosed);
            }
        }
        for (const auto& [peer, req] : timed_out) {
            handler_->on_reply_timeout(peer, req);
        }
        for (const TimerId timer : fired) {
            handler_->on_timer(timer, now);
        }
        return true;
    }

    TransportConfig config_;
    const Checksum* checksum_;
    NetHandler* handler_;
    Fd epoll_;
    Fd wake_;
    Fd listener_;

    // Network thread only.
    std::unordered_map<int, Conn> conns_;
    std::unordered_map<std::uint16_t, int> peer_fd_;
    std::unordered_map<std::uint16_t, Nanos> retry_at_;

    // Shared with other threads.
    std::mutex mutex_;
    std::unordered_map<std::uint16_t, Endpoint> known_;
    std::unordered_map<std::uint16_t, std::deque<Bytes>> outbox_;
    std::vector<NodeId> removed_;
    std::map<std::pair<std::uint16_t, std::uint64_t>, Pending> pending_;
    std::multimap<Nanos, TimerId> timers_;
    std::uint64_t last_timer_ = 0;
    bool stopping_ = false;
};

}  // namespace

Result<std::unique_ptr<Transport>> net_open(const TransportConfig& config, const Checksum& checksum,
                                            NetHandler& handler) {
    Fd epoll{::epoll_create1(EPOLL_CLOEXEC)};
    Fd wake{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = wake.get();
    if (!epoll.valid() || !wake.valid() ||
        ::epoll_ctl(epoll.get(), EPOLL_CTL_ADD, wake.get(), &event) != 0) {
        return Error{Errc::kIo, errno, "open the transport"};
    }
    return std::unique_ptr<Transport>{std::make_unique<EpollTransport>(
        config, checksum, handler, std::move(epoll), std::move(wake))};
}

}  // namespace paramesh
