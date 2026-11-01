#include "platform/sandbox.h"

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace paramesh {
namespace {

// A job's limits. kAddressSpace is the 4 GiB region's mapping, with room for threads and heap.
constexpr rlim_t kAddressSpace = 6ULL << 30U;
constexpr rlim_t kCompilerSpace = 2ULL << 30U;
constexpr rlim_t kCpuSeconds = 120;
constexpr rlim_t kFileBytes = 64ULL << 20U;
constexpr rlim_t kOpenFiles = 512;

// Linux's default ephemeral range: where a job's data-plane ports fall. Nothing a laptop
// usually serves (ssh, web) is in it.
constexpr std::uint64_t kFirstPort = 32768;
constexpr std::uint64_t kLastPort = 60999;
// Inside that range but never a job's: pmd's discovery, control and status ports, and the
// playground's. A job that could reach them could ask a node to start anything.
constexpr std::uint64_t kFirstDaemonPort = 47000;
constexpr std::uint64_t kLastDaemonPort = 47099;

#if defined(__x86_64__)
constexpr std::uint32_t kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t kAuditArch = AUDIT_ARCH_AARCH64;
#else
#error "the sandbox knows x86-64 and AArch64"
#endif

constexpr std::uint64_t kIoctlDev = 1ULL << 15U;  // LANDLOCK_ACCESS_FS_IOCTL_DEV, Linux 6.10

bool limit(int resource, rlim_t value) noexcept {
    const rlimit bound{value, value};
    return ::setrlimit(resource, &bound) == 0;
}

std::uint64_t fs_rights(long abi) noexcept {
    std::uint64_t rights = (LANDLOCK_ACCESS_FS_MAKE_SYM << 1U) - 1;
    rights |= abi >= 2 ? LANDLOCK_ACCESS_FS_REFER : 0;
    rights |= abi >= 3 ? LANDLOCK_ACCESS_FS_TRUNCATE : 0;
    rights |= abi >= 5 ? kIoctlDev : 0;
    return rights;
}

// Rights on a file may only be file rights, so a rule on one must leave the directory ones out.
bool allow_path(int ruleset, const char* path, std::uint64_t rights) noexcept {
    const int fd = ::open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT;  // /lib64 and the like, where this system has none
    }
    const landlock_path_beneath_attr rule{rights, fd};
    const long added =
        ::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    ::close(fd);
    return added == 0;
}

bool allow_port(int ruleset, std::uint64_t port, std::uint64_t rights) noexcept {
    const landlock_net_port_attr rule{rights, port};
    return ::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_NET_PORT, &rule, 0) == 0;
}

bool apply_landlock(const char* directory, SandboxProfile profile) noexcept {
    const long abi =
        ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 4) {
        return false;
    }
    const std::uint64_t everything = fs_rights(abi);
    const std::uint64_t read = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    const std::uint64_t run = read | LANDLOCK_ACCESS_FS_EXECUTE;
    const landlock_ruleset_attr attr{
        everything, LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP};
    const long ruleset = ::syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0);
    if (ruleset < 0) {
        return false;
    }
    const int rules = static_cast<int>(ruleset);
    bool ok = allow_path(rules, directory, everything) && allow_path(rules, "/proc", read) &&
              allow_path(rules, "/dev/null",
                         LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE) &&
              allow_path(rules, "/dev/urandom", LANDLOCK_ACCESS_FS_READ_FILE);
    if (profile == SandboxProfile::kCompiler) {
        for (const char* path :
             {"/usr", "/lib", "/lib64", "/bin", "/opt/paramesh", "/etc/alternatives"}) {
            ok = ok && allow_path(rules, path, run);
        }
        ok = ok && allow_path(rules, "/etc/ld.so.cache", LANDLOCK_ACCESS_FS_READ_FILE);
    } else {
        ok = ok && allow_port(rules, 0, LANDLOCK_ACCESS_NET_BIND_TCP);
        for (std::uint64_t port = kFirstPort; ok && port <= kLastPort; port++) {
            if (port < kFirstDaemonPort || port > kLastDaemonPort) {
                ok = allow_port(rules, port,
                                LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP);
            }
        }
    }
    ok = ok && ::syscall(SYS_landlock_restrict_self, rules, 0) == 0;
    ::close(rules);
    return ok;
}

// A seccomp filter, built on the stack.
class Filter {
public:
    void statement(std::uint16_t code, std::uint32_t k) noexcept {
        program_.at(size_++) = sock_filter{code, 0, 0, k};
    }
    void jump(std::uint16_t code, std::uint32_t k, std::uint8_t yes, std::uint8_t no) noexcept {
        program_.at(size_++) = sock_filter{code, yes, no, k};
    }
    void load_nr() noexcept { statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)); }
    void load_arg(std::size_t arg) noexcept {
        statement(BPF_LD | BPF_W | BPF_ABS,
                  static_cast<std::uint32_t>(offsetof(seccomp_data, args) + 8 * arg));
    }
    void deny() noexcept { statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM); }
    void allow() noexcept { statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW); }
    void deny_call(long nr, std::uint16_t answer = EPERM) noexcept {
        jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(nr), 0, 1);
        statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | answer);
    }
    // A call that names a process: only this one.
    void own_signal_only(long nr, std::uint32_t self) noexcept {
        jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(nr), 0, 4);
        load_arg(0);
        jump(BPF_JMP | BPF_JEQ | BPF_K, self, 1, 0);
        deny();
        allow();
    }
    bool install() noexcept {
        const sock_fprog program{static_cast<unsigned short>(size_), program_.data()};
        return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
    }

private:
    std::array<sock_filter, 96> program_{};
    std::size_t size_ = 0;
};

bool apply_seccomp() noexcept {
    Filter f;
    f.statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
    f.jump(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0);
    f.statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    f.load_nr();
    for (const long nr :
         {SYS_ptrace, SYS_mount, SYS_umount2, SYS_pivot_root, SYS_chroot, SYS_setns, SYS_unshare,
          SYS_bpf, SYS_perf_event_open, SYS_keyctl, SYS_add_key, SYS_request_key,
          SYS_io_uring_setup, SYS_process_vm_writev, SYS_kexec_load, SYS_init_module,
          SYS_finit_module, SYS_memfd_create, SYS_seccomp}) {
        f.deny_call(nr);
    }
#ifdef SYS_fork
    f.deny_call(SYS_fork);
    f.deny_call(SYS_vfork);
#endif
    // clone3 hides its flags from a filter. ENOSYS makes glibc use clone for its threads.
    f.deny_call(SYS_clone3, ENOSYS);
    const auto self = static_cast<std::uint32_t>(::getpid());
    f.own_signal_only(SYS_kill, self);
    f.own_signal_only(SYS_tgkill, self);
    f.own_signal_only(SYS_process_vm_readv, self);  // how the memory engine reads its pages
    // clone: threads only.
    f.jump(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 4);
    f.load_arg(0);
    f.jump(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0);
    f.deny();
    f.allow();
    // socket: local sockets, and TCP over IPv4.
    f.jump(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 9);
    f.load_arg(0);
    f.jump(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 6, 0);
    f.jump(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 1, 0);
    f.deny();
    f.load_arg(1);
    f.statement(BPF_ALU | BPF_AND | BPF_K, 0xF);  // the type, without its flags
    f.jump(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 1, 0);
    f.deny();
    f.allow();
    f.allow();  // every other call
    return f.install();
}

}  // namespace

bool sandbox_enter(const char* directory, SandboxProfile profile) noexcept {
    const bool job = profile == SandboxProfile::kJob;
    if (!limit(RLIMIT_CORE, 0) || !limit(RLIMIT_AS, job ? kAddressSpace : kCompilerSpace) ||
        !limit(RLIMIT_CPU, kCpuSeconds) || !limit(RLIMIT_FSIZE, kFileBytes) ||
        !limit(RLIMIT_NOFILE, kOpenFiles) || ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return false;
    }
    if (!apply_landlock(directory, profile)) {
        return false;
    }
    return !job || apply_seccomp();
}

}  // namespace paramesh
