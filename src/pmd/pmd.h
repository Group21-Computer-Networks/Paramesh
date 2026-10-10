// The per-node daemon, minimal form (M3-4): it listens on the daemon control channel for
// SPAWN_REQ and starts a worker process for the job, and on the local socket for the
// L_REGISTER of job processes, to which it answers with their thread quota.
// docs/PROTOCOL.md, sections 6, 9 and 10.
//
// One thread runs everything (run()). Exceptions are allowed here and are caught where the
// thread starts, in main.cpp.
#ifndef PARAMESH_PMD_PMD_H
#define PARAMESH_PMD_PMD_H

#include "net/transport.h"
#include "platform/factory.h"
#include "platform/ids.h"
#include "platform/result.h"
#include "wire/frame.h"
#include "wire/payloads.h"

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paramesh {

inline constexpr std::uint16_t kControlPort = 47001;

struct PmdConfig {
    NodeId node;                                // --node-id, 1 to 65534; required until M4-1
    std::uint16_t control_port = kControlPort;  // --control-port; 0 lets the system choose
    std::string state_dir;                      // --state-dir; pmd.sock is made in it
    std::uint16_t cap_cores = 0;                // --cap-cores: the thread quota of every job
    std::vector<Endpoint> peers;                // --peers address[:port],...
};

// Reads the command line, without the program name. Missing settings get their defaults:
// the state directory is ~/.local/state/paramesh and the cap is this machine's processors.
// Errc::kInvalidArgument, with the fault in Error::what, for anything it cannot use.
Result<PmdConfig> pmd_parse_args(std::span<const std::string_view> args);
// The text printed for --help and after a bad command line.
std::string_view pmd_usage() noexcept;

class Pmd {
public:
    // `platform` must outlive this.
    Pmd(PmdConfig config, const Platform& platform);
    Pmd(const Pmd&) = delete;
    Pmd& operator=(const Pmd&) = delete;
    Pmd(Pmd&&) = delete;
    Pmd& operator=(Pmd&&) = delete;
    ~Pmd();

    // Makes the state directory and the local socket and starts listening on both channels.
    Result<void> open();
    [[nodiscard]] std::uint16_t control_port() const noexcept { return config_.control_port; }
    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

    // Serves until stop(). stop() may be called from any thread and from a signal handler.
    void run();
    void stop() const noexcept;

private:
    struct Connection {
        std::uint64_t id = 0;
        int fd = -1;
        bool local = false;  // on the local socket, not the control channel
        std::vector<std::byte> in;
    };
    // A worker this daemon started and has not yet heard L_REGISTER from.
    struct Spawn {
        JobId job;
        pid_t pid = 0;
        std::uint64_t asked_on = 0;  // the control connection the SPAWN_REQ came on
        NodeId asker;
        ReqId req;
        std::array<std::byte, 32> hash{};
    };

    void accept_on(int listener, bool local);
    bool read_from(Connection& connection);
    void handle(Connection& connection, const FrameHeader& header,
                std::span<const std::byte> payload);
    void on_spawn_req(Connection& connection, const FrameHeader& header,
                      std::span<const std::byte> payload);
    void on_register(Connection& connection, const FrameHeader& header,
                     std::span<const std::byte> payload);
    void reap();
    template <typename Payload>
    void reply(std::uint64_t connection, Opcode opcode, JobId job, NodeId to, ReqId req,
               const Payload& payload);
    void decline(const Spawn& spawn, Status status, const std::string& message);

    PmdConfig config_;
    const Platform& platform_;
    std::string socket_path_;
    int control_listener_ = -1;
    int local_listener_ = -1;
    int stop_fd_ = -1;
    std::uint64_t next_connection_ = 1;
    std::vector<Connection> connections_;
    std::vector<Spawn> spawns_;
    std::vector<pid_t> children_;  // every worker started and not yet collected
};

}  // namespace paramesh

#endif  // PARAMESH_PMD_PMD_H
