// src/net/: the epoll transport against docs/INTERNAL_API.md section 7, with real sockets on
// localhost. The first test is the card's Done-when and uses a second process.

#include "net/transport.h"
#include "platform/factory.h"
#include "wire/payloads.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using paramesh::Errc;
using paramesh::FrameHeader;
using paramesh::NodeId;
using paramesh::Opcode;
using paramesh::ReplyTimer;
using paramesh::ReqId;
using Bytes = std::vector<std::byte>;
using Steady = std::chrono::steady_clock;

constexpr std::uint32_t kLocalhost = 0x7F000001;
constexpr paramesh::JobId kJob{7};

struct Received {
    FrameHeader header;
    Bytes payload;
};
struct Lost {
    NodeId peer;
    Errc why;
    Steady::time_point when;
};

// Records every callback so the test thread can wait for it. It only queues, as a handler must.
class Recorder : public paramesh::NetHandler {
public:
    void on_peer_joined(const paramesh::JoinInfo& peer) noexcept override {
        push(joined_, peer.node);
    }
    void on_frame(const FrameHeader& header, std::span<const std::byte> payload) noexcept override {
        push(frames_, Received{header, {payload.begin(), payload.end()}});
    }
    void on_peer_lost(NodeId peer, Errc why) noexcept override {
        push(lost_, Lost{peer, why, Steady::now()});
    }
    void on_reply_timeout(NodeId /*peer*/, ReqId req) noexcept override { push(timeouts_, req); }
    void on_timer(paramesh::TimerId timer, paramesh::Nanos /*now*/) noexcept override {
        push(timers_, timer);
    }

    std::optional<NodeId> joined(std::chrono::milliseconds wait = 5s) { return pop(joined_, wait); }
    std::optional<Received> frame(std::chrono::milliseconds wait = 5s) {
        return pop(frames_, wait);
    }
    std::optional<Lost> lost(std::chrono::milliseconds wait) { return pop(lost_, wait); }
    std::optional<ReqId> timeout(std::chrono::milliseconds wait) { return pop(timeouts_, wait); }
    std::optional<paramesh::TimerId> timer(std::chrono::milliseconds wait) {
        return pop(timers_, wait);
    }

private:
    template <typename T>
    void push(std::deque<T>& queue, T value) {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            queue.push_back(std::move(value));
        }
        changed_.notify_all();
    }
    template <typename T>
    std::optional<T> pop(std::deque<T>& queue, std::chrono::milliseconds wait) {
        std::unique_lock<std::mutex> lock{mutex_};
        if (!changed_.wait_for(lock, wait, [&queue] { return !queue.empty(); })) {
            return std::nullopt;
        }
        T value = std::move(queue.front());
        queue.pop_front();
        return value;
    }
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<NodeId> joined_;
    std::deque<Received> frames_;
    std::deque<Lost> lost_;
    std::deque<ReqId> timeouts_;
    std::deque<paramesh::TimerId> timers_;
};

// One node of a job on localhost: a transport listening on a free port, its loop on a thread.
struct Node {
    paramesh::Platform platform;
    std::unique_ptr<paramesh::Transport> transport;
    std::uint16_t port = 0;
    std::thread loop;

    Node(std::uint16_t id, paramesh::NetHandler& handler) {
        auto opened = paramesh::platform_open({}, {});
        REQUIRE(opened.ok());
        platform = std::move(opened).value();
        paramesh::TransportConfig config;
        config.job = kJob;
        config.self.node = NodeId{id};
        config.self.role = 2;
        auto made = paramesh::net_open(config, *platform.checksum, handler);
        REQUIRE(made.ok());
        transport = std::move(made).value();
        const auto bound = transport->listen({kLocalhost, 0});
        REQUIRE(bound.ok());
        port = bound.value();
        loop = std::thread{[this] { static_cast<void>(transport->run()); }};
    }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) = delete;
    Node& operator=(Node&&) = delete;
    ~Node() {
        transport->stop();
        loop.join();
    }
};

FrameHeader header_of(Opcode opcode, std::uint64_t req) {
    FrameHeader header;
    header.opcode = opcode;
    header.req = ReqId{req};
    return header;
}

// Frame i of the exchange test: mostly up to 2 KiB, every 500th one 100 KB, some empty.
Bytes mixed_payload(std::uint64_t i) {
    Bytes payload(i % 500 == 0 ? 100000 : i * 7919 % 2049);
    for (std::size_t j = 0; j < payload.size(); j++) {
        payload[j] = static_cast<std::byte>(i + j);
    }
    return payload;
}

// The child of the two-process tests: node 2, which sends every frame straight back.
class Echo final : public Recorder {
public:
    void attach(paramesh::Transport& transport) noexcept { transport_ = &transport; }
    void on_frame(const FrameHeader& header, std::span<const std::byte> payload) noexcept override {
        if (header.req.value != ++expected_) {
            ::_exit(3);  // a frame was lost or arrived out of order
        }
        static_cast<void>(transport_->send(header.src, header_of(header.opcode, header.req.value),
                                           payload, ReplyTimer::kNone));
    }

private:
    paramesh::Transport* transport_ = nullptr;
    std::uint64_t expected_ = 0;
};

// Starts the echo process and returns its PID and the port it listens on.
std::pair<pid_t, std::uint16_t> start_echo_process() {
    std::array<int, 2> ends{};
    REQUIRE(::pipe(ends.data()) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        Echo echo;
        Node node{2, echo};
        echo.attach(*node.transport);
        static_cast<void>(node.transport->add_peer(NodeId{1}, {}));
        static_cast<void>(::write(ends[1], &node.port, sizeof node.port));
        for (;;) {
            ::pause();
        }
    }
    ::close(ends[1]);
    std::uint16_t port = 0;
    REQUIRE(::read(ends[0], &port, sizeof port) == static_cast<ssize_t>(sizeof port));
    ::close(ends[0]);
    return {child, port};
}

void reap(pid_t child) {
    ::kill(child, SIGKILL);
    ::kill(child, SIGCONT);
    int status = 0;
    ::waitpid(child, &status, 0);
}

}  // namespace

TEST_CASE(
    "two processes exchange 100,000 frames of mixed size in order; a killed peer is reported "
    "within 3 s") {
    const auto [child, port] = start_echo_process();
    Recorder recorder;
    Node node{1, recorder};
    REQUIRE(node.transport->add_peer(NodeId{2}, {kLocalhost, port}).ok());
    REQUIRE(recorder.joined() == NodeId{2});

    constexpr std::uint64_t kEachWay = 50000;
    for (std::uint64_t i = 1; i <= kEachWay; i++) {
        REQUIRE(node.transport
                    ->send(NodeId{2}, header_of(Opcode::kSegMigrate, i), mixed_payload(i),
                           ReplyTimer::kNone)
                    .ok());
    }
    std::uint64_t wrong = 0;
    for (std::uint64_t i = 1; i <= kEachWay; i++) {
        const auto echo = recorder.frame(30s);
        REQUIRE_MESSAGE(echo.has_value(), "no echo for frame ", i);
        const Received got = echo.value_or(Received{});
        if (got.header.req != ReqId{i} || got.payload != mixed_payload(i)) {
            wrong++;
        }
    }
    CHECK(wrong == 0);  // 50,000 there and 50,000 back: none lost, none out of order, none changed

    const auto killed_at = Steady::now();
    ::kill(child, SIGKILL);
    const auto lost = recorder.lost(5s);
    reap(child);
    REQUIRE(lost.has_value());
    CHECK(lost.value_or(Lost{}).peer == NodeId{2});
    CHECK(lost.value_or(Lost{}).when - killed_at < 3s);
}

TEST_CASE(
    "heartbeats keep an idle connection alive; a peer that goes silent is reported after 3 s") {
    const auto [child, port] = start_echo_process();
    Recorder recorder;
    Node node{1, recorder};
    REQUIRE(node.transport->add_peer(NodeId{2}, {kLocalhost, port}).ok());
    REQUIRE(recorder.joined() == NodeId{2});

    CHECK_FALSE(recorder.lost(3500ms).has_value());  // nothing sent for longer than the limit

    const auto stopped_at = Steady::now();
    ::kill(child, SIGSTOP);  // alive, connection open, but silent
    const auto lost = recorder.lost(6s);
    reap(child);
    REQUIRE(lost.has_value());
    const auto after = lost.value_or(Lost{}).when - stopped_at;
    CHECK(after > 1900ms);  // its last heartbeat may be up to a second old
    CHECK(after < 4500ms);
}

TEST_CASE("two transports join, address frames to each other, and refuse what they cannot send") {
    Recorder first;
    Recorder second;
    Node a{1, first};
    Node b{2, second};
    REQUIRE(a.transport->add_peer(NodeId{2}, {kLocalhost, b.port}).ok());
    REQUIRE(b.transport->add_peer(NodeId{1}, {kLocalhost, a.port}).ok());
    REQUIRE(first.joined() == NodeId{2});
    REQUIRE(second.joined() == NodeId{1});

    const Bytes page_id = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                           std::byte{0}, std::byte{0}, std::byte{1}, std::byte{2}};
    REQUIRE(
        a.transport->send(NodeId{2}, header_of(Opcode::kReadReq, 11), page_id, ReplyTimer::kNone)
            .ok());
    const auto got = second.frame();
    REQUIRE(got.has_value());
    const Received frame = got.value_or(Received{});
    CHECK(frame.header.opcode == Opcode::kReadReq);
    CHECK(frame.header.job == kJob);  // filled in by the transport
    CHECK(frame.header.src == NodeId{1});
    CHECK(frame.header.dst == NodeId{2});
    CHECK(frame.header.req == ReqId{11});
    CHECK(frame.payload == page_id);

    CHECK(a.transport->send(NodeId{9}, header_of(Opcode::kReadReq, 12), page_id, ReplyTimer::kNone)
              .error()
              .code == Errc::kNotFound);
    const Bytes huge(paramesh::kMaxPayload + 1);
    CHECK(a.transport->send(NodeId{2}, header_of(Opcode::kSegMigrate, 13), huge, ReplyTimer::kNone)
              .error()
              .code == Errc::kInvalidArgument);
    CHECK(a.transport->add_peer(NodeId{1}, {}).error().code == Errc::kInvalidArgument);  // itself
}

TEST_CASE(
    "a timed request is sent again with RETRY after 2 s and reported after 4 s; a reply stops the "
    "timer") {
    Recorder first;
    Recorder second;
    Node a{1, first};
    Node b{2, second};
    REQUIRE(a.transport->add_peer(NodeId{2}, {kLocalhost, b.port}).ok());
    REQUIRE(b.transport->add_peer(NodeId{1}, {}).ok());
    REQUIRE(first.joined() == NodeId{2});
    REQUIRE(second.joined() == NodeId{1});

    const auto sent_at = Steady::now();
    REQUIRE(a.transport->send(NodeId{2}, header_of(Opcode::kReadReq, 20), {}, ReplyTimer::kTimed)
                .ok());  // never answered
    REQUIRE(a.transport->send(NodeId{2}, header_of(Opcode::kReadReq, 21), {}, ReplyTimer::kTimed)
                .ok());  // answered below
    REQUIRE(second.frame().has_value());
    REQUIRE(second.frame().has_value());
    REQUIRE(
        b.transport->send(NodeId{1}, header_of(Opcode::kReadData, 21), {}, ReplyTimer::kNone).ok());

    const auto again = second.frame(3500ms);  // the retry of request 20
    REQUIRE(again.has_value());
    CHECK(again.value_or(Received{}).header.req == ReqId{20});
    CHECK((again.value_or(Received{}).header.flags & paramesh::kFlagRetry) != 0);
    CHECK(Steady::now() - sent_at > 1900ms);

    const auto timed_out = first.timeout(3500ms);
    REQUIRE(timed_out.has_value());
    CHECK(timed_out == ReqId{20});
    CHECK(Steady::now() - sent_at > 3900ms);
    CHECK_FALSE(first.timeout(300ms).has_value());  // request 21 was answered
    CHECK_FALSE(second.frame(300ms).has_value());   // and was never sent again
}

TEST_CASE("a timer fires once, about when asked, and a cancelled timer does not fire") {
    Recorder recorder;
    Node node{1, recorder};
    const auto before = Steady::now();
    const paramesh::TimerId cancelled = node.transport->start_timer(30ms);
    const paramesh::TimerId kept = node.transport->start_timer(60ms);
    node.transport->cancel_timer(cancelled);
    CHECK(recorder.timer(2s) == kept);
    CHECK(Steady::now() - before >= 60ms);
    CHECK_FALSE(recorder.timer(200ms).has_value());
}

TEST_CASE(
    "a joined peer that sends a frame with a wrong CRC is reported lost for breaking the "
    "protocol") {
    Recorder recorder;
    const Node node{2, recorder};
    auto platform = paramesh::platform_open({}, {});
    REQUIRE(platform.ok());

    // Play node 1 by hand: connect, send a correct JOIN_JOB, then a frame whose CRC is wrong.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(kLocalhost);
    addr.sin_port = htons(node.port);
    REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);

    const auto frame = [&](Opcode opcode, std::span<const std::byte> payload,
                           std::uint32_t crc_error) {
        FrameHeader header = header_of(opcode, 0);
        header.job = kJob;
        header.src = NodeId{1};
        header.dst = NodeId{2};
        header.payload_len = static_cast<std::uint32_t>(payload.size());
        header.payload_crc =
            paramesh::wire_payload_crc(*platform.value().checksum, payload) ^ crc_error;
        Bytes bytes(paramesh::kFrameHeaderSize + payload.size());
        paramesh::wire_encode_header(header, std::span<std::byte, paramesh::kFrameHeaderSize>{
                                                 bytes.data(), paramesh::kFrameHeaderSize});
        std::copy(payload.begin(), payload.end(), bytes.begin() + paramesh::kFrameHeaderSize);
        return bytes;
    };
    std::array<std::byte, 40> join{};
    REQUIRE(paramesh::wire_encode(paramesh::JoinJobPayload{2, 0, 0, {}}, join).ok());
    const Bytes hello = frame(Opcode::kJoinJob, join, 0);
    REQUIRE(::write(fd, hello.data(), hello.size()) == static_cast<ssize_t>(hello.size()));
    REQUIRE(recorder.joined() == NodeId{1});

    const std::array<std::byte, 8> page_id{};
    const Bytes corrupt = frame(Opcode::kReadReq, page_id, 1);
    REQUIRE(::write(fd, corrupt.data(), corrupt.size()) == static_cast<ssize_t>(corrupt.size()));
    const auto lost = recorder.lost(3s);
    ::close(fd);
    REQUIRE(lost.has_value());
    CHECK(lost.value_or(Lost{}).peer == NodeId{1});
    CHECK(lost.value_or(Lost{}).why == Errc::kProtocol);
    CHECK_FALSE(recorder.frame(100ms).has_value());  // the corrupt frame was never delivered
}
