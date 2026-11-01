// The playground's sandbox (X-playground): what an untrusted program may do once it runs.
// Applied before exec, so it holds from the first instruction. It uses Landlock, seccomp and
// rlimits, which need no privilege and no namespaces, so userfaultfd and the job's own process
// ID and executable hash stay as pmd expects.
#ifndef PARAMESH_PLATFORM_SANDBOX_H
#define PARAMESH_PLATFORM_SANDBOX_H

#include <cstdint>
#include <string_view>

namespace paramesh {

// Where playground jobs live, the same path on every node. A program under it is a job.
inline constexpr std::string_view kSandboxRoot = "/var/tmp/paramesh-play/";

[[nodiscard]] constexpr bool sandbox_path(std::string_view path) noexcept {
    return path.starts_with(kSandboxRoot);
}

enum class SandboxProfile : std::uint8_t {
    kJob,       // its directory only; threads, no new processes; high TCP ports, not the daemons'
    kCompiler,  // also reads and runs the toolchain; new processes; no TCP
};

// Confines this process and what it runs, for good. `directory` is the only place it can write.
// Only system calls, so it is safe after fork(). False if it could not all be applied: the
// caller must then not run the program. Needs Landlock ABI 4 (Linux 6.7).
[[nodiscard]] bool sandbox_enter(const char* directory, SandboxProfile profile) noexcept;

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_SANDBOX_H
