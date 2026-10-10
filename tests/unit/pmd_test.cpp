// src/pmd/ and src/tools/: the minimal daemon and pmrun. The tests play the part of a peer
// daemon on the control channel, and this executable, started by a daemon or by pmrun, plays
// a job process on the local socket (see main.cpp for how a test executable becomes a helper
// process). The last tests run three daemons and the real pmrun executable.

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
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
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

// The hash of this executable. Taken once: the file is large and the hash is slow.
std::array<std::byte, 32> own_hash(const paramesh::Platform& platform) {
    static const std::array<std::byte, 32> hash = hash_of(platform, own_path());
    return hash;
}

template <typename Payload>
bool send(int fd, const paramesh::Platform& platform, Opcode opcode, std::uint64_t req,
          const Payload& payload, paramesh::JobId job = kJob) {
    std::vector<std::byte> bytes(paramesh::kMaxSpawnReq);
    const auto size = paramesh::wire_encode(payload, bytes);
    FrameHeader header;
    header.opcode = opcode;
    header.job = job;
    header.src = NodeId{1};  // the launcher's node
    header.req = paramesh::ReqId{req};
    return size.ok() && paramesh::frame_write(fd, *platform.checksum, header,
                                              std::span{bytes}.first(size.value()))
                            .ok();
}

// A daemon with a cap of 3 threads, serving on a thread of its own until the test ends; and a
// connection to its control port. Node 5 with no peers unless the test says otherwise.
struct Daemon {
    paramesh::Platform platform = quiet_platform();
    std::string state_dir;
    paramesh::Pmd pmd;
    std::thread thread;
    int control = -1;

    static paramesh::PmdConfig config(const std::string& state_dir, std::uint16_t node,
                                      std::vector<paramesh::Endpoint> peers) {
        paramesh::PmdConfig c;
        c.node = NodeId{node};
        c.control_port = 0;
        c.cap_cores = 3;
        c.state_dir = state_dir;
        c.peers = std::move(peers);
        return c;
    }
    static std::string fresh_directory() {
        std::array<char, 32> name = {"/tmp/pmd-test-XXXXXX"};
        REQUIRE(::mkdtemp(name.data()) != nullptr);
        return name.data();
    }

    explicit Daemon(std::uint16_t node = 5, std::vector<paramesh::Endpoint> peers = {})
        : state_dir(fresh_directory()), pmd(config(state_dir, node, std::move(peers)), platform) {
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
        r.binary_hash = own_hash(platform);
        r.path = own_path();
        r.cwd = state_dir;
        r.argv = {"--as-job-process", report, "an argument with spaces"};
        return r;
    }

    // Where another daemon finds this one.
    [[nodiscard]] paramesh::Endpoint endpoint() const {
        return paramesh::Endpoint{(127U << 24U) | 1U, pmd.control_port()};
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

// Writes a report whole: under another name first.
bool write_report(const std::string& path, const std::string& text) {
    {
        std::ofstream out{path + ".part"};
        out << text;
    }
    return ::rename((path + ".part").c_str(), path.c_str()) == 0;
}

int connect_unix(const std::string& path) {
    sockaddr_un to{};
    to.sun_family = AF_UNIX;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (path.size() >= sizeof to.sun_path) {
        return -1;
    }
    std::copy(path.begin(), path.end(), std::begin(to.sun_path));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    return ::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0 ? fd : -1;
}

// A TCP socket on 127.0.0.1: listening on a port the system chose (`port` 0, which is then
// set), or connected to `port`.
int loopback(std::uint16_t& port) {
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    at.sin_port = htons(port);
    socklen_t length = sizeof at;
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (port != 0) {
        return ::connect(fd, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0 ? fd : -1;
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&at), sizeof at) != 0 ||
        ::listen(fd, 8) != 0 || ::getsockname(fd, reinterpret_cast<sockaddr*>(&at), &length) != 0) {
        return -1;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    port = ntohs(at.sin_port);
    return fd;
}

// What this executable does when pmrun or a daemon starts it as a job process of a whole job:
// it speaks the local socket as a real one will (M3-7), and stands in for the data plane with
// one TCP connection from each worker to the launcher. The launcher ends when told to (`stay`)
// or at once; a worker ends when the launcher's connection closes. Reports go to `dir`, where
// the test has left this executable's hash.
int stand_in(const std::string& dir, bool stay) {
    auto opened = paramesh::platform_open(
        {}, paramesh::PlatformOptions{NodeId{1}, kJob, 2, paramesh::LogLevel::kError});
    if (!opened.ok()) {
        return 20;
    }
    const paramesh::Platform platform = std::move(opened).value();
    const bool launcher = env("PARAMESH_ROLE") == "launcher";
    const auto node = static_cast<std::uint16_t>(std::stoul(env("PARAMESH_NODE_ID")));
    const paramesh::JobId job{static_cast<std::uint32_t>(std::stoul(env("PARAMESH_JOB_ID")))};
    const std::string head = "pid=" + std::to_string(::getpid()) +
                             "\njob=" + std::to_string(job.value) +
                             "\nnode=" + std::to_string(node);

    paramesh::LRegisterPayload hello;
    hello.pid = static_cast<std::uint32_t>(::getpid());
    hello.role = launcher ? 1 : 2;
    const int listener = loopback(hello.data_port);
    const std::string hash = contents(dir + "/hash");
    const int pmd = connect_unix(env("PARAMESH_PMD_SOCKET"));
    if (listener < 0 || pmd < 0 || hash.size() != hello.binary_hash.size()) {
        return 21;
    }
    std::transform(hash.begin(), hash.end(), hello.binary_hash.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    if (!send(pmd, platform, Opcode::kLRegister, 0, hello, job)) {
        return 22;
    }
    const paramesh::Result<Frame> told = paramesh::frame_read(pmd, *platform.checksum);
    if (!told.ok() || told.value().header.opcode != Opcode::kLQuota) {
        return 23;
    }
    const auto threads = paramesh::wire_decode_l_quota(told.value().payload);
    if (!threads.ok()) {
        return 23;
    }
    const std::string quota = "\nquota=" + std::to_string(threads.value().threads);

    if (!launcher) {
        const std::string to = env("PARAMESH_LAUNCHER");
        auto port = static_cast<std::uint16_t>(std::stoul(to.substr(to.find(':') + 1)));
        const int link = loopback(port);
        const std::array<std::uint8_t, 2> id{static_cast<std::uint8_t>(node >> 8U),
                                             static_cast<std::uint8_t>(node)};
        if (link < 0 || ::send(link, id.data(), id.size(), MSG_NOSIGNAL) != 2 ||
            !write_report(dir + "/worker-" + std::to_string(node),
                          head + quota + "\nlauncher=" + to + "\n")) {
            return 24;
        }
        std::array<char, 16> rest{};
        while (::recv(link, rest.data(), rest.size(), 0) > 0) {
        }
        return 0;  // the launcher has gone: so does this worker
    }

    if (!send(pmd, platform, Opcode::kLAdmitReq, 0,
              paramesh::LAdmitReqPayload{paramesh::kSegmentSize, 2}, job)) {
        return 25;
    }
    const paramesh::Result<Frame> admitted = paramesh::frame_read(pmd, *platform.checksum);
    if (!admitted.ok() || admitted.value().header.opcode != Opcode::kLAdmitOk) {
        return 26;
    }
    const auto members = paramesh::wire_decode_l_admit_ok(admitted.value().payload);
    if (!members.ok() || members.value().members.front().port != hello.data_port) {
        return 27;
    }
    // The launcher is first; the workers follow in the order their daemons answered, which is
    // not fixed, so they are reported sorted.
    std::vector<unsigned> others;
    for (std::size_t i = 1; i < members.value().members.size(); i++) {
        others.push_back(members.value().members[i].node.value);
    }
    std::sort(others.begin(), others.end());
    std::string list = "\nlauncher_quota=" + std::to_string(members.value().quota) +
                       "\nmembers=" + std::to_string(members.value().members.front().node.value) +
                       " ";
    for (const unsigned other : others) {
        list += std::to_string(other) + " ";
    }
    // Every worker connects and names itself.
    std::vector<int> links;
    std::vector<unsigned> connected;
    for (std::size_t i = 1; i < members.value().members.size(); i++) {
        const int link = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        std::array<std::uint8_t, 2> id{};
        if (link < 0 || ::recv(link, id.data(), id.size(), MSG_WAITALL) != 2) {
            return 28;
        }
        links.push_back(link);
        connected.push_back((static_cast<unsigned>(id[0]) << 8U) | id[1]);
    }
    std::sort(connected.begin(), connected.end());
    list += "\nconnected=";
    for (const unsigned worker : connected) {
        list += std::to_string(worker) + " ";
    }
    if (!write_report(dir + "/launcher", head + quota + list + "\n")) {
        return 29;
    }
    if (!stay) {
        return 0;
    }
    const paramesh::Result<Frame> last = paramesh::frame_read(pmd, *platform.checksum);
    if (last.ok() && last.value().header.opcode == Opcode::kLAbort) {
        const auto why = paramesh::wire_decode_spawn_decline(last.value().payload);
        write_report(dir + "/aborted",
                     why.ok()
                         ? "status=" + std::to_string(static_cast<unsigned>(why.value().status)) +
                               "\nmessage=" + why.value().message + "\n"
                         : "unreadable\n");
    }
    return 1;
}

// The pmrun executable of this build: beside the unit tests' directory.
std::string pmrun_path() {
    return (std::filesystem::path{own_path()}.parent_path() / "../../src/tools/pmrun")
        .lexically_normal()
        .string();
}

pid_t start_pmrun(const std::vector<std::string>& arguments) {
    std::vector<std::string> all{pmrun_path()};
    all.insert(all.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(all.size() + 1);
    for (std::string& argument : all) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int null = ::open("/dev/null", O_WRONLY);
        ::dup2(null, STDERR_FILENO);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    return pid;
}

// The exit status of a child, or -1 if it has not ended in time.
int exit_status(pid_t pid, int seconds) {
    for (int tenths = 0; tenths < seconds * 10; tenths++) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        ::usleep(100000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return -1;
}

// The value of `key=` in a report.
std::string field(const std::string& report, const std::string& key) {
    const auto at = report.find(key + "=");
    if (at == std::string::npos) {
        return "(none)";
    }
    const auto from = at + key.size() + 1;
    return report.substr(from, report.find('\n', from) - from);
}

// Three daemons on this machine, nodes 1 to 3. Node 1 knows a peer that is not there, then
// the other two; and a directory for the stand-in processes' reports.
struct ThreeNodes {
    Daemon two{2};
    Daemon three{3};
    Daemon one{1, {paramesh::Endpoint{(127U << 24U) | 1U, 1}, two.endpoint(), three.endpoint()}};
    std::string reports = one.state_dir + "/reports";

    ThreeNodes() {
        std::filesystem::create_directory(reports);
        const std::array<std::byte, 32> hash = own_hash(one.platform);
        std::ofstream{reports + "/hash", std::ios::binary}.write(
            reinterpret_cast<const char*>(hash.data()), static_cast<std::streamsize>(hash.size()));
    }

    // pmrun on node 1, running this executable as a stand-in job process.
    [[nodiscard]] pid_t run(const char* nodes, const char* stay_or_go) const {
        return start_pmrun({"-n", nodes, "--state-dir", one.state_dir, own_path(), "--as-stand-in",
                            reports, stay_or_go});
    }
    [[nodiscard]] std::string report(const std::string& name) const {
        return contents(reports + "/" + name);
    }
};

}  // namespace

extern "C" int paramesh_test_helper(int argc, char** argv) {
    if (argc == 4 && std::string_view{argv[1]} == "--as-job-process") {
        return job_process(argv[2], argv[3]);
    }
    if (argc == 4 && std::string_view{argv[1]} == "--as-stand-in") {
        return stand_in(argv[2], std::string_view{argv[3]} == "stay");
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

TEST_CASE("pmrun starts a job on three simulated nodes from one command") {
    const ThreeNodes nodes;
    CHECK(exit_status(nodes.run("3", "go"), 120) == 0);  // pmrun's status is the launcher's

    // The launcher: job 1 of node 1, its quota, and three members. The peer that could not be
    // reached was passed over for the next.
    const std::string launcher = nodes.report("launcher");
    CHECK(field(launcher, "job") == "65537");  // node 1 in the high half, counter 1 in the low
    CHECK(field(launcher, "node") == "1");
    CHECK(field(launcher, "quota") == "3");
    CHECK(field(launcher, "launcher_quota") == "3");
    CHECK(field(launcher, "members") == "1 2 3 ");
    CHECK(field(launcher, "connected") == "2 3 ");  // both workers found the launcher
    for (const char* worker : {"worker-2", "worker-3"}) {
        const std::string report = nodes.report(worker);
        CHECK(field(report, "job") == "65537");
        CHECK(field(report, "quota") == "3");
        CHECK(field(report, "launcher").starts_with("127.0.0.1:"));
    }

    // The next job gets the next number. With one node, nobody is asked.
    std::filesystem::remove(nodes.reports + "/launcher");
    CHECK(exit_status(nodes.run("1", "go"), 120) == 0);
    CHECK(field(nodes.report("launcher"), "job") == "65538");
    CHECK(field(nodes.report("launcher"), "members") == "1 ");
}

TEST_CASE("Ctrl+C on pmrun ends the job and leaves no process behind") {
    const ThreeNodes nodes;
    const pid_t pmrun = nodes.run("3", "stay");
    for (int tenths = 0; tenths < 1200 && nodes.report("launcher").empty(); tenths++) {
        ::usleep(100000);  // until the job is up
    }
    REQUIRE_FALSE(nodes.report("launcher").empty());
    const std::vector<std::string> processes{field(nodes.report("launcher"), "pid"),
                                             field(nodes.report("worker-2"), "pid"),
                                             field(nodes.report("worker-3"), "pid")};

    REQUIRE(::kill(pmrun, SIGINT) == 0);
    CHECK(exit_status(pmrun, 30) == 130);
    // The launcher was told to end the job, by its daemon, because pmrun let go.
    CHECK(nodes.report("aborted") == "status=8\nmessage=interrupted\n");  // USER_ABORT
    for (const std::string& pid : processes) {
        bool gone = false;
        for (int tenths = 0; tenths < 100 && !gone; tenths++) {
            gone = ::kill(static_cast<pid_t>(std::stol(pid)), 0) != 0;
            ::usleep(100000);  // a worker is collected by its daemon within a moment
        }
        CHECK_MESSAGE(gone, "process " << pid << " is still there");
    }
}

TEST_CASE("pmrun says what is wrong when it cannot start a job") {
    const std::string nowhere = "/tmp/pmd-test-no-daemon";
    CHECK(exit_status(start_pmrun({"--state-dir", nowhere, "/bin/true"}), 20) == 1);  // no daemon
    CHECK(exit_status(start_pmrun({"--state-dir", nowhere, "/no/such/program"}), 20) == 2);
    CHECK(exit_status(start_pmrun({"-n", "9", "/bin/true"}), 20) == 2);
    CHECK(exit_status(start_pmrun({}), 20) == 2);
}
