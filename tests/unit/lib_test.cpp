// src/lib/: three job processes on localhost run the M1 program, started the way
// docs/PROTOCOL.md section 10 describes for a job without pmd: by environment variables.
//
// The processes map the real region, so these tests are skipped under ThreadSanitizer and
// where user-mode userfaultfd is missing, as in mem_test.cpp.

#include "lib/test_hook.h"
#include "platform/ids.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <netinet/in.h>
#include <paramesh.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// The environment is read and set only in processes that have one thread at that moment.
// NOLINTBEGIN(concurrency-mt-unsafe)

namespace {

constexpr std::uint64_t kNumbers = 6ULL * 131072;  // 6 MiB of 64-bit numbers: three segments
constexpr std::uint64_t kRegionBytes = 16ULL << 20U;

bool can_run() {
    if (std::string_view{PARAMESH_SANITIZER_NAME} == "thread") {
        return false;
    }
    const long fd = ::syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
    if (fd >= 0) {
        ::close(static_cast<int>(fd));
    }
    return fd >= 0;
}

std::uint64_t* numbers() {
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the first allocation is at the region's base
    return reinterpret_cast<std::uint64_t*>(paramesh::kRegionBase);
}

std::uint64_t expected(std::uint64_t i) {
    return i * i + 1;
}

// What every worker runs once the launcher has written the array.
void check_numbers() {
    for (std::uint64_t i = 0; i < kNumbers; i++) {
        if (numbers()[i] != expected(i)) {
            pm_test_fail("a worker read a wrong number");
        }
    }
}

// As check_numbers, but node 3 dies half way through.
void check_numbers_and_die() {
    for (std::uint64_t i = 0; i < kNumbers; i++) {
        if (i == kNumbers / 2 && std::string_view{std::getenv("PARAMESH_NODE_ID")} == "3") {
            ::kill(::getpid(), SIGKILL);
        }
        if (numbers()[i] != expected(i)) {
            pm_test_fail("a worker read a wrong number");
        }
    }
}

// The M1 program. On a worker pm_init() never returns.
int program() {
    pm_test_register(0, check_numbers);
    pm_test_register(1, check_numbers_and_die);
    pm_config config{};
    config.region_bytes = kRegionBytes;
    if (pm_init(nullptr, nullptr, &config) != PM_OK) {
        return 2;
    }
    auto* mine = static_cast<std::uint64_t*>(pm_malloc(kNumbers * sizeof(std::uint64_t)));
    if (mine != numbers()) {
        return 3;
    }
    for (std::uint64_t i = 0; i < kNumbers; i++) {
        mine[i] = expected(i);
    }
    const char* function = std::getenv("TEST_FUNCTION");
    if (pm_test_run_on_workers(function != nullptr && *function == '1' ? 1 : 0) != PM_OK) {
        return 4;
    }
    for (std::uint64_t i = 0; i < kNumbers; i++) {  // the launcher can still read its own data
        if (mine[i] != expected(i)) {
            return 5;
        }
    }
    return pm_finalize() == PM_OK ? 0 : 6;
}

std::uint16_t free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof addr;
    REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    ::close(fd);
    return ntohs(addr.sin_port);
}

struct Started {
    std::array<pid_t, 3> pids{};
    int launcher_stderr = -1;
};

// Starts node 1 as the launcher and nodes 2 and 3 as workers, each a process running program().
Started start_job(const char* function) {
    const std::array<std::uint16_t, 3> ports = {free_port(), free_port(), free_port()};
    std::string peers;
    for (std::size_t i = 0; i < ports.size(); i++) {
        peers += (i == 0 ? "" : ",") + std::to_string(i + 1) +
                 "@127.0.0.1:" + std::to_string(ports.at(i));
    }
    std::array<int, 2> err{};
    REQUIRE(::pipe(err.data()) == 0);
    Started started;
    started.launcher_stderr = err[0];
    for (std::size_t i = 0; i < ports.size(); i++) {
        const pid_t pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            ::setenv("PARAMESH_ROLE", i == 0 ? "launcher" : "worker", 1);
            ::setenv("PARAMESH_JOB_ID", "42", 1);
            ::setenv("PARAMESH_NODE_ID", std::to_string(i + 1).c_str(), 1);
            ::setenv("PARAMESH_LISTEN", ("127.0.0.1:" + std::to_string(ports.at(i))).c_str(), 1);
            ::setenv("PARAMESH_PEERS", peers.c_str(), 1);
            ::setenv("TEST_FUNCTION", function, 1);
            if (i == 0) {
                ::dup2(err[1], STDERR_FILENO);
            }
            ::_exit(program());
        }
        started.pids.at(i) = pid;
    }
    ::close(err[1]);
    return started;
}

// Exit status of a child, or -1 if it did not exit by itself within the time allowed.
int wait_for(pid_t pid, int seconds) {
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

std::string read_all(int fd) {
    std::string text;
    std::array<char, 1024> chunk{};
    for (ssize_t got = 0; (got = ::read(fd, chunk.data(), chunk.size())) > 0;) {
        text.append(chunk.data(), static_cast<std::size_t>(got));
    }
    ::close(fd);
    return text;
}

}  // namespace

TEST_CASE(
    "three processes on localhost complete the M1 program: two workers read what the launcher "
    "wrote" *
    doctest::skip(!can_run())) {
    const Started job = start_job("0");
    CHECK(wait_for(job.pids[0], 60) ==
          0);  // the launcher: wrote, had it read, read it back, ended the job
    CHECK(wait_for(job.pids[1], 10) == 0);  // each worker: every number was right, then JOB_END
    CHECK(wait_for(job.pids[2], 10) == 0);
    CHECK(read_all(job.launcher_stderr).empty());
}

TEST_CASE("a lost node makes every other process exit with a message" * doctest::skip(!can_run())) {
    const Started job = start_job("1");  // node 3 kills itself while reading
    CHECK(wait_for(job.pids[2], 60) == 128 + SIGKILL);
    CHECK(wait_for(job.pids[0], 10) == 1);
    CHECK(wait_for(job.pids[1], 10) == 1);
    CHECK(read_all(job.launcher_stderr) == "job 42 aborted: node 3 lost\n");
}

TEST_CASE("pm_init without the launch environment reports a configuration error") {
    ::unsetenv("PARAMESH_ROLE");
    CHECK(pm_init(nullptr, nullptr, nullptr) == PM_ERR_CONFIG);
    CHECK(pm_finalize() == PM_ERR_STATE);
    CHECK(pm_malloc(64) == nullptr);
    CHECK(std::string_view{pm_strerror(PM_ERR_CONFIG)} ==
          "the launch environment is missing or malformed");
    CHECK(std::string_view{pm_strerror(-99)} == "unknown error");
}

// NOLINTEND(concurrency-mt-unsafe)
