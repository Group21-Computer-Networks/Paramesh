// src/pmd/ and src/tools/: the minimal daemon and pmrun. The tests play the part of a peer
// daemon on the control channel, and this executable, started by a daemon or by pmrun, plays
// a job process on the local socket (see main.cpp for how a test executable becomes a helper
// process). The last tests run three daemons and the real pmrun executable, first with
// stand-in job processes and then with this executable as a real ParaMesh program.

#include "pmd/pmd.h"

#include "net/frame_io.h"
#include "platform/factory.h"
#include "pmd/ledger.h"
#include "pmd/ledger_status.h"
#include "pmd/peers.h"
#include "pmd/scheduler.h"
#include "tools/pm.h"
#include "wire/payloads.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <paramesh.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
    std::atomic<bool> stopped{false};  // run() has returned: the daemon has stopped or left
    int control = -1;

    using Adjust = std::function<void(paramesh::PmdConfig&)>;

    static paramesh::PmdConfig config(const std::string& state_dir, std::uint16_t node,
                                      std::vector<paramesh::Endpoint> peers, const Adjust& adjust) {
        paramesh::PmdConfig c;
        c.node = NodeId{node};
        c.control_port = 0;
        c.discovery_port = 0;  // ports the system chooses, and no multicast: daemons of other
        c.http_port = 0;       // tests may be running on this machine at the same time
        c.multicast = false;
        c.cap_cores = 3;
        c.state_dir = state_dir;
        c.peers = std::move(peers);
        if (adjust) {
            adjust(c);
        }
        return c;
    }
    static std::string fresh_directory() {
        std::array<char, 32> name = {"/tmp/pmd-test-XXXXXX"};
        REQUIRE(::mkdtemp(name.data()) != nullptr);
        return name.data();
    }

    explicit Daemon(std::uint16_t node = 5, std::vector<paramesh::Endpoint> peers = {},
                    const Adjust& adjust = nullptr)
        : state_dir(fresh_directory()),
          pmd(config(state_dir, node, std::move(peers), adjust), platform) {
        const paramesh::Result<void> opened = pmd.open();
        REQUIRE_MESSAGE(opened.ok(), opened.error().what);
        thread = std::thread{[this] {
            pmd.run();
            stopped = true;
        }};
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
    const paramesh::JobId job{static_cast<std::uint32_t>(std::stoul(env("PARAMESH_JOB_ID")))};
    if (!send(fd, platform, Opcode::kLRegister, 0, hello, job)) {
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
    out << "quota=" << quota.value().threads << "\nram=" << quota.value().ram_bytes
        << "\njob=" << frame.value().header.job.value << "\nrole=" << env("PARAMESH_ROLE")
        << "\njob_id=" << env("PARAMESH_JOB_ID") << "\nnode_id=" << env("PARAMESH_NODE_ID")
        << "\nlauncher=" << env("PARAMESH_LAUNCHER") << "\nlisten=" << env("PARAMESH_LISTEN")
        << "\ncwd=" << std::filesystem::current_path().string()
        << "\nlast_argument=" << last_argument << "\n";
    if (!out.good()) {
        return 16;
    }
    out.close();
    // "watch": until pmd closes the connection, at most 120 s (a sanitizer build is slow to
    // start the next job), writing each quota pmd pushes after the first to `report`.pushed.
    for (int tenths = 0; last_argument == "watch" && tenths < 1200; tenths++) {
        pollfd ready{fd, POLLIN, 0};
        if (::poll(&ready, 1, 100) == 1) {
            const paramesh::Result<Frame> pushed = paramesh::frame_read(fd, *platform.checksum);
            if (!pushed.ok()) {
                break;
            }
            const auto again = paramesh::wire_decode_l_quota(pushed.value().payload);
            std::ofstream{report + ".pushed", std::ios::app}
                << (again.ok() ? std::to_string(again.value().threads) : std::string{"?"}) << ' ';
        }
    }
    return 0;
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

pid_t start_pmrun(const std::vector<std::string>& arguments,
                  const std::string& stderr_to = "/dev/null") {
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
        const int null = ::open(stderr_to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
    Daemon two;
    Daemon three;
    Daemon one;
    std::string reports = one.state_dir + "/reports";

    explicit ThreeNodes(const Daemon::Adjust& adjust = nullptr)
        : two{2, {}, adjust},
          three{3, {}, adjust},
          one{1,
              {paramesh::Endpoint{(127U << 24U) | 1U, 1}, two.endpoint(), three.endpoint()},
              adjust} {
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

// ---- this executable as a real ParaMesh program, started by pmrun and by daemons --------------

constexpr std::uint64_t kSquares = 64ULL * 512;        // 64 pages of numbers
constexpr std::uint64_t kManySquares = 2048ULL * 512;  // 2,048 pages: 8 MB
constexpr std::uint64_t kHugeRegion = 48ULL << 20U;    // twice three RAM caps of 8 MB
constexpr std::uint64_t kHugeSquares = (kHugeRegion - (64ULL << 10U)) / 8;  // nearly all of it

struct SquareArgs {
    std::uint64_t* squares;
    std::uint64_t* by_node;    // by_node[n]: indexes node n ran, n below 8
    std::uint64_t* by_thread;  // by_thread[t]: chunks run by a thread with index t, t below 64
    std::uint64_t* started;    // started[n]: node n has begun a chunk
    std::uint64_t slow;        // 1: every chunk takes ten minutes (Ctrl+C); 2: 2 ms (a join)
};

bool region_can_be_mapped() {
    return std::string_view{PARAMESH_SANITIZER_NAME} != "thread";  // as in lib_test.cpp
}

// Counts the processes whose command line names `marker`.
int processes_naming(const std::string& marker) {
    int found = 0;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc"}) {
        const std::string line = contents(entry.path().string() + "/cmdline");
        found += line.find(marker) != std::string::npos ? 1 : 0;
    }
    return found;
}

// The size of the largest spill file that a process naming `marker` has open now. The file
// has no name any more, but the process's descriptor still leads to it.
std::uint64_t spill_file_bytes(const std::string& marker) {
    std::uint64_t most = 0;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc"}) {
        if (contents(entry.path().string() + "/cmdline").find(marker) == std::string::npos) {
            continue;
        }
        for (const auto& fd : std::filesystem::directory_iterator{entry.path() / "fd", ignored}) {
            const std::string target = std::filesystem::read_symlink(fd.path(), ignored).string();
            if (target.find("/paramesh-spill-") != std::string::npos) {
                most =
                    std::max<std::uint64_t>(most, std::filesystem::file_size(fd.path(), ignored));
            }
        }
    }
    return most;
}

// The most kilobytes of the shared region that any process naming `marker` has in RAM now:
// the pages its local cache holds. Each process is stopped while it is read: the kernel walks
// the region from low addresses to high, and a running process can give up a low page and
// install a higher one during the walk, which would count one page twice.
std::uint64_t most_region_kb(const std::string& marker) {
    std::uint64_t most = 0;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc"}) {
        if (contents(entry.path().string() + "/cmdline").find(marker) == std::string::npos) {
            continue;
        }
        const pid_t pid = static_cast<pid_t>(std::stol(entry.path().filename().string()));
        ::kill(pid, SIGSTOP);
        for (int tries = 0; tries < 100 && contents(entry.path().string() + "/stat").find(") T ") ==
                                               std::string::npos;
             tries++) {
            ::usleep(1000);  // until it has stopped
        }
        std::ifstream smaps{entry.path().string() + "/smaps"};
        bool in_region = false;
        for (std::string line; std::getline(smaps, line);) {
            if (line.find('-') == 12 && line.find(' ') == 25) {  // a mapping's first line
                in_region = line.starts_with("600000000000-");
            } else if (in_region && line.starts_with("Rss:")) {
                most = std::max<std::uint64_t>(most, std::stoull(line.substr(4)));
            }
        }
        ::kill(pid, SIGCONT);
    }
    return most;
}

}  // namespace

PM_TASK(pmd_test_square) {
    const auto* args = static_cast<const SquareArgs*>(arg);
    // The first chunks wait until every node has begun one, so that each node runs some.
    volatile std::uint64_t* started = args->started;
    started[ctx->node] = 1;
    for (int waited = 0; waited < 10000 && (started[1] & started[2] & started[3]) == 0; waited++) {
        ::usleep(1000);
    }
    for (std::uint64_t i = lo; i < hi; i++) {
        args->squares[i] = i * i;
    }
    if (args->slow == 1) {
        ::usleep(600U * 1000000U);  // ten minutes: far longer than the test waits
    } else if (args->slow == 2) {
        ::usleep(2000);  // long enough for a node that joins to be given some
    }
    pm_atomic_add(&args->by_thread[ctx->thread < 64 ? ctx->thread : 63], 1);
    pm_atomic_add(&args->by_node[ctx->node < 8 ? ctx->node : 0], hi - lo);
}

namespace {

// The modes that run in rounds (M4-7, M4-8): once node 3 has begun a chunk the report says so
// (`.three`), and the test or script makes something happen. Rounds of chunks then go on until
// node 4 has run some (join), or until the file `.stop` beside the report exists (until-stop,
// big-until-stop), at most for 120 s. The last round is checked.
int rounds_program(const std::string& report, const SquareArgs& args, std::uint64_t count,
                   bool join) {
    const volatile std::uint64_t* started = args.started;
    const volatile std::uint64_t* by_node = args.by_node;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    bool told = false;
    do {
        if (pm_parallel_for_data("pmd_test_square", 0, count, 512, &args, sizeof args, args.squares,
                                 sizeof(std::uint64_t)) != PM_OK) {
            return 32;
        }
        while (!told && started[3] == 0 && std::chrono::steady_clock::now() < until) {
            ::usleep(1000);
        }
        if (!told) {
            write_report(report + ".three", "");
            told = true;
        }
        if (pm_wait_all() != PM_OK) {
            return 32;
        }
    } while ((join ? by_node[4] == 0 : !std::filesystem::exists(report + ".stop")) &&
             std::chrono::steady_clock::now() < until);
    std::uint64_t wrong = 0;
    for (std::uint64_t i = 0; i < count; i++) {
        wrong += args.squares[i] != i * i ? 1U : 0U;
    }
    write_report(report,
                 "wrong=" + std::to_string(wrong) + "\nnode4=" + std::to_string(by_node[4]) + "\n");
    return pm_finalize() == PM_OK ? 0 : 33;
}

// The launcher's side; a worker does not return from pm_init(). Writes its report, or the
// number of the step that failed.
int program(const std::string& report, std::string_view mode) {
    const bool slow = mode == "slow";
    const bool join = mode == "join";
    const bool rounds = join || mode == "until-stop" || mode == "big-until-stop";
    // squares over many more pages than a node may cache
    const bool big = mode == "big" || mode == "big-until-stop";
    const bool huge = mode == "huge";  // and over more than the nodes' home stores hold in RAM
    std::uint64_t count = kSquares;
    if (big || huge) {
        count = huge ? kHugeSquares : kManySquares;
    }
    pm_config config{.region_bytes = huge ? kHugeRegion : (big ? 12ULL : 4ULL) << 20U,
                     .threads_per_node = 0};
    if (mode == "too-big") {
        config.region_bytes = 64ULL << 20U;  // more than three nodes of 8 MB and 8 MB of spill
    }
    // Every process of the job leaves a mark as it starts, the workers too.
    write_report(report + ".started." + std::to_string(::getpid()), "");
    const int started = pm_init(nullptr, nullptr, &config);
    if (started != PM_OK) {
        return started == PM_ERR_REFUSED ? 34 : 30;
    }
    SquareArgs args{};
    args.squares = static_cast<std::uint64_t*>(pm_malloc(count * sizeof(std::uint64_t)));
    args.by_node = static_cast<std::uint64_t*>(pm_malloc(8 * sizeof(std::uint64_t)));
    args.by_thread = static_cast<std::uint64_t*>(pm_malloc(64 * sizeof(std::uint64_t)));
    args.started = static_cast<std::uint64_t*>(pm_malloc(8 * sizeof(std::uint64_t)));
    args.slow = slow ? 1 : 0;
    if (rounds) {
        args.slow = 2;
    }
    if (args.squares == nullptr || args.by_node == nullptr || args.by_thread == nullptr ||
        args.started == nullptr) {
        return 31;
    }
    write_report(report + ".up", "pid=" + std::to_string(::getpid()) + "\n");
    if (rounds) {
        return rounds_program(report, args, count, join);
    }
    if (pm_parallel_for_data("pmd_test_square", 0, count, huge ? 4096 : 512, &args, sizeof args,
                             args.squares, sizeof(std::uint64_t)) != PM_OK ||
        pm_wait_all() != PM_OK) {
        return 32;
    }
    std::uint64_t wrong = 0;
    for (std::uint64_t i = 0; i < count; i++) {
        wrong += args.squares[i] != i * i ? 1U : 0U;
    }
    std::uint64_t over_quota = 0;  // chunks run by a thread the quota of 3 does not allow
    for (std::size_t thread = 3; thread < 64; thread++) {
        over_quota += args.by_thread[thread];
    }
    const bool every_node = args.by_node[1] != 0 && args.by_node[2] != 0 && args.by_node[3] != 0;
    write_report(report, "wrong=" + std::to_string(wrong) + "\nindexes=" +
                             std::to_string(args.by_node[1] + args.by_node[2] + args.by_node[3]) +
                             "\nevery_node=" + (every_node ? "yes" : "no") +
                             "\nover_quota=" + std::to_string(over_quota) + "\n");
    return pm_finalize() == PM_OK ? 0 : 33;
}

}  // namespace

extern "C" int paramesh_test_helper(int argc, char** argv) {
    if (argc == 4 && std::string_view{argv[1]} == "--as-program") {
        return program(argv[2], argv[3]);
    }
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
    CHECK(parse({"--cap-ram", "512M"}).value().cap_ram == 512ULL << 20U);
    CHECK(parse({"--cap-ram", "2G"}).value().cap_ram == 2ULL << 30U);
    CHECK(parse({"--cap-ram", "8K"}).value().cap_ram == 8192);
    CHECK(parse({"--cap-ram", "4096"}).value().cap_ram == 4096);
    CHECK(all.value().control_port == 47002);
    CHECK(all.value().state_dir == "/tmp/somewhere");
    REQUIRE(all.value().peers.size() == 2);
    CHECK(all.value().peer_discovery_ports == std::vector<std::uint16_t>{47000, 47000});
    CHECK(all.value().peers[0].ipv4 == ((10U << 24U) | 2U));
    CHECK(all.value().peers[0].port == paramesh::kControlPort);  // the port's default
    CHECK(all.value().peers[1].port == 47005);

    const auto discovery = parse({"--discovery-port", "47100", "--http-port", "0", "--no-multicast",
                                  "--iface", "lo", "--peers", "10.0.0.2:47005:47004"});
    REQUIRE(discovery.ok());
    CHECK(discovery.value().node == paramesh::kNoNode);
    CHECK(discovery.value().discovery_port == 47100);
    CHECK(discovery.value().http_port == 0);
    CHECK_FALSE(discovery.value().multicast);
    CHECK(discovery.value().iface == "lo");
    REQUIRE(discovery.value().peer_discovery_ports.size() == 1);
    CHECK(discovery.value().peers[0].port == 47005);
    CHECK(discovery.value().peer_discovery_ports[0] == 47004);

    const auto least = parse({"--node-id", "1"});
    REQUIRE(least.ok());
    CHECK(least.value().control_port == 47001);
    CHECK(least.value().discovery_port == 47000);
    CHECK(least.value().http_port == 47002);
    CHECK(least.value().multicast);
    CHECK(least.value().cap_cores >= 1);                  // this machine's processors
    CHECK(least.value().cap_ram >= paramesh::kPageSize);  // this machine's RAM
    CHECK(least.value().state_dir.ends_with("/.local/state/paramesh"));
    CHECK(least.value().peers.empty());

    const auto refused = [&](std::vector<std::string_view> args) {
        const auto parsed = parse(std::move(args));
        return !parsed.ok() && parsed.error().code == Errc::kInvalidArgument
                   ? std::string{parsed.error().what}
                   : std::string{"accepted"};
    };
    CHECK(refused({}) == "accepted");  // the node ID then comes from the state directory
    CHECK(refused({"--node-id", "0"}) == "--node-id must be 1 to 65534");
    CHECK(refused({"--node-id", "65535"}) == "--node-id must be 1 to 65534");
    CHECK(refused({"--node-id", "seven"}) == "--node-id must be 1 to 65534");
    CHECK(refused({"--node-id", "1", "--cap-cores", "0"}) == "--cap-cores must be 1 to 65535");
    CHECK(refused({"--node-id", "1", "--control-port", "70000"}) ==
          "--control-port must be 0 to 65535");
    const std::string bad_peers =
        "--peers must be address[:control port[:discovery port]], separated by commas";
    CHECK(refused({"--node-id", "1", "--peers", "not-an-address"}) == bad_peers);
    CHECK(refused({"--node-id", "1", "--peers", "10.0.0.2:0"}) == bad_peers);
    CHECK(refused({"--node-id", "1", "--peers", "10.0.0.2:47001:0"}) == bad_peers);
    CHECK(refused({"--http-port", "70000"}) == "--http-port must be 0 to 65535");
    CHECK(refused({"--discovery-port", "x"}) == "--discovery-port must be 0 to 65535");
    const std::string bad_ram =
        "--cap-ram must be a number of bytes, at least 4096, with an optional K, M or G";
    CHECK(refused({"--cap-ram", "4095"}) == bad_ram);  // less than a page
    CHECK(refused({"--cap-ram", "1K"}) == bad_ram);
    CHECK(refused({"--cap-ram", "12X"}) == bad_ram);
    CHECK(refused({"--cap-ram", "G"}) == bad_ram);
    CHECK(refused({"--cap-ram", "99999999999999G"}) == bad_ram);  // more than 64 bits
    CHECK(refused({"--node-id", "1", "--speed", "9"}) == "a flag pmd does not know");
    CHECK(refused({"--node-id"}) == "the last flag has no value");
}

TEST_CASE("a job process started by pmd receives its quota, and the peer that asked is told") {
    const Daemon daemon{5, {}, [](paramesh::PmdConfig& c) { c.cap_ram = 64ULL << 20U; }};
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
          "quota=3\nram=67108864\njob=77\nrole=worker\njob_id=77\nnode_id=5\n"
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
    CHECK(field(launcher, "quota") == "3");  // at L_REGISTER the job's bound is not known yet
    // Since M5-3 a job is given no more slots than it can use: the stand-in asks for at most
    // 2 threads a node (L_ADMIT_REQ), so 2 of the 3 (--cap-cores), here and on the workers.
    CHECK(field(launcher, "launcher_quota") == "2");
    CHECK(field(launcher, "members") == "1 2 3 ");
    CHECK(field(launcher, "connected") == "2 3 ");  // both workers found the launcher
    for (const char* worker : {"worker-2", "worker-3"}) {
        const std::string report = nodes.report(worker);
        CHECK(field(report, "job") == "65537");
        CHECK(field(report, "quota") == "2");
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

TEST_CASE("a ParaMesh program started by pmrun runs its tasks on three nodes, within the quota" *
          doctest::skip(!region_can_be_mapped())) {
    const ThreeNodes nodes;
    const std::string report = nodes.reports + "/program";
    CHECK(exit_status(start_pmrun({"-n", "3", "--state-dir", nodes.one.state_dir, own_path(),
                                   "--as-program", report, "quick"}),
                      180) == 0);
    const std::string said = contents(report);
    CHECK(field(said, "wrong") == "0");
    CHECK(field(said, "indexes") == std::to_string(kSquares));
    CHECK(field(said, "every_node") == "yes");
    // Each daemon's --cap-cores is 3 and the program asks for no bound of its own: no thread
    // with an index of 3 or more ran a chunk, on a machine with more processors than that.
    CHECK(field(said, "over_quota") == "0");
    for (int tenths = 0; tenths < 100 && processes_naming(report) != 0; tenths++) {
        ::usleep(100000);  // the workers end on JOB_END
    }
    CHECK(processes_naming(report) == 0);
}

TEST_CASE("with --cap-ram, no process maps more of the region than its cache's share" *
          doctest::skip(!region_can_be_mapped())) {
    // 16 MB per node: 4 MB, 1,024 pages, for the local cache and 12 MB for the home store,
    // which holds the 12 MB region whatever the placement. Each node writes about 680 of the
    // 2,048 pages of squares and the launcher reads all of them, so pages are given up and
    // modified ones written back. The memory is sampled every 100 ms.
    const ThreeNodes nodes{[](paramesh::PmdConfig& c) { c.cap_ram = 16ULL << 20U; }};
    const std::string report = nodes.reports + "/program";
    const pid_t pmrun = start_pmrun(
        {"-n", "3", "--state-dir", nodes.one.state_dir, own_path(), "--as-program", report, "big"});
    std::uint64_t most_kb = 0;
    int status = -1;
    for (int tenths = 0; tenths < 1800 && status < 0; tenths++) {
        most_kb = std::max(most_kb, most_region_kb(report));
        int raw = 0;
        if (::waitpid(pmrun, &raw, WNOHANG) == pmrun) {
            status = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
        }
        ::usleep(100000);
    }
    if (status < 0) {
        ::kill(pmrun, SIGKILL);
        ::waitpid(pmrun, nullptr, 0);
    }
    REQUIRE(status == 0);
    const std::string said = contents(report);
    CHECK(field(said, "wrong") == "0");
    CHECK(field(said, "over_quota") == "0");  // and the threads stay within --cap-cores
    MESSAGE("most of the region one process held: " << most_kb << " KB");
    CHECK(most_kb > 0);
    CHECK(most_kb <= 4096);
}

TEST_CASE("a job whose region is twice the RAM caps completes with the right answer" *
          doctest::skip(!region_can_be_mapped())) {
    // Three nodes of 8 MB each and a region of 48 MB, nearly all of it written from the three
    // nodes and read back by the launcher. A node's home store holds 6 MB in RAM; the rest of
    // what it is home for goes to its spill file, which spill.budget lets reach 16 MB here
    // (the default, equal to --cap-ram, cannot hold a region of twice the caps).
    // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread of this process is running
    ::setenv("PARAMESH_CFG_SPILL_BUDGET", "16777216", 1);
    std::uint64_t most_spilled = 0;
    int status = -1;
    std::string said;
    {
        const ThreeNodes nodes{[](paramesh::PmdConfig& c) { c.cap_ram = 8ULL << 20U; }};
        const std::string report = nodes.reports + "/program";
        const pid_t pmrun = start_pmrun({"-n", "3", "--state-dir", nodes.one.state_dir, own_path(),
                                         "--as-program", report, "huge"});
        for (int tenths = 0; tenths < 3000 && status < 0; tenths++) {
            most_spilled = std::max(most_spilled, spill_file_bytes(report));
            int raw = 0;
            if (::waitpid(pmrun, &raw, WNOHANG) == pmrun) {
                status = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
            }
            ::usleep(100000);
        }
        if (status < 0) {
            ::kill(pmrun, SIGKILL);
            ::waitpid(pmrun, nullptr, 0);
        }
        said = contents(report);
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): as above
    ::unsetenv("PARAMESH_CFG_SPILL_BUDGET");
    REQUIRE(status == 0);
    CHECK(field(said, "wrong") == "0");
    CHECK(field(said, "indexes") == std::to_string(kHugeSquares));
    MESSAGE("the largest spill file seen: " << most_spilled / 1024 << " KB");
    CHECK(most_spilled > 0);              // pages did go to disk
    CHECK(most_spilled <= 16ULL << 20U);  // and no file passed its budget
}

TEST_CASE("Ctrl+C on pmrun ends a running ParaMesh job on every node" *
          doctest::skip(!region_can_be_mapped())) {
    const ThreeNodes nodes;
    const std::string report = nodes.reports + "/program";
    const pid_t pmrun = start_pmrun({"-n", "3", "--state-dir", nodes.one.state_dir, own_path(),
                                     "--as-program", report, "slow"});
    for (int tenths = 0; tenths < 1800 && contents(report + ".up").empty(); tenths++) {
        ::usleep(100000);  // until the job is up and about to queue its tasks
    }
    REQUIRE_FALSE(contents(report + ".up").empty());
    ::usleep(500000);                      // the chunks are running: each sleeps for ten minutes
    CHECK(processes_naming(report) == 4);  // pmrun, the launcher, two workers

    REQUIRE(::kill(pmrun, SIGINT) == 0);
    CHECK(exit_status(pmrun, 30) == 130);
    for (int tenths = 0; tenths < 100 && processes_naming(report) != 0; tenths++) {
        ::usleep(100000);
    }
    CHECK(processes_naming(report) == 0);
    CHECK(contents(report).empty());  // the job did not finish: it was ended
}

// ---- discovery (M4-1) ---------------------------------------------------------------------------

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kLoopback = (127U << 24U) | 1U;

paramesh::HelloPayload beacon(std::uint32_t instance) {
    paramesh::HelloPayload hello;
    hello.instance = instance;
    hello.control_port = 47001;
    hello.cap_cores = 2;
    hello.free_cores = 2;
    return hello;
}

// Free UDP ports, as many as asked for.
std::vector<std::uint16_t> free_udp_ports(std::size_t count) {
    std::vector<int> held;
    std::vector<std::uint16_t> ports;
    for (std::size_t i = 0; i < count; i++) {
        std::uint16_t port = 0;
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_ANY);
        socklen_t length = sizeof at;
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0 &&
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&at), &length) == 0) {
            port = ntohs(at.sin_port);
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        held.push_back(fd);
        ports.push_back(port);
    }
    for (const int fd : held) {
        ::close(fd);
    }
    return ports;
}

// Sends one discovery frame to a daemon's discovery port, from the given loopback address.
template <typename Payload>
void send_beacon(const paramesh::Platform& platform, Opcode opcode, std::uint16_t node,
                 const Payload& payload, std::uint16_t to_port, std::uint32_t from = kLoopback) {
    std::array<std::byte, 300> body{};
    const auto size = paramesh::wire_encode(payload, body);
    REQUIRE(size.ok());
    const std::span<const std::byte> bytes = std::span{body}.first(size.value());
    FrameHeader header;
    header.opcode = opcode;
    header.src = NodeId{node};
    header.dst = paramesh::kAllNodes;
    header.payload_len = static_cast<std::uint32_t>(bytes.size());
    header.payload_crc = paramesh::wire_payload_crc(*platform.checksum, bytes);
    std::vector<std::byte> datagram(paramesh::kFrameHeaderSize + bytes.size());
    paramesh::wire_encode_header(header, std::span{datagram}.first<paramesh::kFrameHeaderSize>());
    std::copy(bytes.begin(), bytes.end(), datagram.begin() + paramesh::kFrameHeaderSize);
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(from);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(kLoopback);
    to.sin_port = htons(to_port);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0);
    REQUIRE(::sendto(fd, datagram.data(), datagram.size(), 0,
                     reinterpret_cast<const sockaddr*>(&to),
                     sizeof to) == static_cast<ssize_t>(datagram.size()));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    ::close(fd);
}

// What GET /v1/peers says: "node:state" for each peer and "node@address" for each clash, in
// node order.
struct Heard {
    std::vector<std::string> peers;
    std::vector<std::string> clashes;
};

Heard heard_by(const Daemon& daemon) {
    Heard heard;
    const paramesh::Result<std::string> body =
        paramesh::http_get(daemon.pmd.http_port(), "/v1/peers");
    REQUIRE(body.ok());
    const auto parsed = daemon.platform.json->decode(body.value());
    REQUIRE(parsed.ok());
    const auto& top = std::get<paramesh::JsonObject>(parsed.value().v);
    for (const auto& [key, out] :
         {std::pair{"peers", &heard.peers}, std::pair{"clashes", &heard.clashes}}) {
        for (const paramesh::JsonValue& each : std::get<paramesh::JsonArray>(top.at(key).v)) {
            const auto& peer = std::get<paramesh::JsonObject>(each.v);
            const std::string node = std::to_string(std::get<std::int64_t>(peer.at("node").v));
            out->push_back(std::string_view{key} == "peers"
                               ? node + ":" + std::get<std::string>(peer.at("state").v)
                               : node + "@" + std::get<std::string>(peer.at("address").v));
        }
    }
    std::sort(heard.peers.begin(), heard.peers.end());
    std::sort(heard.clashes.begin(), heard.clashes.end());
    return heard;
}

// Waits up to `limit` for every daemon to list exactly `expected[i]`; returns how long it took,
// or `limit` if it did not happen.
std::chrono::milliseconds until_heard(const std::vector<const Daemon*>& daemons,
                                      const std::vector<std::vector<std::string>>& expected,
                                      std::chrono::milliseconds limit) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        bool all = true;
        for (std::size_t i = 0; i < daemons.size(); i++) {
            all = all && heard_by(*daemons[i]).peers == expected[i];
        }
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (all || took >= limit) {
            return all ? took : limit;
        }
        ::usleep(50000);
    }
}

}  // namespace

TEST_CASE("the peer table: new, refreshed, suspect after 3 missed beacons, gone after 5") {
    paramesh::PeerTable table{NodeId{1}, 77};
    using Heard = paramesh::PeerTable::Heard;
    CHECK(table.hello(NodeId{1}, kLoopback, beacon(77), 0s) == Heard::kOwn);  // its own, back
    CHECK(table.hello(NodeId{2}, kLoopback, beacon(5), 0s) == Heard::kNew);
    CHECK(table.hello(NodeId{2}, kLoopback, beacon(5), 1s) == Heard::kKnown);
    REQUIRE(table.peers().size() == 1);
    CHECK_FALSE(paramesh::PeerTable::suspect(table.peers()[0], 4s));  // 3 s silent: not yet
    CHECK(paramesh::PeerTable::suspect(table.peers()[0], 4s + 1ms));
    CHECK(table.expire(6s).empty());  // 5 s silent: not yet
    const auto gone = table.expire(6s + 1ms);
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].node == NodeId{2});
    CHECK(table.peers().empty());
}

TEST_CASE("the peer table: a restart, a BYE, and two daemons with one ID") {
    paramesh::PeerTable table{NodeId{1}, 77};
    using Heard = paramesh::PeerTable::Heard;
    table.hello(NodeId{2}, kLoopback, beacon(5), 0s);
    // Same address, new instance: the daemon restarted. It replaces the old entry.
    CHECK(table.hello(NodeId{2}, kLoopback, beacon(6), 1s) == Heard::kKnown);
    CHECK(table.peers()[0].hello.instance == 6);
    // Another address, another instance, the same ID: a clash. The first one keeps the ID.
    CHECK(table.hello(NodeId{2}, kLoopback + 1, beacon(9), 2s) == Heard::kClash);
    CHECK(table.peers()[0].address == kLoopback);
    // Someone else using this daemon's own ID.
    CHECK(table.hello(NodeId{1}, kLoopback + 1, beacon(8), 2s) == Heard::kClash);
    // A BYE of an instance that is not the one known changes nothing; the right one removes it.
    table.bye(NodeId{2}, 5);
    CHECK(table.peers().size() == 1);
    table.bye(NodeId{2}, 6);
    CHECK(table.peers().empty());
}

TEST_CASE("three daemons list each other within 2 s, by multicast") {
    const std::uint16_t port = free_udp_ports(1).front();
    const auto by_multicast = [port](paramesh::PmdConfig& c) {
        c.multicast = true;
        c.iface = "lo";  // multicast stays on this machine
        c.discovery_port = port;
    };
    const Daemon one{1, {}, by_multicast};
    const Daemon two{2, {}, by_multicast};
    const Daemon three{3, {}, by_multicast};
    const auto took = until_heard(
        {&one, &two, &three},
        {{"2:alive", "3:alive"}, {"1:alive", "3:alive"}, {"1:alive", "2:alive"}}, 3000ms);
    MESSAGE("listed each other after " << took.count() << " ms");
    CHECK(took < 2000ms);
    CHECK(heard_by(one).clashes.empty());

    // pm status says the same.
    std::array<int, 2> out{};
    REQUIRE(::pipe(out.data()) == 0);
    const std::string pm = (std::filesystem::path{pmrun_path()}.parent_path() / "pm").string();
    const std::string port_text = std::to_string(one.pmd.http_port());
    const pid_t child = ::fork();
    if (child == 0) {
        ::dup2(out[1], STDOUT_FILENO);
        ::execl(pm.c_str(), pm.c_str(), "status", "--http-port", port_text.c_str(), nullptr);
        ::_exit(127);
    }
    ::close(out[1]);
    CHECK(exit_status(child, 20) == 0);
    std::string printed;
    std::array<char, 1024> chunk{};
    for (ssize_t got = 0; (got = ::read(out[0], chunk.data(), chunk.size())) > 0;) {
        printed.append(chunk.data(), static_cast<std::size_t>(got));
    }
    ::close(out[0]);
    CHECK(printed.starts_with("node 1 (this node), 3 cores\n"));
    CHECK(printed.find("\n2      127.0.0.1") != std::string::npos);
    CHECK(printed.find("\n3      127.0.0.1") != std::string::npos);
}

TEST_CASE("with multicast off, the seed list is enough") {
    const std::vector<std::uint16_t> ports = free_udp_ports(3);
    // Each daemon is given the other two as seeds, with their discovery ports.
    const auto seeds_of = [&ports](std::size_t me) {
        return [&ports, me](paramesh::PmdConfig& c) {
            c.discovery_port = ports[me];
            for (std::size_t other = 0; other < ports.size(); other++) {
                if (other != me) {
                    c.peers.push_back(paramesh::Endpoint{kLoopback, 1});
                    c.peer_discovery_ports.push_back(ports[other]);
                }
            }
        };
    };
    const Daemon one{1, {}, seeds_of(0)};
    const Daemon two{2, {}, seeds_of(1)};
    const Daemon three{3, {}, seeds_of(2)};
    const auto took = until_heard(
        {&one, &two, &three},
        {{"2:alive", "3:alive"}, {"1:alive", "3:alive"}, {"1:alive", "2:alive"}}, 3000ms);
    CHECK(took < 2000ms);
}

TEST_CASE("a silent node is suspect after 3 s and disappears after 5 s; BYE removes one at once") {
    const Daemon daemon;
    send_beacon(daemon.platform, Opcode::kHello, 9, beacon(41), daemon.pmd.discovery_port());
    send_beacon(daemon.platform, Opcode::kHello, 8, beacon(42), daemon.pmd.discovery_port());
    CHECK(until_heard({&daemon}, {{"8:alive", "9:alive"}}, 1000ms) < 1000ms);
    send_beacon(daemon.platform, Opcode::kBye, 8, paramesh::ByePayload{42},
                daemon.pmd.discovery_port());
    CHECK(until_heard({&daemon}, {{"9:alive"}}, 1000ms) < 1000ms);  // gone at once

    // Node 9 says nothing more.
    CHECK(until_heard({&daemon}, {{"9:suspect"}}, 4000ms) < 4000ms);
    CHECK(until_heard({&daemon}, {{}}, 3000ms) < 3000ms);
}

TEST_CASE("two daemons with one node ID are reported, and the first keeps it") {
    const Daemon daemon;  // node 5
    send_beacon(daemon.platform, Opcode::kHello, 9, beacon(41), daemon.pmd.discovery_port());
    CHECK(until_heard({&daemon}, {{"9:alive"}}, 1000ms) < 1000ms);
    // Node 9 again, another instance, from another address; and someone claiming node 5.
    send_beacon(daemon.platform, Opcode::kHello, 9, beacon(43), daemon.pmd.discovery_port(),
                kLoopback + 1);
    send_beacon(daemon.platform, Opcode::kHello, 5, beacon(44), daemon.pmd.discovery_port(),
                kLoopback + 2);
    Heard heard;
    for (int tries = 0; tries < 20 && heard.clashes.size() < 2; tries++) {
        ::usleep(50000);
        heard = heard_by(daemon);
    }
    CHECK(heard.clashes == std::vector<std::string>{"5@127.0.0.3", "9@127.0.0.2"});
    CHECK(heard.peers == std::vector<std::string>{"9:alive"});
}

TEST_CASE("a node ID is made on the first run and kept in the state directory") {
    const paramesh::Platform platform = quiet_platform();
    const std::string state_dir = Daemon::fresh_directory();
    NodeId first;
    for (int run = 0; run < 2; run++) {
        const paramesh::PmdConfig config =
            Daemon::config(state_dir, 0, {}, nullptr);  // no --node-id
        paramesh::Pmd pmd{config, platform};
        REQUIRE(pmd.open().ok());
        CHECK(pmd.node() != paramesh::kNoNode);
        CHECK(pmd.node() != paramesh::kAllNodes);
        if (run == 0) {
            first = pmd.node();
        }
        CHECK(pmd.node() == first);  // the second run reads it back
        CHECK(contents(state_dir + "/node_id") == std::to_string(first.value) + "\n");
    }
    std::filesystem::remove_all(state_dir);
}

// ---- admission (M4-4)
// ----------------------------------------------------------------------------

namespace {

// Three daemons of 8 MB of RAM and 8 MB of spill each that find each other by multicast on
// this machine and have no seed list: a peer is known only through its beacons.
struct ThreeHeard {
    std::uint16_t port = free_udp_ports(1).front();
    Daemon::Adjust adjust = [this](paramesh::PmdConfig& c) {
        c.multicast = true;
        c.iface = "lo";
        c.discovery_port = port;
        c.cap_ram = 8ULL << 20U;
        c.spill_budget = 8ULL << 20U;
    };
    Daemon one{1, {}, adjust};
    Daemon two{2, {}, adjust};
    Daemon three{3, {}, adjust};
    std::string report = one.state_dir + "/program";

    ThreeHeard() {
        REQUIRE(
            until_heard({&one, &two, &three},
                        {{"2:alive", "3:alive"}, {"1:alive", "3:alive"}, {"1:alive", "2:alive"}},
                        5000ms) < 5000ms);
    }
    // The processes of the job that left their mark.
    [[nodiscard]] int started() const {
        int marks = 0;
        for (const auto& entry : std::filesystem::directory_iterator{one.state_dir}) {
            marks += entry.path().filename().string().starts_with("program.started.") ? 1 : 0;
        }
        return marks;
    }
};

}  // namespace

TEST_CASE(
    "an oversized job is refused before any worker starts, with what it needs and what there is" *
    doctest::skip(!region_can_be_mapped())) {
    const ThreeHeard nodes;
    const std::string said = nodes.one.state_dir + "/stderr";
    // 64 MB against 90% of three times 16 MB: 43.2 MB.
    CHECK(exit_status(start_pmrun({"-n", "3", "--state-dir", nodes.one.state_dir, own_path(),
                                   "--as-program", nodes.report, "too-big"},
                                  said),
                      60) == 34);  // pm_init() returned PM_ERR_REFUSED
    const std::string text = contents(said);
    CHECK(text.find("refused: the region needs 64 MB and the 3 nodes chosen give 43 MB (90% of "
                    "their RAM caps and spill budgets)") != std::string::npos);
    ::usleep(300000);             // time for a worker to have started, had one been asked for
    CHECK(nodes.started() == 1);  // the launcher alone
}

TEST_CASE("a job that fits is admitted and runs on the peers found by discovery" *
          doctest::skip(!region_can_be_mapped())) {
    const ThreeHeard nodes;  // no --peers: the workers can only be on discovered nodes
    CHECK(exit_status(start_pmrun({"-n", "3", "--state-dir", nodes.one.state_dir, own_path(),
                                   "--as-program", nodes.report, "quick"}),
                      180) == 0);
    const std::string said = contents(nodes.report);
    CHECK(field(said, "wrong") == "0");
    CHECK(field(said, "every_node") == "yes");
    CHECK(nodes.started() == 3);
}

TEST_CASE("one node alone refuses a region past 90% of its own RAM cap and spill budget" *
          doctest::skip(!region_can_be_mapped())) {
    const Daemon alone{1, {}, [](paramesh::PmdConfig& c) {
                           c.cap_ram = 2ULL << 20U;  // with 2 MB of spill: 3.6 MB admissible
                       }};
    const std::string report = alone.state_dir + "/program";
    const std::string said = alone.state_dir + "/stderr";
    CHECK(exit_status(start_pmrun({"-n", "1", "--state-dir", alone.state_dir, own_path(),
                                   "--as-program", report, "quick"},  // a 4 MB region
                                  said),
                      60) == 34);
    CHECK(contents(said).find("the region needs 4 MB and the 1 node chosen gives 3 MB") !=
          std::string::npos);
}

// ---- the abort path (M4-6)
// -----------------------------------------------------------------------

namespace {

// A job of three nodes whose chunks are running (each sleeps for ten minutes), started by
// pmrun with its stderr kept, and one of its two workers.
struct RunningJob {
    ThreeNodes nodes;
    std::string report = nodes.reports + "/program";
    std::string said = nodes.reports + "/stderr";
    pid_t pmrun = start_pmrun(
        {"-n", "3", "--state-dir", nodes.one.state_dir, own_path(), "--as-program", report, "slow"},
        said);
    pid_t worker = 0;
    std::string worker_node;  // its PARAMESH_NODE_ID

    RunningJob() {
        for (int tenths = 0; tenths < 1800 && contents(report + ".up").empty(); tenths++) {
            ::usleep(100000);
        }
        REQUIRE_FALSE(contents(report + ".up").empty());
        ::usleep(500000);  // the chunks are running
        for (const auto& entry : std::filesystem::directory_iterator{"/proc"}) {
            const std::string environment = contents(entry.path().string() + "/environ");
            const auto id = environment.find("PARAMESH_NODE_ID=");
            if (contents(entry.path().string() + "/cmdline").find(report) != std::string::npos &&
                environment.find("PARAMESH_ROLE=worker") != std::string::npos &&
                id != std::string::npos) {
                worker = static_cast<pid_t>(std::stol(entry.path().filename().string()));
                worker_node = environment.c_str() + id + 17;  // up to the entry's NUL
            }
        }
        REQUIRE(worker > 0);
    }

    // Seconds until pmrun has ended, and its exit status.
    [[nodiscard]] std::pair<double, int> ended() const {
        const auto from = std::chrono::steady_clock::now();
        const int status = exit_status(pmrun, 30);
        return {std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count(),
                status};
    }
    [[nodiscard]] bool said_lost() const {
        return contents(said).find(" aborted: node " + worker_node + " lost\n") !=
               std::string::npos;
    }
    // True once no process of the job is left, within ten seconds.
    [[nodiscard]] bool all_gone() const {
        for (int tenths = 0; tenths < 100 && processes_naming(report) != 0; tenths++) {
            ::usleep(100000);
        }
        return processes_naming(report) == 0;
    }
};

}  // namespace

TEST_CASE("killing a node mid-run ends the job everywhere within 3 s, naming the node" *
          doctest::skip(!region_can_be_mapped())) {
    const RunningJob job;
    REQUIRE(::kill(job.worker, SIGKILL) == 0);
    const auto [seconds, status] = job.ended();
    MESSAGE("the job ended " << seconds << " s after node " << job.worker_node << " was killed");
    CHECK(status == 1);
    CHECK(seconds < 3.0);
    CHECK(job.said_lost());  // "job N aborted: node K lost"
    CHECK(job.all_gone());   // the other worker too: nothing hangs
    CHECK(contents(job.report).empty());
}

TEST_CASE("a node that goes silent ends the job after 3 s without a heartbeat" *
          doctest::skip(!region_can_be_mapped())) {
    const RunningJob job;
    REQUIRE(::kill(job.worker, SIGSTOP) == 0);  // alive, connected, and saying nothing
    const auto [seconds, status] = job.ended();
    MESSAGE("the job ended " << seconds << " s after node " << job.worker_node << " went silent");
    CHECK(status == 1);
    // The 3 s run from the node's last frame, which may be a heartbeat sent up to 1 s before
    // it was stopped: so about 2 s at the least, and never at once as for a closed connection.
    CHECK(seconds >= 1.9);
    CHECK(seconds < 5.0);
    CHECK(job.said_lost());
    REQUIRE(::kill(job.worker, SIGKILL) == 0);  // the silent one itself cannot be told
    CHECK(job.all_gone());
}

// ---- leaving (M4-5) ----------------------------------------------------------------------------

namespace {

// Runs a tool of this build (pm, or matmul) to the end with its output in `out`; its exit
// status, or -1 if it took longer than `seconds`.
int run_tool(const std::string& path, const std::vector<std::string>& arguments,
             const std::string& out, int seconds = 60) {
    std::vector<std::string> all{path};
    all.insert(all.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(all.size() + 1);
    for (std::string& argument : all) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        const int file = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ::dup2(file, STDOUT_FILENO);
        ::dup2(file, STDERR_FILENO);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    return exit_status(child, seconds);
}

std::string tool(const char* name) {
    return (std::filesystem::path{pmrun_path()}.parent_path() / name).string();
}

std::string matmul_path() {
    return (std::filesystem::path{own_path()}.parent_path() / "../../apps/matmul/matmul")
        .lexically_normal()
        .string();
}

bool stops_within(const Daemon& daemon, int seconds) {
    for (int tenths = 0; tenths < seconds * 10 && !daemon.stopped; tenths++) {
        ::usleep(100000);
    }
    return daemon.stopped;
}

}  // namespace

TEST_CASE("pm leave on a node that serves no job: it says so, and its daemon goes") {
    const Daemon alone;
    const std::string out = alone.state_dir + "/out";
    CHECK(run_tool(tool("pm"), {"leave", "--state-dir", alone.state_dir}, out) == 0);
    CHECK(contents(out) == "pm: node 5 is leaving\n");
    CHECK(stops_within(alone, 5));
    CHECK(run_tool(tool("pm"), {"leave", "--state-dir", "/tmp/pmd-test-no-daemon"}, out) == 1);
    CHECK(contents(out).find("cannot reach pmd") != std::string::npos);
    CHECK(run_tool(tool("pm"), {"leave", "--forcibly"}, out) == 2);
}

TEST_CASE("Ctrl+C on pmd is a graceful leave: it logs why and goes; a second one stops at once") {
    const std::string pmd = (std::filesystem::path{pmrun_path()}.parent_path() / "../pmd/pmd")
                                .lexically_normal()
                                .string();
    for (const int presses : {1, 2}) {
        std::string state = "/tmp/pmd-test-int-XXXXXX";
        REQUIRE(::mkdtemp(state.data()) != nullptr);
        const std::string log = state + "/log";
        std::vector<std::string> all{pmd, "--node-id",      "9", "--no-multicast", "--http-port",
                                     "0", "--control-port", "0", "--state-dir",    state};
        std::vector<char*> argv;
        argv.reserve(all.size() + 1);
        for (std::string& argument : all) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        const pid_t child = ::fork();
        if (child == 0) {
            ::dup2(::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600), STDERR_FILENO);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        for (int tenths = 0; tenths < 50 && !std::filesystem::exists(state + "/pmd.sock");
             tenths++) {
            ::usleep(100000);
        }
        REQUIRE(std::filesystem::exists(state + "/pmd.sock"));
        for (int i = 0; i < presses; i++) {
            REQUIRE(::kill(child, SIGINT) == 0);
        }
        CHECK(exit_status(child, 5) == 0);
        const std::string said = contents(log);
        INFO(said);
        if (presses == 1) {
            CHECK(said.find("\"event\":\"leaving\",\"fields\":{\"reason\":2,\"workers\":0}") !=
                  std::string::npos);
        }
        std::filesystem::remove_all(state);
    }
}

TEST_CASE("on a launcher pm leave is refused while its job runs, and pm leave --force ends it" *
          doctest::skip(!region_can_be_mapped())) {
    const RunningJob job;
    const std::string out = job.nodes.reports + "/pm";
    const std::string& here = job.nodes.one.state_dir;
    CHECK(run_tool(tool("pm"), {"leave", "--state-dir", here}, out) == 1);
    CHECK(contents(out).find("this node launched job ") != std::string::npos);
    CHECK(contents(out).find(", which is still running; pm leave --force ends it") !=
          std::string::npos);
    CHECK_FALSE(job.nodes.one.stopped);  // nothing happened

    CHECK(run_tool(tool("pm"), {"leave", "--force", "--state-dir", here}, out) == 0);
    CHECK(contents(out) == "pm: node 1 is leaving\n");
    CHECK(exit_status(job.pmrun, 30) == 1);  // the job was ended ...
    CHECK(contents(job.said).find(" aborted: node 1 is leaving") != std::string::npos);
    CHECK(job.all_gone());                  // ... on every node
    CHECK(stops_within(job.nodes.one, 5));  // and the launcher's node went
}

TEST_CASE(
    "a worker leaves during matrix multiply: the job finishes with the single-machine result" *
    doctest::skip(!region_can_be_mapped())) {
    const std::string plain_out = "/tmp/pmd-test-matmul-plain";
    REQUIRE(run_tool(matmul_path(), {"--plain", "2048"}, plain_out, 120) == 0);
    const std::string plain = contents(plain_out);
    const std::string expected = plain.substr(plain.find("checksum "));
    std::filesystem::remove(plain_out);

    // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread of this process is running
    ::setenv("PARAMESH_CFG_LOG_LEVEL", "info", 1);
    {
        const ThreeNodes nodes;
        const std::string said = nodes.reports + "/stderr";
        const std::string printed = nodes.reports + "/stdout";
        std::vector<std::string> all{pmrun_path(),        "-n",          "3",    "--state-dir",
                                     nodes.one.state_dir, matmul_path(), "2048", "1"};
        std::vector<char*> argv;
        argv.reserve(all.size() + 1);
        for (std::string& argument : all) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        const pid_t pmrun = ::fork();
        if (pmrun == 0) {
            ::dup2(::open(printed.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600), STDOUT_FILENO);
            ::dup2(::open(said.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600), STDERR_FILENO);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        // matmul 2048 with one thread a node runs for seconds, long enough for the leave to
        // end inside it; at 1024 a slow machine finished the job before the leave was done.
        // Node 3's worker is running chunks: node 3 leaves, as `pm leave` there asks.
        // In the job means connected to the launcher and to node 2 besides its pmd and its
        // listener: four sockets.
        bool running = false;
        for (int tenths = 0; tenths < 600 && !running; tenths++) {
            ::usleep(100000);
            for (const auto& entry : std::filesystem::directory_iterator{"/proc"}) {
                const std::string environment = contents(entry.path().string() + "/environ");
                if (contents(entry.path().string() + "/cmdline").find(matmul_path()) ==
                        std::string::npos ||
                    environment.find("PARAMESH_NODE_ID=3") == std::string::npos) {
                    continue;
                }
                int sockets = 0;
                std::error_code ignored;
                for (const auto& fd :
                     std::filesystem::directory_iterator{entry.path() / "fd", ignored}) {
                    const std::string target = std::filesystem::read_symlink(fd.path(), ignored);
                    sockets += target.starts_with("socket:") ? 1 : 0;
                }
                running = running || sockets >= 4;
            }
        }
        REQUIRE(running);
        ::usleep(500000);
        const std::string out = nodes.reports + "/pm";
        CHECK(run_tool(tool("pm"), {"leave", "--state-dir", nodes.three.state_dir}, out) == 0);
        CHECK(contents(out) == "pm: node 3 is leaving\n");

        CHECK(exit_status(pmrun, 180) == 0);
        const std::string result = contents(printed);
        INFO(result);
        INFO(contents(said));
        CHECK(result.find(expected) != std::string::npos);  // every chunk done, every page kept
        CHECK(contents(said).find("\"event\":\"member_left\"") != std::string::npos);
        CHECK(stops_within(nodes.three, 10));  // node 3's daemon went once its worker had
        CHECK_FALSE(nodes.two.stopped);
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): as above
    ::unsetenv("PARAMESH_CFG_LOG_LEVEL");
}

TEST_CASE("after a worker leaves, a fourth node joins the running job and runs chunks" *
          doctest::skip(!region_can_be_mapped())) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): no other thread of this process is running
    ::setenv("PARAMESH_CFG_LOG_LEVEL", "info", 1);
    {
        const ThreeNodes nodes;
        const std::string report = nodes.reports + "/program";
        const std::string said = nodes.reports + "/stderr";
        const pid_t pmrun = start_pmrun({"-n", "3", "--state-dir", nodes.one.state_dir, own_path(),
                                         "--as-program", report, "join"},
                                        said);
        // Start-up alone can take over a minute under a sanitizer on two processors: every
        // process and daemon hashes the 30 MB test binary at debug-build speed.
        for (int tenths = 0; tenths < 1800 && !std::filesystem::exists(report + ".three");
             tenths++) {
            ::usleep(100000);
        }
        REQUIRE(std::filesystem::exists(report + ".three"));  // node 3 is running a chunk
        CHECK(run_tool(tool("pm"), {"leave", "--state-dir", nodes.three.state_dir},
                       nodes.reports + "/pm") == 0);
        CHECK(stops_within(nodes.three, 30));  // node 3 has left: the job is a node short

        // Node 4 comes up knowing node 1 as a seed; node 1 hears it and offers it the job.
        const Daemon four{4, {}, [&nodes](paramesh::PmdConfig& c) {
                              c.peers.push_back(nodes.one.endpoint());
                              c.peer_discovery_ports.push_back(nodes.one.pmd.discovery_port());
                          }};
        CHECK(exit_status(pmrun, 180) == 0);
        const std::string result = contents(report);
        INFO(result);
        INFO(contents(said));
        CHECK(field(result, "wrong") == "0");
        CHECK(std::strtoull(field(result, "node4").c_str(), nullptr, 10) > 0);  // node 4 ran chunks
        MESSAGE("chunks' indexes run by node 4: " << field(result, "node4"));
        CHECK(contents(said).find("\"event\":\"member_joined\"") != std::string::npos);
        CHECK(contents(said).find("\"event\":\"member_left\"") != std::string::npos);
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): as above
    ::unsetenv("PARAMESH_CFG_LOG_LEVEL");
}

namespace {

std::string ledger_dir() {
    std::array<char, 32> name = {"/tmp/pmd-ledger-XXXXXX"};
    REQUIRE(::mkdtemp(name.data()) != nullptr);
    return name.data();
}

// A ledger with `jobs` jobs of node 2 and one of node 3, every count derived from `seed`.
void fill_ledger(paramesh::Ledger& ledger, std::uint32_t jobs, std::uint64_t seed) {
    using paramesh::Direction;
    for (std::uint32_t job = 1; job <= jobs; job++) {
        ledger.add_chunk(NodeId{2}, paramesh::JobId{job}, Direction::kForPeer, 0xABCDEF, seed,
                         std::chrono::nanoseconds{seed * 10});
        ledger.add_chunk(NodeId{2}, paramesh::JobId{job}, Direction::kByPeer, 7, seed + 1,
                         std::chrono::nanoseconds{0});
        ledger.add_segment_seconds(NodeId{2}, paramesh::JobId{job}, Direction::kByPeer, 1.5);
    }
    ledger.add_chunk(NodeId{3}, paramesh::JobId{65537}, Direction::kForPeer, 7, seed,
                     std::chrono::nanoseconds{1});
}

}  // namespace

TEST_CASE("the ledger keeps both directions per peer and job, and reads back what it saved") {
    const paramesh::Platform platform = quiet_platform();
    const std::string dir = ledger_dir();
    const std::string path = dir + "/ledger.json";
    using paramesh::Direction;
    {
        auto opened = paramesh::Ledger::open(path, *platform.json);
        REQUIRE(opened.ok());  // no file yet: an empty ledger
        paramesh::Ledger ledger = std::move(opened).value();
        CHECK(ledger.data().empty());
        CHECK_FALSE(ledger.dirty());
        ledger.add_chunk(NodeId{2}, paramesh::JobId{9}, Direction::kForPeer, 0x1234, 512,
                         std::chrono::nanoseconds{7000});
        ledger.add_chunk(NodeId{2}, paramesh::JobId{9}, Direction::kForPeer, 0x1234, 256,
                         std::chrono::nanoseconds{3000});
        ledger.add_chunk(NodeId{2}, paramesh::JobId{9}, Direction::kByPeer, 0x1234, 512,
                         std::chrono::nanoseconds{0});
        ledger.add_segment_seconds(NodeId{2}, paramesh::JobId{9}, Direction::kForPeer, 2.5);
        ledger.side(NodeId{2}, paramesh::JobId{9}, Direction::kByPeer).credit = 10.0;
        ledger.side(NodeId{2}, paramesh::JobId{9}, Direction::kForPeer).credit = 4.0;
        CHECK(ledger.dirty());
        CHECK(ledger.balance(NodeId{2}) == doctest::Approx(6.0));  // given 10, taken 4
        CHECK(ledger.balance(NodeId{3}) == doctest::Approx(0.0));
        const paramesh::TaskWork mine = ledger.data().at(2).at(9).for_peer.tasks.at(0x1234);
        CHECK(mine == paramesh::TaskWork{2, 768, 10000});
        REQUIRE(ledger.save().ok());
        CHECK_FALSE(ledger.dirty());
        CHECK_FALSE(std::filesystem::exists(path + ".tmp"));
    }
    const auto again = paramesh::Ledger::open(path, *platform.json);
    REQUIRE(again.ok());
    const paramesh::JobWork& work = again.value().data().at(2).at(9);
    CHECK(work.for_peer.tasks.at(0x1234) == paramesh::TaskWork{2, 768, 10000});
    CHECK(work.by_peer.tasks.at(0x1234) == paramesh::TaskWork{1, 512, 0});
    CHECK(work.for_peer.segment_seconds == doctest::Approx(2.5));
    CHECK(again.value().balance(NodeId{2}) == doctest::Approx(6.0));

    for (const std::string text :
         {"not json", R"({"version":2,"peers":{}})", R"({"version":1,"peers":{"x":{}}})",
          R"({"version":1,"peers":{"2":{"9":{"for_peer":{}}}}})"}) {
        std::ofstream{path, std::ios::trunc} << text;
        const auto refused = paramesh::Ledger::open(path, *platform.json);
        CHECK(refused.error().code == paramesh::Errc::kInvalidArgument);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a process killed while it saves the ledger leaves a whole ledger, old or new") {
    const paramesh::Platform platform = quiet_platform();
    const std::string dir = ledger_dir();
    const std::string path = dir + "/ledger.json";
    // Two different ledgers, large enough that a save takes a while.
    auto made = paramesh::Ledger::open(path, *platform.json);
    REQUIRE(made.ok());
    paramesh::Ledger first = made.value();
    paramesh::Ledger second = made.value();
    fill_ledger(first, 2000, 1);
    fill_ledger(second, 2000, 2);
    REQUIRE(first.save().ok());
    int killed_mid_write = 0;
    for (int round = 0; round < 20; round++) {
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            for (int i = 0;; i++) {  // saves without end until killed
                static_cast<void>((i % 2 == 0 ? second : first).save());
            }
        }
        // Killed as soon as it has begun writing the temporary file: in the middle of a save.
        std::filesystem::remove(path + ".tmp");
        for (int waited = 0; waited < 100000 && !std::filesystem::exists(path + ".tmp"); waited++) {
            ::usleep(10);
        }
        ::usleep(static_cast<useconds_t>((round * 37) % 200));
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        killed_mid_write += std::filesystem::exists(path + ".tmp") ? 1 : 0;
        const auto read = paramesh::Ledger::open(path, *platform.json);
        REQUIRE(read.ok());
        const bool whole =
            read.value().data() == first.data() || read.value().data() == second.data();
        CHECK(whole);
    }
    MESSAGE("killed with a temporary file left in " << killed_mid_write << " of 20 rounds");
    CHECK(killed_mid_write > 0);  // the kills did land during writes
    std::filesystem::remove_all(dir);
}

namespace {

// The ledger a daemon saved in its state directory.
paramesh::LedgerData saved_ledger(const Daemon& daemon) {
    const auto read =
        paramesh::Ledger::open(daemon.state_dir + "/ledger.json", *daemon.platform.json);
    REQUIRE(read.ok());
    return read.value().data();
}

std::uint64_t indexes_of(const paramesh::WorkSide& side) {
    std::uint64_t sum = 0;
    for (const auto& [task, work] : side.tasks) {
        sum += work.indexes;
    }
    return sum;
}

}  // namespace

TEST_CASE("after a job each daemon's ledger has the work both ways, and keeps it over a restart" *
          doctest::skip(!region_can_be_mapped())) {
    const ThreeNodes nodes;
    const std::string report = nodes.reports + "/program";
    REQUIRE(exit_status(start_pmrun({"-n", "3", "--state-dir", nodes.one.state_dir, own_path(),
                                     "--as-program", report, "quick"}),
                        180) == 0);
    REQUIRE(field(contents(report), "every_node") == "yes");
    for (const Daemon* daemon : {&nodes.one, &nodes.two, &nodes.three}) {
        daemon->pmd.stop();  // a daemon saves its ledger as it stops
    }
    for (const Daemon* daemon : {&nodes.one, &nodes.two, &nodes.three}) {
        CHECK(stops_within(*daemon, 5));
    }
    const paramesh::LedgerData one = saved_ledger(nodes.one);
    const paramesh::LedgerData two = saved_ledger(nodes.two);
    const paramesh::LedgerData three = saved_ledger(nodes.three);
    REQUIRE(one.size() == 2);  // nodes 2 and 3 worked for node 1's job
    REQUIRE(two.size() == 1);  // node 2 worked for node 1 only
    REQUIRE(three.size() == 1);
    const std::uint32_t job = one.at(2).begin()->first;
    const paramesh::JobWork& by_two = one.at(2).at(job);
    const paramesh::JobWork& for_one = two.at(1).at(job);
    // Each side counted the same chunks, from its own records: the launcher from the peers'
    // TASK_DONE, the worker from its own threads, which also measured their CPU time.
    CHECK(indexes_of(by_two.by_peer) > 0);
    CHECK(indexes_of(by_two.by_peer) == indexes_of(for_one.for_peer));
    CHECK(indexes_of(one.at(3).at(job).by_peer) == indexes_of(three.at(1).at(job).for_peer));
    CHECK(indexes_of(by_two.by_peer) + indexes_of(one.at(3).at(job).by_peer) < kSquares);
    CHECK(for_one.for_peer.tasks.begin()->second.cpu_ns > 0);
    CHECK(by_two.by_peer.tasks.begin()->second.cpu_ns == 0);
    CHECK(by_two.for_peer.tasks.empty());  // node 1 did nothing for node 2's jobs
    // Segment-seconds: node 2 hosted some of the job's segments while it ran, as both see it.
    CHECK(for_one.for_peer.segment_seconds > 0);
    CHECK(by_two.by_peer.segment_seconds > 0);
    // The credits (M5-2), worked out as each daemon saved: node 1 values node 2's work at its
    // own cost of the same task, measured on the chunks its own threads ran.
    CHECK(by_two.by_peer.credit > 0);
    CHECK(for_one.for_peer.credit > 0);
    // What each daemon serves at GET /v1/ledger, and what they said to each other (M5-4) are
    // checked in "after a job both daemons' /v1/ledger agree, and LEDGER_SYNC says so".

    // A daemon started again on node 2's state directory reads the same ledger, and leaves it so.
    {
        const paramesh::Platform platform = quiet_platform();
        paramesh::Pmd again{Daemon::config(nodes.two.state_dir, 2, {}, nullptr), platform};
        REQUIRE(again.open().ok());
        std::thread running{[&again] { again.run(); }};
        again.stop();
        running.join();
    }
    CHECK(saved_ledger(nodes.two) == two);
}

TEST_CASE("credit rules: fixed counts give the expected balances, from this node's measures only") {
    const paramesh::Platform platform = quiet_platform();
    const std::string dir = ledger_dir();
    using paramesh::Direction;
    using std::chrono::milliseconds;
    auto opened = paramesh::Ledger::open(dir + "/ledger.json", *platform.json);
    REQUIRE(opened.ok());
    paramesh::Ledger ledger = std::move(opened).value();
    const std::uint64_t task_a = 0xA;
    const std::uint64_t task_b = 0xB;
    const std::uint64_t task_c = 0xC;
    // This node ran 1,000 indexes of task A in 2 s of CPU: 2 ms an index.
    ledger.add_own_chunk(task_a, 600, milliseconds{1200});
    ledger.add_own_chunk(task_a, 400, milliseconds{800});
    // Node 2 ran 500 indexes of A for this node's job 1, and claims 999 s of CPU for them.
    ledger.add_chunk(NodeId{2}, paramesh::JobId{1}, Direction::kByPeer, task_a, 500,
                     milliseconds{999000});
    // ... and hosted segments of it: 512 segments (1 GB) for an hour.
    ledger.add_segment_seconds(NodeId{2}, paramesh::JobId{1}, Direction::kByPeer, 512.0 * 3600);
    // This node ran task B for node 2's job 2, measuring 3 s, and hosted half a GB-hour for it.
    ledger.add_chunk(NodeId{2}, paramesh::JobId{2}, Direction::kForPeer, task_b, 50,
                     milliseconds{3000});
    ledger.add_segment_seconds(NodeId{2}, paramesh::JobId{2}, Direction::kForPeer, 256.0 * 3600);
    // Node 3 ran 700 indexes of task C, which this node has never run.
    ledger.add_chunk(NodeId{3}, paramesh::JobId{1}, Direction::kByPeer, task_c, 700,
                     milliseconds{5});

    ledger.apply_credits(paramesh::CreditRates{450});
    // Given by node 2: 500 x 2 ms = 1 core-second, plus 1 GB-hour = 450. Taken: 3 + 225.
    CHECK(ledger.data().at(2).at(1).by_peer.credit == doctest::Approx(451.0));
    CHECK(ledger.data().at(2).at(2).for_peer.credit == doctest::Approx(228.0));
    CHECK(ledger.balance(NodeId{2}) == doctest::Approx(223.0));
    // Node 3's work waits for a measure of task C here: no credit yet, 700 indexes pending.
    CHECK(ledger.balance(NodeId{3}) == doctest::Approx(0.0));
    CHECK(ledger.pending_indexes(NodeId{3}) == 700);
    CHECK(ledger.pending_indexes(NodeId{2}) == 0);

    // No input comes from a received message field: a different claimed CPU time, the only such
    // field in the counts, changes nothing.
    ledger.add_chunk(NodeId{2}, paramesh::JobId{1}, Direction::kByPeer, task_a, 0,
                     milliseconds{123456789});
    ledger.apply_credits(paramesh::CreditRates{450});
    CHECK(ledger.balance(NodeId{2}) == doctest::Approx(223.0));

    // Once this node has run task C (100 indexes in 0.5 s: 5 ms each), node 3's work is valued.
    ledger.add_own_chunk(task_c, 100, milliseconds{500});
    ledger.apply_credits(paramesh::CreditRates{450});
    CHECK(ledger.balance(NodeId{3}) == doctest::Approx(3.5));
    CHECK(ledger.pending_indexes(NodeId{3}) == 0);
    // The rate is the configured one: twice the rate, twice the memory credit.
    ledger.apply_credits(paramesh::CreditRates{900});
    CHECK(ledger.balance(NodeId{2}) == doctest::Approx(1.0 + 900 - 3 - 450));

    // The measures of this node are saved with the rest.
    REQUIRE(ledger.save().ok());
    const auto again = paramesh::Ledger::open(dir + "/ledger.json", *platform.json);
    REQUIRE(again.ok());
    CHECK(again.value().own() == ledger.own());
    CHECK(again.value().balance(NodeId{2}) == doctest::Approx(ledger.balance(NodeId{2})));
    std::filesystem::remove_all(dir);
}

namespace {

// A ledger for the slot allocation's tests: balances set by hand.
class FakeLedger final : public paramesh::LedgerView {
public:
    std::map<std::uint16_t, double> balances;
    [[nodiscard]] double balance(NodeId peer) const override {
        const auto found = balances.find(peer.value);
        return found == balances.end() ? 0 : found->second;
    }
};

paramesh::SlotDemand guest(double weight, std::uint16_t want = 16, double ram_share = 0) {
    return paramesh::SlotDemand{false, want, ram_share, weight, 0};
}

}  // namespace

TEST_CASE("slot weights come from this node's ledger, never below 0.25") {
    FakeLedger ledger;
    ledger.balances = {{2, 3600.0}, {3, -1800.0}, {4, -100000.0}};
    CHECK(paramesh::slot_weight(ledger, NodeId{2}, 3600) == doctest::Approx(2.0));  // a core-hour
    CHECK(paramesh::slot_weight(ledger, NodeId{3}, 3600) == doctest::Approx(0.5));
    CHECK(paramesh::slot_weight(ledger, NodeId{4}, 3600) == doctest::Approx(0.25));  // the floor
    CHECK(paramesh::slot_weight(ledger, NodeId{9}, 3600) == doctest::Approx(1.0));   // unknown: 0
}

TEST_CASE("slots follow weights; own jobs first; equal guests share evenly") {
    using paramesh::allocate_slots;
    using V = std::vector<std::uint16_t>;
    CHECK(allocate_slots(6, {guest(2.0), guest(1.0)}) == V{4, 2});  // twice the weight
    CHECK(allocate_slots(6, {guest(1.0), guest(1.0), guest(1.0)}) == V{2, 2, 2});
    // The node's own job takes what it wants first; the guests share the rest.
    CHECK(allocate_slots(6, {guest(1.0), {true, 2, 0, 1, 0}, guest(1.0)}) == V{2, 2, 2});
    CHECK(allocate_slots(4, {{true, 8, 0, 1, 0}, guest(4.0)}) == V{4, 0});
    // No more than a job wants; slots nobody wants stay free.
    CHECK(allocate_slots(6, {guest(1.0, 1), guest(1.0, 2)}) == V{1, 2});
    // A free-rider at the 0.25 floor gets less than an equal contributor, but some.
    const V share = allocate_slots(10, {guest(1.0), guest(1.0), guest(0.25)});
    CHECK(share[2] < share[0]);
    CHECK(share[2] > 0);
    CHECK(share[0] + share[1] + share[2] == 10);
    // The home-store RAM a job holds counts in its dominant share.
    // A guest holding three quarters of the RAM gets slots only once the other's share of the
    // slots has caught up: 2 against 6.
    CHECK(allocate_slots(8, {guest(1.0, 16, 0.75), guest(1.0, 16, 0.0)}) == V{2, 6});
}

TEST_CASE("a job with work never goes without a slot for more than one share-out") {
    // One slot, three guests of different weight, each with work: at every task boundary the
    // slots are shared out again, and one left out last time comes first.
    std::vector<paramesh::SlotDemand> jobs = {guest(4.0), guest(1.0), guest(0.25)};
    std::vector<int> rounds_without(jobs.size(), 0);
    for (int boundary = 0; boundary < 30; boundary++) {
        const std::vector<std::uint16_t> slots = paramesh::allocate_slots(1, jobs);
        for (std::size_t i = 0; i < jobs.size(); i++) {
            rounds_without[i] = slots[i] == 0 ? rounds_without[i] + 1 : 0;
            CHECK(rounds_without[i] <= 2);  // with three jobs on one slot, at most two in a row
            jobs[i].waited = slots[i] == 0 ? jobs[i].waited + 1 : 0;
        }
    }
}

TEST_CASE("pmd shares its slots among the jobs on it and pushes each new share") {
    const Daemon daemon;  // --cap-cores 3
    const std::string first = daemon.state_dir + "/first";
    paramesh::SpawnReqPayload watching = daemon.request(first);
    watching.argv.back() = "watch";
    REQUIRE(send(daemon.control, daemon.platform, Opcode::kSpawnReq, 1, watching));
    REQUIRE(daemon.reply().header.opcode == Opcode::kSpawnOk);
    for (int tenths = 0; tenths < 50 && contents(first).empty(); tenths++) {
        ::usleep(100000);
    }
    CHECK(field(contents(first), "quota") == "3");  // alone: every slot

    // A second job, from the same peer, so of the same weight: the slots are shared, 2 and 1.
    // When it ends, the first has all three again.
    const std::string second = daemon.state_dir + "/second";
    REQUIRE(send(daemon.control, daemon.platform, Opcode::kSpawnReq, 2, daemon.request(second),
                 paramesh::JobId{78}));
    REQUIRE(daemon.reply().header.opcode == Opcode::kSpawnOk);
    for (int tenths = 0; tenths < 50 && contents(second).empty(); tenths++) {
        ::usleep(100000);
    }
    CHECK(field(contents(second), "quota") == "1");
    for (int tenths = 0; tenths < 50 && contents(first + ".pushed") != "2 3 "; tenths++) {
        ::usleep(100000);
    }
    CHECK(contents(first + ".pushed") == "2 3 ");
}

TEST_CASE("raw counts disagree past 10% (and past rounding), and totals are this node's own") {
    using paramesh::counts_disagree;
    CHECK_FALSE(counts_disagree(100, 110));  // 10%: not past it
    CHECK(counts_disagree(100, 112));
    CHECK(counts_disagree(112, 100));
    CHECK_FALSE(counts_disagree(3, 4));  // a gap of one is rounding
    CHECK(counts_disagree(0, 5));
    CHECK_FALSE(counts_disagree(0, 0));
    // What this node did for peer 2 is compared with what peer 2 says it got, and so on.
    const paramesh::LedgerTotal mine{NodeId{2}, 50, 80, 0, 0};
    CHECK_FALSE(paramesh::totals_disagree(mine, {NodeId{1}, 80, 50, 0, 0}));
    CHECK(paramesh::totals_disagree(mine, {NodeId{1}, 50, 80, 0, 0}));  // the two crossed

    const paramesh::Platform platform = quiet_platform();
    const std::string dir = ledger_dir();
    auto opened = paramesh::Ledger::open(dir + "/ledger.json", *platform.json);
    REQUIRE(opened.ok());
    paramesh::Ledger ledger = std::move(opened).value();
    using paramesh::Direction;
    ledger.add_chunk(NodeId{2}, paramesh::JobId{9}, Direction::kForPeer, 1, 512,
                     std::chrono::nanoseconds{1});
    ledger.add_chunk(NodeId{2}, paramesh::JobId{9}, Direction::kForPeer, 1, 512,
                     std::chrono::nanoseconds{1});
    ledger.add_segment_seconds(NodeId{2}, paramesh::JobId{9}, Direction::kByPeer, 7.9);
    const paramesh::LedgerTotal totals = paramesh::ledger_totals(ledger, NodeId{2});
    CHECK(totals.chunks_for_peer == 2);
    CHECK(totals.chunks_by_peer == 0);
    CHECK(totals.segsec_by_peer == 7);  // rounded down
    std::filesystem::remove_all(dir);
}

TEST_CASE("forged totals on HELLO are shown and flagged, and move no weight") {
    const Daemon daemon;  // node 5, an empty ledger
    const std::uint16_t port = daemon.pmd.http_port();
    // Node 8 says it ran a billion chunks for node 5's jobs and hosted for them for years.
    paramesh::HelloPayload forged = beacon(81);
    forged.totals.push_back({NodeId{5}, 1000000000, 0, 999999999, 0});
    const auto ledger_of_8 = [&]() -> std::string {
        const auto body = paramesh::http_get(port, "/v1/ledger");
        return body.ok() ? body.value() : std::string{};
    };
    for (int tries = 0; tries < 30 && ledger_of_8().find("\"announced\":{") == std::string::npos;
         tries++) {
        send_beacon(daemon.platform, Opcode::kHello, 8, forged, daemon.pmd.discovery_port());
        ::usleep(100000);
    }
    const std::string said = ledger_of_8();
    INFO(said);
    CHECK(said.find("\"chunks_for_peer\":1000000000") != std::string::npos);  // shown
    CHECK(said.find("\"disagree\":true") != std::string::npos);               // flagged
    CHECK(said.find("\"balance\":0") != std::string::npos);                   // no credit
    CHECK(said.find("\"weight\":1") != std::string::npos);                    // no weight
    // pm status shows it too.
    const std::string out = daemon.state_dir + "/pm";
    REQUIRE(run_tool(tool("pm"), {"status", "--http-port", std::to_string(port)}, out) == 0);
    INFO(contents(out));
    CHECK(contents(out).find("DISAGREES by more than 10%") != std::string::npos);
}

namespace {

// From a daemon's GET /v1/ledger: the entry of `peer`'s first job, or an empty object.
paramesh::JsonObject ledger_job(const Daemon& daemon, std::uint16_t peer) {
    const auto body = paramesh::http_get(daemon.pmd.http_port(), "/v1/ledger");
    const auto parsed = body.ok() ? daemon.platform.json->decode(body.value())
                                  : paramesh::Result<paramesh::JsonValue>{body.error()};
    if (!parsed.ok()) {
        return {};
    }
    const auto& top = std::get<paramesh::JsonObject>(parsed.value().v);
    for (const auto& each : std::get<paramesh::JsonArray>(top.at("peers").v)) {
        const auto& entry = std::get<paramesh::JsonObject>(each.v);
        const auto& jobs = std::get<paramesh::JsonArray>(entry.at("jobs").v);
        if (std::get<std::int64_t>(entry.at("peer").v) == peer && !jobs.empty()) {
            return std::get<paramesh::JsonObject>(jobs.front().v);
        }
    }
    return {};
}

std::int64_t count_in(const paramesh::JsonObject& job, const char* which, const char* key) {
    const auto found = job.find(which);
    const auto* counts =
        found != job.end() ? std::get_if<paramesh::JsonObject>(&found->second.v) : nullptr;
    return counts != nullptr ? std::get<std::int64_t>(counts->at(key).v) : -1;
}

}  // namespace

TEST_CASE("after a job both daemons' /v1/ledger agree, and LEDGER_SYNC says so" *
          doctest::skip(!region_can_be_mapped())) {
    const ThreeNodes nodes;
    const std::string report = nodes.reports + "/program";
    REQUIRE(exit_status(start_pmrun({"-n", "3", "--state-dir", nodes.one.state_dir, own_path(),
                                     "--as-program", report, "quick"}),
                        180) == 0);
    // Each side sends its counts for the job as its process there ends: the workers to node 1,
    // node 1 to each worker. The counts are in /v1/ledger within a second of arriving.
    paramesh::JsonObject by_two;
    paramesh::JsonObject for_one;
    for (int tenths = 0; tenths < 100; tenths++) {
        by_two = ledger_job(nodes.one, 2);
        for_one = ledger_job(nodes.two, 1);
        if (count_in(by_two, "announced", "chunks_for_peer") >= 0 &&
            count_in(for_one, "announced", "chunks_by_peer") >= 0) {
            break;
        }
        ::usleep(100000);
    }
    // Node 1's count of node 2's chunks is node 2's own count, and node 2 announced the same.
    const std::int64_t counted = count_in(by_two, "counts", "chunks_by_peer");
    CHECK(counted > 0);
    CHECK(counted == count_in(for_one, "counts", "chunks_for_peer"));
    CHECK(count_in(by_two, "announced", "chunks_for_peer") == counted);
    CHECK(count_in(for_one, "announced", "chunks_by_peer") == counted);
    CHECK(std::get<bool>(by_two.at("disagree").v) == false);
    CHECK(std::get<bool>(for_one.at("disagree").v) == false);
}
