// src/pmd/: the minimal daemon. The tests play the part of a peer daemon on the control
// channel, and this executable, started by the daemon as a worker, plays a job process on the
// local socket (see main.cpp for how a test executable becomes a helper process).

#include "pmd/pmd.h"

#include "platform/factory.h"
#include "pmd/frame_io.h"
#include "wire/payloads.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using paramesh::Errc;
using paramesh::Frame;
using paramesh::FrameHeader;
using paramesh::NodeId;
using paramesh::Opcode;
using paramesh::Status;

constexpr paramesh::JobId kJob{77};
constexpr std::uint16_t kWorkerPort = 4242;  // the data-plane port the helper process reports

// A platform whose log lines go nowhere.
paramesh::Platform quiet_platform() {
    static const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    auto platform = paramesh::platform_open(
        {}, paramesh::PlatformOptions{NodeId{9}, kJob, null, paramesh::LogLevel::kError});
    REQUIRE(platform.ok());
    return std::move(platform).value();
}

std::string own_path() {
    return std::filesystem::read_symlink("/proc/self/exe").string();
}

std::array<std::byte, 32> hash_of(const paramesh::Platform& platform, const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    const std::vector<char> image{std::istreambuf_iterator<char>{file},
                                  std::istreambuf_iterator<char>{}};
    return platform.hasher->sha256(std::as_bytes(std::span{image}));
}

template <typename Payload>
bool send(int fd, const paramesh::Platform& platform, Opcode opcode, std::uint64_t req,
          const Payload& payload) {
    std::vector<std::byte> bytes(paramesh::kMaxSpawnReq);
    const auto size = paramesh::wire_encode(payload, bytes);
    FrameHeader header;
    header.opcode = opcode;
    header.job = kJob;
    header.src = NodeId{1};  // the launcher's node
    header.req = paramesh::ReqId{req};
    return size.ok() && paramesh::frame_write(fd, *platform.checksum, header,
                                              std::span{bytes}.first(size.value()))
                            .ok();
}

// A daemon of node 5 with a cap of 3 threads, serving on a thread of its own until the test
// ends; and a connection to its control port.
struct Daemon {
    paramesh::Platform platform = quiet_platform();
    std::string state_dir;
    paramesh::Pmd pmd;
    std::thread thread;
    int control = -1;

    static paramesh::PmdConfig config(const std::string& state_dir) {
        paramesh::PmdConfig c;
        c.node = NodeId{5};
        c.control_port = 0;
        c.cap_cores = 3;
        c.state_dir = state_dir;
        return c;
    }
    static std::string fresh_directory() {
        std::array<char, 32> name = {"/tmp/pmd-test-XXXXXX"};
        REQUIRE(::mkdtemp(name.data()) != nullptr);
        return name.data();
    }

    Daemon() : state_dir(fresh_directory()), pmd(config(state_dir), platform) {
        const paramesh::Result<void> opened = pmd.open();
        REQUIRE_MESSAGE(opened.ok(), opened.error().what);
        thread = std::thread{[this] { pmd.run(); }};
        control = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(pmd.control_port());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
        REQUIRE(::connect(control, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0);
    }
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;
    Daemon(Daemon&&) = delete;
    Daemon& operator=(Daemon&&) = delete;
    ~Daemon() {
        ::close(control);
        pmd.stop();
        thread.join();
        std::filesystem::remove_all(state_dir);
    }

    // A SPAWN_REQ for this very executable as the worker, which then writes what it saw to
    // `report`.
    [[nodiscard]] paramesh::SpawnReqPayload request(const std::string& report) const {
        paramesh::SpawnReqPayload r;
        r.launcher_node = NodeId{1};
        r.launcher_addr = (10U << 24U) | (1U << 16U) | (2U << 8U) | 3U;  // 10.1.2.3
        r.launcher_port = 5000;
        r.threads_per_node = 8;
        r.region_bytes = paramesh::kSegmentSize;
        // Hashed once for all the tests: this executable is large and the hash is slow.
        static const std::array<std::byte, 32> own_hash = hash_of(platform, own_path());
        r.binary_hash = own_hash;
        r.path = own_path();
        r.cwd = state_dir;
        r.argv = {"--as-job-process", report, "an argument with spaces"};
        return r;
    }

    [[nodiscard]] Frame reply() const {
        paramesh::Result<Frame> frame = paramesh::frame_read(control, *platform.checksum);
        REQUIRE_MESSAGE(frame.ok(), frame.error().what);
        return std::move(frame).value();
    }
};

std::string contents(const std::string& path) {
    const std::ifstream file{path};
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// Read once, at the start of a process with one thread.
std::string env(const char* name) {
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return value != nullptr ? value : "(unset)";
}

// What this executable does when the daemon starts it as a worker: register on the local
// socket, wait for the quota, and write down what it was given.
int job_process(const std::string& report, const std::string& last_argument) {
    auto opened = paramesh::platform_open(
        {}, paramesh::PlatformOptions{NodeId{5}, kJob, 2, paramesh::LogLevel::kError});
    if (!opened.ok()) {
        return 10;
    }
    const paramesh::Platform platform = std::move(opened).value();
    const std::string path = env("PARAMESH_PMD_SOCKET");
    sockaddr_un to{};
    to.sun_family = AF_UNIX;
    if (path.size() >= sizeof to.sun_path) {
        return 11;
    }
    std::copy(path.begin(), path.end(), std::begin(to.sun_path));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (fd < 0 || ::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0) {
        return 12;
    }
    paramesh::LRegisterPayload hello;
    hello.pid = static_cast<std::uint32_t>(::getpid());
    hello.role = 2;
    hello.data_port = kWorkerPort;
    hello.binary_hash = hash_of(platform, own_path());
    if (!send(fd, platform, Opcode::kLRegister, 0, hello)) {
        return 13;
    }
    const paramesh::Result<Frame> frame = paramesh::frame_read(fd, *platform.checksum);
    if (!frame.ok() || frame.value().header.opcode != Opcode::kLQuota) {
        return 14;
    }
    const auto quota = paramesh::wire_decode_l_quota(frame.value().payload);
    if (!quota.ok()) {
        return 15;
    }
    std::ofstream out{report};
    out << "quota=" << quota.value().threads << "\njob=" << frame.value().header.job.value
        << "\nrole=" << env("PARAMESH_ROLE") << "\njob_id=" << env("PARAMESH_JOB_ID")
        << "\nnode_id=" << env("PARAMESH_NODE_ID") << "\nlauncher=" << env("PARAMESH_LAUNCHER")
        << "\nlisten=" << env("PARAMESH_LISTEN")
        << "\ncwd=" << std::filesystem::current_path().string()
        << "\nlast_argument=" << last_argument << "\n";
    return out.good() ? 0 : 16;
}

}  // namespace

extern "C" int paramesh_test_helper(int argc, char** argv) {
    if (argc == 4 && std::string_view{argv[1]} == "--as-job-process") {
        return job_process(argv[2], argv[3]);
    }
    return -1;
}

TEST_CASE("the command line: defaults, every flag, and what is refused") {
    const auto parse = [](std::vector<std::string_view> args) {
        return paramesh::pmd_parse_args(args);
    };
    const auto all = parse({"--node-id", "7", "--cap-cores", "4", "--control-port", "47002",
                            "--state-dir", "/tmp/somewhere", "--peers", "10.0.0.2,10.0.0.3:47005"});
    REQUIRE(all.ok());
    CHECK(all.value().node == NodeId{7});
    CHECK(all.value().cap_cores == 4);
    CHECK(all.value().control_port == 47002);
    CHECK(all.value().state_dir == "/tmp/somewhere");
    REQUIRE(all.value().peers.size() == 2);
    CHECK(all.value().peers[0].ipv4 == ((10U << 24U) | 2U));
    CHECK(all.value().peers[0].port == paramesh::kControlPort);  // the port's default
    CHECK(all.value().peers[1].port == 47005);

    const auto least = parse({"--node-id", "1"});
    REQUIRE(least.ok());
    CHECK(least.value().control_port == 47001);
    CHECK(least.value().cap_cores >= 1);  // this machine's processors
    CHECK(least.value().state_dir.ends_with("/.local/state/paramesh"));
    CHECK(least.value().peers.empty());

    const auto refused = [&](std::vector<std::string_view> args) {
        const auto parsed = parse(std::move(args));
        return !parsed.ok() && parsed.error().code == Errc::kInvalidArgument
                   ? std::string{parsed.error().what}
                   : std::string{"accepted"};
    };
    CHECK(refused({}) == "--node-id is required");
    CHECK(refused({"--cap-cores", "2"}) == "--node-id is required");
    CHECK(refused({"--node-id", "0"}) == "--node-id must be 1 to 65534");
    CHECK(refused({"--node-id", "65535"}) == "--node-id must be 1 to 65534");
    CHECK(refused({"--node-id", "seven"}) == "--node-id must be 1 to 65534");
    CHECK(refused({"--node-id", "1", "--cap-cores", "0"}) == "--cap-cores must be 1 to 65535");
    CHECK(refused({"--node-id", "1", "--control-port", "70000"}) ==
          "--control-port must be 0 to 65535");
    CHECK(refused({"--node-id", "1", "--peers", "not-an-address"}) ==
          "--peers must be address[:port], separated by commas");
    CHECK(refused({"--node-id", "1", "--peers", "10.0.0.2:0"}) ==
          "--peers must be address[:port], separated by commas");
    CHECK(refused({"--node-id", "1", "--speed", "9"}) == "a flag pmd does not know");
    CHECK(refused({"--node-id"}) == "the last flag has no value");
}

TEST_CASE("a job process started by pmd receives its quota, and the peer that asked is told") {
    const Daemon daemon;
    const std::string report = daemon.state_dir + "/report";
    REQUIRE(send(daemon.control, daemon.platform, Opcode::kSpawnReq, 31, daemon.request(report)));

    const Frame reply = daemon.reply();
    REQUIRE(reply.header.opcode == Opcode::kSpawnOk);
    CHECK(reply.header.job == kJob);
    CHECK(reply.header.src == NodeId{5});
    CHECK(reply.header.dst == NodeId{1});
    CHECK(reply.header.req == paramesh::ReqId{31});  // the request's number comes back
    const auto ok = paramesh::wire_decode_spawn_ok(reply.payload);
    REQUIRE(ok.ok());
    CHECK(ok.value().node == NodeId{5});
    CHECK(ok.value().data_port == kWorkerPort);  // what the worker said in L_REGISTER
    CHECK(ok.value().cores == 3);                // --cap-cores

    // The worker writes its report after the quota arrives, a moment after SPAWN_OK.
    for (int tenths = 0; tenths < 50 && contents(report).empty(); tenths++) {
        ::usleep(100000);
    }
    CHECK(contents(report) ==
          "quota=3\njob=77\nrole=worker\njob_id=77\nnode_id=5\n"
          "launcher=10.1.2.3:5000\nlisten=(unset)\ncwd=" +
              daemon.state_dir + "\nlast_argument=an argument with spaces\n");
}

TEST_CASE("a binary with a different hash is refused with \"binary mismatch\"") {
    const Daemon daemon;
    const std::string report = daemon.state_dir + "/report";
    paramesh::SpawnReqPayload request = daemon.request(report);
    request.binary_hash[0] ^= std::byte{1};  // the launcher runs some other build
    REQUIRE(send(daemon.control, daemon.platform, Opcode::kSpawnReq, 32, request));

    const Frame reply = daemon.reply();
    REQUIRE(reply.header.opcode == Opcode::kSpawnDecline);
    CHECK(reply.header.req == paramesh::ReqId{32});
    const auto declined = paramesh::wire_decode_spawn_decline(reply.payload);
    REQUIRE(declined.ok());
    CHECK(declined.value().status == Status::kBinaryMismatch);
    CHECK(declined.value().message ==
          "binary mismatch: " + own_path() + " on node 5 is not the launcher's executable");
    ::usleep(300000);
    CHECK_FALSE(std::filesystem::exists(report));  // and no process was started
}

TEST_CASE("a request for a program that is not there, or that dies at once, is declined") {
    const Daemon daemon;
    paramesh::SpawnReqPayload request = daemon.request(daemon.state_dir + "/report");
    request.path = daemon.state_dir + "/no-such-program";
    REQUIRE(send(daemon.control, daemon.platform, Opcode::kSpawnReq, 33, request));
    Frame reply = daemon.reply();
    REQUIRE(reply.header.opcode == Opcode::kSpawnDecline);
    auto declined = paramesh::wire_decode_spawn_decline(reply.payload);
    REQUIRE(declined.ok());
    CHECK(declined.value().status == Status::kNotFound);
    CHECK(declined.value().message == "no executable at " + request.path + " on node 5");

    // /bin/true is there and its hash is right, but it exits without registering.
    request.path = "/bin/true";
    request.binary_hash = hash_of(daemon.platform, request.path);
    request.argv.clear();
    REQUIRE(send(daemon.control, daemon.platform, Opcode::kSpawnReq, 34, request));
    reply = daemon.reply();
    REQUIRE(reply.header.opcode == Opcode::kSpawnDecline);
    CHECK(reply.header.req == paramesh::ReqId{34});
    declined = paramesh::wire_decode_spawn_decline(reply.payload);
    REQUIRE(declined.ok());
    CHECK(declined.value().status == Status::kInternal);
    CHECK(declined.value().message == "the worker on node 5 exited before it registered");
}

TEST_CASE("a stream that is not frames is closed") {
    const Daemon daemon;
    const std::string_view noise = "GET / HTTP/1.1\r\nHost: example\r\n\r\n....";
    REQUIRE(::send(daemon.control, noise.data(), noise.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(noise.size()));
    CHECK(paramesh::frame_read(daemon.control, *daemon.platform.checksum).error().code ==
          Errc::kClosed);
}
