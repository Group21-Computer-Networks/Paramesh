// The per-node daemon, minimal form (M3-4, M3-5). docs/PROTOCOL.md, sections 6, 8.1, 9, 10.
//
// For a job launched elsewhere: on SPAWN_REQ from another daemon it starts a worker process,
// and answers SPAWN_OK when the worker has registered.
// For a job launched here: it gives pmrun a job ID (L_RUN_REQ); when the launcher asks for
// admission (L_ADMIT_REQ) it asks its peers, in the order of --peers, for N-1 workers, takes
// the next peer for each that declines, and answers with the members (L_ADMIT_OK). If pmrun
// goes away while the job runs, the launcher is told to end it (L_ABORT).
// Every job process that registers on the local socket is told its thread quota.
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
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

    // A job launched on this node, from L_RUN_REQ until its launcher has gone.
    struct Run {
        JobId job;
        LRunReqPayload request;
        std::uint64_t pmrun = 0;  // connection IDs; 0 when there is none
        std::uint64_t launcher = 0;
        LAdmitReqPayload wants;             // from L_ADMIT_REQ
        std::vector<SegMapMember> members;  // the launcher first
        std::size_t next_peer = 0;          // index in --peers of the next peer to ask
        std::size_t asked = 0;              // SPAWN_REQs not yet answered
        bool admitting = false;
    };
    // One SPAWN_REQ this daemon sent and has no answer to.
    struct Ask {
        JobId job;
        std::uint64_t connection = 0;
        Endpoint peer;
    };

    void on_run_req(Connection& connection, const FrameHeader& header,
                    std::span<const std::byte> payload);
    void on_admit_req(Connection& connection, const FrameHeader& header,
                      std::span<const std::byte> payload);
    void on_spawn_answer(const FrameHeader& header, std::span<const std::byte> payload);
    void on_closed(const Connection& connection);
    void ask_peers(Run& run);
    void ask_next_peer(Run& run);
    void finish_admission(Run& run);
    std::uint32_t next_job_number();
    Run* run_of(JobId job);

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
    std::deque<Connection> connections_;  // a deque: handling a frame may add one
    std::vector<Spawn> spawns_;
    std::vector<pid_t> children_;  // every worker started and not yet collected
    std::vector<Run> runs_;
    std::unordered_map<std::uint64_t, Ask> asks_;  // by the SPAWN_REQ's request number
    std::uint64_t next_req_ = 1;
};

}  // namespace paramesh

#endif  // PARAMESH_PMD_PMD_H
