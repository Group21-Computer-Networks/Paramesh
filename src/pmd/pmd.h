// The per-node daemon, minimal form (M3-4, M3-5). docs/PROTOCOL.md, sections 6, 8.1, 9, 10.
//
// For a job launched elsewhere: on SPAWN_REQ from another daemon it starts a worker process,
// and answers SPAWN_OK when the worker has registered.
// For a job launched here: it gives pmrun a job ID (L_RUN_REQ); when the launcher asks for
// admission (L_ADMIT_REQ) it asks its peers, in the order of --peers, for N-1 workers, takes
// the next peer for each that declines, and answers with the members (L_ADMIT_OK). If pmrun
// goes away while the job runs, the launcher is told to end it (L_ABORT).
// Every job process that registers on the local socket is told its thread quota.
// Discovery (M4-1): a HELLO every second on UDP multicast, and to every seed in --peers; the
// peers heard are kept in a PeerTable and served as JSON at GET /v1/peers on 127.0.0.1.
//
// One thread runs everything (run()). Exceptions are allowed here and are caught where the
// thread starts, in main.cpp.
#ifndef PARAMESH_PMD_PMD_H
#define PARAMESH_PMD_PMD_H

#include "net/transport.h"
#include "platform/factory.h"
#include "platform/ids.h"
#include "platform/result.h"
#include "pmd/ledger.h"
#include "pmd/peers.h"
#include "pmd/scheduler.h"
#include "wire/frame.h"
#include "wire/payloads.h"

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace paramesh {

inline constexpr std::uint16_t kDiscoveryPort = 47000;
inline constexpr std::uint16_t kControlPort = 47001;
inline constexpr std::uint16_t kHttpPort = 47002;
inline constexpr std::uint32_t kDiscoveryGroup = 0xEF4D4D4D;  // 239.77.77.77

struct PmdConfig {
    NodeId node;  // --node-id, 1 to 65534; without it, the ID saved in the state directory
    std::uint16_t control_port = kControlPort;      // --control-port; 0 lets the system choose
    std::uint16_t discovery_port = kDiscoveryPort;  // --discovery-port; the same on every node
    std::uint16_t http_port = kHttpPort;            // --http-port; 0 lets the system choose
    bool multicast = true;                          // --no-multicast: seeds only
    std::string iface;               // --iface: the one interface for multicast; else every one
    std::string state_dir;           // --state-dir; pmd.sock and node_id are kept in it
    std::uint16_t cap_cores = 0;     // --cap-cores: the thread quota of every job
    std::uint64_t cap_ram = 0;       // --cap-ram, bytes: the page data each job may hold here
    std::uint64_t spill_budget = 0;  // spill.budget, bytes: what a job may spill here
    // --peers address[:control_port[:discovery_port]],...: the seed list. A daemon sends its
    // HELLO to each seed too, and asks them for workers (M3-5).
    std::vector<Endpoint> peers;
    std::vector<std::uint16_t> peer_discovery_ports;  // one for each of peers
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
    [[nodiscard]] std::uint16_t discovery_port() const noexcept { return config_.discovery_port; }
    [[nodiscard]] std::uint16_t http_port() const noexcept { return config_.http_port; }
    [[nodiscard]] NodeId node() const noexcept { return config_.node; }
    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

    // Serves until stop(). stop() may be called from any thread and from a signal handler.
    void run();
    void stop() const noexcept;
    // A graceful leave, as `pm leave` asks (docs/PROTOCOL.md, 8.2): every job this node serves
    // is told, and run() returns once its workers have gone. Any thread or a signal handler.
    void leave() const noexcept;

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
        std::uint16_t want = 0;  // the job's threads_per_node; 0 for no bound
    };

    // A worker that has registered, until its local connection closes.
    struct Worker {
        JobId job;
        std::uint64_t local = 0;               // its connection on the local socket
        std::uint64_t control = 0;             // the control connection its SPAWN_REQ came on
        NodeId launcher;                       // the job's launcher node
        std::vector<LSegmentsEntry> segments;  // the newest L_SEGMENTS, since `since`
        Nanos since{};
        std::uint16_t want = 0;    // threads_per_node; 0 for no bound
        std::uint16_t slots = 0;   // its share of this node's worker slots, as last told
        std::uint32_t waited = 0;  // share-outs in a row it had none at
    };

    // A job launched on this node, from L_RUN_REQ until its launcher has gone.
    struct Run {
        JobId job;
        LRunReqPayload request;
        std::uint64_t pmrun = 0;  // connection IDs; 0 when there is none
        std::uint64_t launcher = 0;
        LAdmitReqPayload wants;             // from L_ADMIT_REQ
        std::vector<SegMapMember> members;  // the launcher first
        std::vector<Endpoint> candidates;   // the peers to ask, in order
        std::size_t next_peer = 0;          // index in candidates of the next peer to ask
        std::uint64_t committed = 0;        // RAM and spill the members commit, this node's too
        bool unlimited = false;             // a member has no RAM cap
        std::size_t asked = 0;              // SPAWN_REQs not yet answered
        bool admitting = false;
        std::vector<LSegmentsEntry> segments;  // the newest L_SEGMENTS, since `since`
        Nanos since{};
        std::uint16_t slots = 0;   // its share of this node's worker slots, as last told
        std::uint32_t waited = 0;  // share-outs in a row it had none at
        bool answered = false;     // L_ADMIT_OK is sent: quotas may be pushed to it
    };
    // One SPAWN_REQ this daemon sent and has no answer to.
    struct Ask {
        JobId job;
        std::uint64_t connection = 0;
        Endpoint peer;
        bool mid_run = false;  // a refill of a running job (M4-7)
    };

    void on_run_req(Connection& connection, const FrameHeader& header,
                    std::span<const std::byte> payload);
    void on_admit_req(Connection& connection, const FrameHeader& header,
                      std::span<const std::byte> payload);
    void on_spawn_answer(const FrameHeader& header, std::span<const std::byte> payload);
    void on_closed(const Connection& connection);
    void on_leave_req(Connection& connection, std::span<const std::byte> payload);
    void on_leave_intent(const FrameHeader& header, std::span<const std::byte> payload);
    void begin_leave(std::uint8_t reason);
    void on_chunk(const Connection& connection, const FrameHeader& header,
                  std::span<const std::byte> payload);
    void on_segments(const Connection& connection, const FrameHeader& header,
                     std::span<const std::byte> payload);
    void accrue(JobId job, NodeId launcher, std::vector<LSegmentsEntry>& segments, Nanos& since);
    void save_ledger();
    void share_slots(std::uint64_t registering = 0);
    void ask_peers(Run& run);
    void ask_next_peer(Run& run);
    void ask_peer(Run& run, Endpoint peer, std::uint16_t flags);
    void refill(NodeId node, Endpoint peer);
    void finish_admission(Run& run);
    void refuse(Run& run, std::uint64_t available, std::size_t nodes);
    std::uint32_t next_job_number();
    Run* run_of(JobId job);

    Result<void> open_discovery();
    void beacon(Nanos now);
    void hear();
    void say_bye();
    void announce(Opcode opcode, std::span<const std::byte> payload);
    void send_datagram(Opcode opcode, std::span<const std::byte> payload, std::uint32_t ipv4,
                       std::uint16_t port);
    std::string peers_json();

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
    std::vector<Worker> workers_;
    bool leaving_ = false;
    mutable std::atomic<bool> stop_asked_{false};
    mutable std::atomic<bool> leave_asked_{false};
    std::vector<pid_t> children_;  // every worker started and not yet collected
    std::vector<Run> runs_;
    std::unordered_map<std::uint64_t, Ask> asks_;  // by the SPAWN_REQ's request number
    std::uint64_t next_req_ = 1;
    // Discovery.
    int discovery_ = -1;                     // the UDP socket
    std::vector<std::uint32_t> interfaces_;  // IPv4 of each interface multicast goes out on
    std::uint32_t instance_ = 0;
    Nanos last_beacon_{};
    std::mutex peers_mu_;               // the table is read by the HTTP thread too
    std::vector<Peer> clashes_;         // beacons that claimed a node ID another daemon has
    std::unique_ptr<PeerTable> table_;  // made by open(), once the node ID is known
    std::optional<Ledger> ledger_;      // read by open() from the state directory
    CreditRates rates_;                 // ledger.mem_rate
    double ledger_scale_ = 3600;        // ledger.scale, core-seconds
    std::uint64_t cache_share_ = 25;    // mem.cache_share, percent, as the job processes take it
    Nanos last_save_{};
};

}  // namespace paramesh

#endif  // PARAMESH_PMD_PMD_H
