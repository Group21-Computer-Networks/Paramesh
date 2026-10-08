// src/lib/: three job processes on localhost run the M1 program (read sharing) and the M2
// program (write sharing, locks, a barrier), started the way docs/PROTOCOL.md section 10
// describes for a job without pmd: by environment variables.
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
#include <cerrno>
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

// ---- the M2 program: every node writes one shared page -----------------------------------------

constexpr std::uint64_t kAdds = 1000;

// The first thing in the region. All of it is in one page, on purpose.
struct Shared {
    pm_lock_t lock;
    pm_barrier_t barrier;
    std::uint64_t counter;                 // changed only under `lock`
    std::array<std::uint64_t, 4> own;      // own[n] is written by node n alone, without a lock
    std::array<std::uint64_t, 4> arrived;  // arrived[n]: node n reached the barrier
};

Shared* shared() {
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the first allocation is at the region's base
    return reinterpret_cast<Shared*>(paramesh::kRegionBase);
}

std::size_t node_number() {
    const char* id = std::getenv("PARAMESH_NODE_ID");
    return id != nullptr ? static_cast<std::size_t>(std::strtoul(id, nullptr, 10)) : 0;
}

void add_with_lock() {
    for (std::uint64_t i = 0; i < kAdds; i++) {
        if (pm_lock(shared()->lock) != PM_OK) {
            pm_test_fail("pm_lock failed");
        }
        shared()->counter = shared()->counter + 1;
        if (pm_unlock(shared()->lock) != PM_OK) {
            pm_test_fail("pm_unlock failed");
        }
    }
}

// No lock: each node has its own number, but they share a page, so the page changes owner
// under the writers again and again and no write may be lost on the way.
void add_to_own_number() {
    volatile std::uint64_t* mine = &shared()->own.at(node_number());
    for (std::uint64_t i = 0; i < kAdds; i++) {
        *mine = *mine + 1;
    }
}

void meet_at_the_barrier() {
    shared()->arrived.at(node_number()) = 1;
    if (pm_barrier_wait(shared()->barrier) != PM_OK) {
        pm_test_fail("pm_barrier_wait failed");
    }
    if (shared()->arrived.at(2) != 1 || shared()->arrived.at(3) != 1) {
        pm_test_fail("a worker passed the barrier before the other reached it");
    }
}

// What paramesh.h says each wrong call returns, checked on a worker.
void misuse_a_lock() {
    const pm_lock_t lock = shared()->lock;
    const bool as_documented =
        pm_lock_create().id == 0 &&  // launcher only
        pm_barrier_create(2).id == 0 &&
        pm_lock(pm_lock_t{0}) == PM_ERR_INVALID &&  // all zero bytes is never a lock
        pm_unlock(lock) == PM_ERR_INVALID &&        // not held
        pm_lock(lock) == PM_OK && pm_lock(lock) == PM_ERR_INVALID &&  // not recursive
        pm_unlock(lock) == PM_OK &&
        pm_lock(pm_lock_t{987654}) == PM_ERR_INVALID &&  // the launcher knows no such lock
        pm_lock(pm_lock_t{shared()->barrier.id}) == PM_ERR_INVALID &&  // a barrier is not a lock
        pm_barrier_wait(pm_barrier_t{lock.id}) == PM_ERR_INVALID;
    if (!as_documented) {
        pm_test_fail("a wrong lock or barrier call did not return what paramesh.h says");
    }
}

// The launcher's side. Returns 0, or the number of the step that went wrong.
int write_program() {
    pm_test_register(2, add_with_lock);
    pm_test_register(3, add_to_own_number);
    pm_test_register(4, meet_at_the_barrier);
    pm_test_register(5, misuse_a_lock);
    pm_config config{};
    config.region_bytes = kRegionBytes;
    if (pm_lock_create().id != 0 || pm_lock(pm_lock_t{1}) != PM_ERR_STATE) {
        return 1;  // before pm_init() there is no job
    }
    if (pm_init(nullptr, nullptr, &config) != PM_OK) {
        return 2;
    }
    if (pm_malloc(sizeof(Shared)) != shared()) {
        return 3;
    }
    shared()->lock = pm_lock_create();
    shared()->barrier = pm_barrier_create(2);
    if (shared()->lock.id == 0 || shared()->barrier.id == 0) {
        return 4;
    }
    if (pm_test_run_on_workers(5) != PM_OK) {
        return 5;
    }
    if (pm_test_run_on_workers(2) != PM_OK || shared()->counter != 2 * kAdds) {
        return 6;  // two workers, kAdds each, under the lock
    }
    add_with_lock();  // the launcher takes the lock from itself and the page from a worker
    if (shared()->counter != 3 * kAdds) {
        return 7;
    }
    if (pm_test_run_on_workers(3) != PM_OK || shared()->own.at(2) != kAdds ||
        shared()->own.at(3) != kAdds || shared()->counter != 3 * kAdds) {
        return 8;
    }
    if (pm_test_run_on_workers(4) != PM_OK) {
        return 9;
    }
    return pm_finalize() == PM_OK ? 0 : 10;
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

// Starts node 1 as the launcher and nodes 2 and 3 as workers, each a process running `run`.
Started start_job(const char* function, int (*run)() = program) {
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
            ::_exit(run());
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

TEST_CASE("three processes write one shared page and lose no update, with and without a lock" *
          doctest::skip(!can_run())) {
    const Started job = start_job("0", write_program);
    CHECK(wait_for(job.pids[0], 120) == 0);  // every total was exact
    CHECK(wait_for(job.pids[1], 10) == 0);
    CHECK(wait_for(job.pids[2], 10) == 0);
    CHECK(read_all(job.launcher_stderr).empty());
}

// A job of one node, in a child process: a system call on shared memory before and after
// pm_touch. Returns 0 if everything was as paramesh.h says, else the number of the step that
// was not.
int touch_program() {
    ::setenv("PARAMESH_ROLE", "launcher", 1);
    ::setenv("PARAMESH_JOB_ID", "43", 1);
    ::setenv("PARAMESH_NODE_ID", "1", 1);
    const std::string here = "127.0.0.1:" + std::to_string(free_port());
    ::setenv("PARAMESH_LISTEN", here.c_str(), 1);
    ::setenv("PARAMESH_PEERS", ("1@" + here).c_str(), 1);
    pm_config config{};
    config.region_bytes = kRegionBytes;
    if (pm_init(nullptr, nullptr, &config) != PM_OK) {
        return 1;
    }
    auto* buffer = static_cast<char*>(pm_malloc(4 * paramesh::kPageSize));
    std::array<char, 16> name = {"/tmp/pm-XXXXXX"};
    const int file = ::mkstemp(name.data());
    ::unlink(name.data());
    const std::string_view text = "read through the kernel";
    if (buffer == nullptr || file < 0 ||
        ::pwrite(file, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size())) {
        return 2;
    }

    // The kernel is asked to write into a shared page this node does not hold.
    errno = 0;
    if (::pread(file, buffer, text.size(), 0) != -1 || errno != EFAULT) {
        return 3;
    }
    if (pm_touch(buffer, text.size(), PM_ACCESS_WRITE) != PM_OK) {
        return 4;
    }
    if (::pread(file, buffer, text.size(), 0) != static_cast<ssize_t>(text.size()) ||
        std::string_view{buffer, text.size()} != text) {
        return 5;
    }

    // The kernel is asked to read from one: the same, with read access. An untouched page reads as
    // zeros.
    char* second = buffer + paramesh::kPageSize;
    errno = 0;
    if (::pwrite(file, second, 8, 100) != -1 || errno != EFAULT) {
        return 6;
    }
    if (pm_touch(second, 8, PM_ACCESS_READ) != PM_OK || ::pwrite(file, second, 8, 100) != 8) {
        return 7;
    }

    // A range over a page boundary is brought in whole; bad arguments are refused.
    char* across = buffer + 3 * paramesh::kPageSize -
                   4;  // the last 4 bytes of one page, the first 4 of the next
    if (pm_touch(across, 8, PM_ACCESS_WRITE) != PM_OK || ::pread(file, across, 8, 0) != 8) {
        return 8;
    }
    if (pm_touch(buffer, 0, PM_ACCESS_READ) != PM_OK ||
        pm_touch(&config, 8, PM_ACCESS_READ) != PM_ERR_INVALID ||
        pm_touch(buffer, kRegionBytes + 1, PM_ACCESS_READ) != PM_ERR_INVALID) {
        return 9;
    }
    return pm_finalize() == PM_OK ? 0 : 10;
}

TEST_CASE("a system call on a missing shared page fails with EFAULT, and succeeds after pm_touch" *
          doctest::skip(!can_run())) {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::_exit(touch_program());
    }
    CHECK(wait_for(child, 30) == 0);
}

TEST_CASE("pm_init without the launch environment reports a configuration error") {
    ::unsetenv("PARAMESH_ROLE");
    CHECK(pm_init(nullptr, nullptr, nullptr) == PM_ERR_CONFIG);
    CHECK(pm_finalize() == PM_ERR_STATE);
    CHECK(pm_malloc(64) == nullptr);
    CHECK(pm_touch(numbers(), 8, PM_ACCESS_READ) == PM_ERR_STATE);
    CHECK(std::string_view{pm_strerror(PM_ERR_CONFIG)} ==
          "the launch environment is missing or malformed");
    CHECK(std::string_view{pm_strerror(-99)} == "unknown error");
}

// NOLINTEND(concurrency-mt-unsafe)
