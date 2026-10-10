#include "tools/pmrun.h"

#include "platform/factory.h"
#include "pmd/frame_io.h"
#include "wire/payloads.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace paramesh {

namespace {

constexpr int kStopWaitTenths = 50;  // how long an interrupted launcher gets to end the job

// Set by SIGINT and SIGTERM.
volatile std::sig_atomic_t& interrupted() {
    static volatile std::sig_atomic_t flag = 0;
    return flag;
}

void on_signal(int /*signal*/) {
    interrupted() = 1;
}

int fail(int status, const std::string& what) {
    static_cast<void>(std::fprintf(stderr, "pmrun: %s\n", what.c_str()));
    return status;
}

const char* usage() {
    return "usage: pmrun [-n nodes] [--state-dir DIRECTORY] program [arguments...]\n"
           "  -n           nodes to run on, 1 to 8 (default 1)\n"
           "  --state-dir  the local pmd's state directory, where its pmd.sock is\n"
           "               (default: $PARAMESH_PMD_SOCKET, else ~/.local/state/paramesh)";
}

// The environment is read and changed only while this process has one thread.
// NOLINTBEGIN(concurrency-mt-unsafe)
std::string socket_path(const std::string& state_dir) {
    if (!state_dir.empty()) {
        return state_dir + "/pmd.sock";
    }
    if (const char* given = std::getenv("PARAMESH_PMD_SOCKET")) {
        return given;
    }
    const char* home = std::getenv("HOME");
    return std::string{home != nullptr ? home : "."} + "/.local/state/paramesh/pmd.sock";
}

// The child: becomes the launcher, with the environment of docs/PROTOCOL.md section 10.
[[noreturn]] void become_launcher(const LRunOkPayload& job, const std::string& socket,
                                  const std::string& path, std::vector<std::string>& arguments) {
    ::setenv("PARAMESH_ROLE", "launcher", 1);
    ::setenv("PARAMESH_JOB_ID", std::to_string(job.job.value).c_str(), 1);
    ::setenv("PARAMESH_NODE_ID", std::to_string(job.node.value).c_str(), 1);
    ::setenv("PARAMESH_PMD_SOCKET", socket.c_str(), 1);
    for (const char* stale : {"PARAMESH_LISTEN", "PARAMESH_PEERS", "PARAMESH_LAUNCHER"}) {
        ::unsetenv(stale);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    ::execv(path.c_str(), argv.data());
    static_cast<void>(std::fprintf(stderr, "pmrun: cannot start %s\n", path.c_str()));
    ::_exit(127);
}
// NOLINTEND(concurrency-mt-unsafe)

// Waits for the launcher. If a signal arrives first, lets go of pmd, which tells the launcher
// to end the job on every node, and gives it a while before killing it.
int wait_for_launcher(pid_t launcher, int pmd) {
    int status = 0;
    while (::waitpid(launcher, &status, 0) != launcher) {
        if (errno != EINTR || interrupted() == 0) {
            continue;
        }
        ::close(pmd);
        for (int tenths = 0; tenths < kStopWaitTenths; tenths++) {
            if (::waitpid(launcher, &status, WNOHANG) == launcher) {
                return 130;
            }
            ::usleep(100000);
        }
        ::kill(launcher, SIGKILL);
        ::waitpid(launcher, &status, 0);
        return 130;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

}  // namespace

int pmrun(std::span<const std::string_view> args) {
    unsigned nodes = 1;
    std::string state_dir;
    std::size_t at = 0;
    for (; at + 1 < args.size() && args[at].starts_with('-'); at += 2) {
        const std::string_view value = args[at + 1];
        if (args[at] == "-n") {
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), nodes);
            if (error != std::errc{} || end != value.data() + value.size() || nodes < 1 ||
                nodes > 8) {
                return fail(2, std::string{"-n must be 1 to 8\n"} + usage());
            }
        } else if (args[at] == "--state-dir") {
            state_dir = value;
        } else {
            return fail(2, std::string{"unknown flag\n"} + usage());
        }
    }
    if (at >= args.size() || args[at].starts_with('-')) {
        return fail(2, std::string{"no program to run\n"} + usage());
    }

    // The program, as every node will find it: by its absolute path.
    std::array<char, PATH_MAX> resolved{};
    std::array<char, PATH_MAX> cwd{};
    const std::string program{args[at]};
    if (::realpath(program.c_str(), resolved.data()) == nullptr ||
        ::access(resolved.data(), X_OK) != 0 || ::getcwd(cwd.data(), cwd.size()) == nullptr) {
        return fail(2, "no program at " + program);
    }
    const std::string path = resolved.data();
    Result<Platform> platform =
        platform_open({}, PlatformOptions{kNoNode, JobId{0}, 2, LogLevel::kWarn});
    std::ifstream file{path, std::ios::binary};
    const std::vector<char> image{std::istreambuf_iterator<char>{file},
                                  std::istreambuf_iterator<char>{}};
    if (!platform.ok() || image.empty()) {
        return fail(1, "cannot read " + path);
    }

    LRunReqPayload request;
    request.nodes = static_cast<std::uint16_t>(nodes);
    request.binary_hash = platform.value().hasher->sha256(std::as_bytes(std::span{image}));
    request.path = path;
    request.cwd = cwd.data();
    request.argv.assign(args.begin() + static_cast<std::ptrdiff_t>(at) + 1, args.end());

    const std::string socket = socket_path(state_dir);
    sockaddr_un to{};
    to.sun_family = AF_UNIX;
    const int pmd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket.size() < sizeof to.sun_path) {
        std::copy(socket.begin(), socket.end(), std::begin(to.sun_path));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (pmd < 0 || ::connect(pmd, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0) {
        return fail(1, "cannot reach pmd at " + socket + "; is it running on this node?");
    }
    std::vector<std::byte> bytes(kMaxSpawnReq);
    const Result<std::size_t> size = wire_encode(request, bytes);
    FrameHeader header;
    header.opcode = Opcode::kLRunReq;
    const Checksum& checksum = *platform.value().checksum;
    if (!size.ok() ||
        !frame_write(pmd, checksum, header, std::span{bytes}.first(size.value())).ok()) {
        return fail(1, "cannot ask pmd for a job");
    }
    const Result<Frame> answer = frame_read(pmd, checksum);
    if (!answer.ok()) {
        return fail(1, "pmd gave no answer");
    }
    if (answer.value().header.opcode != Opcode::kLRunOk) {
        const Result<StatusTextPayload> refused = wire_decode_spawn_decline(answer.value().payload);
        return fail(1, "pmd refused the job: " + (refused.ok() ? refused.value().message
                                                               : std::string{"no reason given"}));
    }
    const Result<LRunOkPayload> job = wire_decode_l_run_ok(answer.value().payload);
    if (!job.ok()) {
        return fail(1, "pmd gave an answer pmrun cannot read");
    }

    std::vector<std::string> arguments{path};
    arguments.insert(arguments.end(), request.argv.begin(), request.argv.end());
    struct sigaction action {};
    action.sa_handler = on_signal;  // no SA_RESTART: waitpid() must return when it arrives
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    const pid_t launcher = ::fork();
    if (launcher == 0) {
        become_launcher(job.value(), socket, path, arguments);
    }
    if (launcher < 0) {
        return fail(1, "cannot start a process");
    }
    return wait_for_launcher(launcher, pmd);
}

}  // namespace paramesh
