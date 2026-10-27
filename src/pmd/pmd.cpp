#include "pmd/pmd.h"

#include "net/frame_io.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <thread>

// POSIX leaves the declaration to the program.
// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;

namespace paramesh {

namespace {

constexpr int kReapEveryMs = 200;  // how often run() looks for workers that have exited
constexpr Nanos kLedgerSaveEvery = std::chrono::seconds{1};  // at most; and when pmd stops
constexpr std::size_t kReadAtOnce = 65536;

Error bad_flag(const char* what) {
    return Error{Errc::kInvalidArgument, 0, what};
}

bool to_u16(std::string_view text, std::uint16_t lowest, std::uint16_t& out) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < lowest ||
        value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

// "4096", "512M", "2G": bytes, with an optional K, M or G (powers of 1024). At least a page.
bool to_bytes(std::string_view text, std::uint64_t& out) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    const std::string_view unit{end, static_cast<std::size_t>(text.data() + text.size() - end)};
    unsigned shift = 0;
    if (unit == "K") {
        shift = 10;
    } else if (unit == "M") {
        shift = 20;
    } else if (unit == "G") {
        shift = 30;
    } else if (!unit.empty()) {
        return false;
    }
    if (error != std::errc{} || value > (~std::uint64_t{0} >> shift) ||
        (value << shift) < kPageSize) {
        return false;
    }
    out = value << shift;
    return true;
}

// "10.0.0.2", "10.0.0.2:47005" or "10.0.0.2:47005:47004": the address, its control port and
// its discovery port.
bool to_peer(std::string_view text, Endpoint& out, std::uint16_t& discovery) {
    const auto colon = text.find(':');
    const std::string address{text.substr(0, colon)};
    in_addr parsed{};
    out.port = kControlPort;
    discovery = kDiscoveryPort;
    if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        return false;
    }
    out.ipv4 = ntohl(parsed.s_addr);
    if (colon == std::string_view::npos) {
        return true;
    }
    const std::string_view ports = text.substr(colon + 1);
    const auto second = ports.find(':');
    return to_u16(ports.substr(0, second), 1, out.port) &&
           (second == std::string_view::npos || to_u16(ports.substr(second + 1), 1, discovery));
}

// This machine's RAM, in bytes.
std::uint64_t machine_ram() noexcept {
    return static_cast<std::uint64_t>(::sysconf(_SC_PHYS_PAGES)) *
           static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
}

std::string mb(std::uint64_t bytes) {
    return std::to_string(bytes >> 20U) + " MB";
}

// L_QUOTA: a share of the slots and what this node committed to the job.
LQuotaPayload quota_of(std::uint16_t slots, const JobCommit& commit) {
    return {slots, commit.ram, commit.spill, commit.no_homes ? kQuotaNoHomes : std::uint16_t{0}};
}

std::string dotted(std::uint32_t ipv4) {
    in_addr address{};
    address.s_addr = htonl(ipv4);
    std::array<char, INET_ADDRSTRLEN> text{};
    ::inet_ntop(AF_INET, &address, text.data(), text.size());
    return text.data();
}

}  // namespace

std::string_view pmd_usage() noexcept {
    return "usage: pmd [--node-id N] [--cap-cores N] [--cap-ram BYTES]\n"
           "           [--peers address[:port[:port]],...]\n"
           "           [--state-dir DIRECTORY] [--iface NAME] [--no-multicast]\n"
           "           [--control-port N] [--discovery-port N] [--http-port N]\n"
           "           [--dashboard FILE] [--standby]\n"
           "  --node-id       this node's ID, 1 to 65534 (default: a random ID, made on the first\n"
           "                  run and kept in the state directory)\n"
           "  --cap-cores     the most worker threads a job may run here (default: all "
           "processors)\n"
           "  --cap-ram       the most RAM the jobs here hold shared pages in, together; bytes or\n"
           "                  K, M or G (default: all of this machine's RAM)\n"
           "  --peers         seeds: daemons to send beacons to and ask for workers; the ports\n"
           "                  default to 47001 (control) and 47000 (discovery)\n"
           "  --state-dir     where pmd.sock and node_id are kept (default "
           "~/.local/state/paramesh)\n"
           "  --iface         the one interface for multicast (default: every one that can)\n"
           "  --no-multicast  send beacons to the seeds only\n"
           "  --control-port  the TCP port for other daemons (default 47001)\n"
           "  --discovery-port the UDP port for beacons, the same on every node (default 47000)\n"
           "  --http-port     the status API on 127.0.0.1 (default 47002); the dashboard is at /\n"
           "  --dashboard     the dashboard page (default: tools/dashboard/index.html of the\n"
           "                  source tree pmd was built in)\n"
           "  --standby       start out of the pool: hear it, take no work, and join from the\n"
           "                  dashboard with the cores and RAM chosen there\n";
}

namespace {

// --peers: entries separated by commas.
bool to_peers(std::string_view value, PmdConfig& config) {
    for (std::string_view rest = value; !rest.empty();) {
        const auto comma = rest.find(',');
        Endpoint peer;
        std::uint16_t discovery = 0;
        if (!to_peer(rest.substr(0, comma), peer, discovery)) {
            return false;
        }
        config.peers.push_back(peer);
        config.peer_discovery_ports.push_back(discovery);
        rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
    }
    return true;
}

// The flags that take a 16-bit number: where it goes, its least value, and what to say.
struct NumberFlag {
    std::string_view name;
    std::uint16_t PmdConfig::*field;
    std::uint16_t lowest;
    const char* wrong;
};

}  // namespace

Result<PmdConfig> pmd_parse_args(std::span<const std::string_view> args) {
    static constexpr std::array kNumbers{
        NumberFlag{"--cap-cores", &PmdConfig::cap_cores, 1, "--cap-cores must be 1 to 65535"},
        NumberFlag{"--control-port", &PmdConfig::control_port, 0,
                   "--control-port must be 0 to 65535"},
        NumberFlag{"--discovery-port", &PmdConfig::discovery_port, 0,
                   "--discovery-port must be 0 to 65535"},
        NumberFlag{"--http-port", &PmdConfig::http_port, 0, "--http-port must be 0 to 65535"},
    };
    PmdConfig config;
    for (std::size_t i = 0; i < args.size(); i += 2) {
        const std::string_view flag = args[i];
        if (flag == "--no-multicast" || flag == "--standby") {  // the flags with no value
            (flag == "--standby" ? config.standby : config.multicast) = flag == "--standby";
            i--;
            continue;
        }
        if (i + 1 == args.size()) {
            return bad_flag("the last flag has no value");
        }
        const std::string_view value = args[i + 1];
        const auto* number = std::find_if(kNumbers.begin(), kNumbers.end(),
                                          [flag](const NumberFlag& n) { return n.name == flag; });
        if (number != kNumbers.end()) {
            if (!to_u16(value, number->lowest, config.*(number->field))) {
                return bad_flag(number->wrong);
            }
        } else if (flag == "--node-id") {
            if (!to_u16(value, 1, config.node.value) || config.node == kAllNodes) {
                return bad_flag("--node-id must be 1 to 65534");
            }
        } else if (flag == "--cap-ram") {
            if (!to_bytes(value, config.cap_ram)) {
                return bad_flag(
                    "--cap-ram must be a number of bytes, at least 4096, with an "
                    "optional K, M or G");
            }
        } else if (flag == "--iface") {
            config.iface = value;
        } else if (flag == "--state-dir") {
            config.state_dir = value;
        } else if (flag == "--dashboard") {
            config.dashboard = value;
        } else if (flag == "--peers") {
            if (!to_peers(value, config)) {
                return bad_flag(
                    "--peers must be address[:control port[:discovery port]], separated by "
                    "commas");
            }
        } else {
            return bad_flag("a flag pmd does not know");
        }
    }
    if (config.state_dir.empty()) {
        const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): at start-up
        config.state_dir = std::string{home != nullptr ? home : "."} + "/.local/state/paramesh";
    }
    if (config.cap_cores == 0) {
        config.cap_cores = static_cast<std::uint16_t>(
            std::clamp<unsigned>(std::thread::hardware_concurrency(), 1, 65535));
    }
    if (config.cap_ram == 0) {  // as with the cores: all of it
        config.cap_ram = machine_ram();
    }
    return config;
}

Pmd::Pmd(PmdConfig config, const Platform& platform)
    : config_(std::move(config)), platform_(platform) {}

Pmd::~Pmd() {
    for (const Connection& connection : connections_) {
        ::close(connection.fd);
    }
    {
        const std::scoped_lock hold{status_mu_};
        streams_end_ = true;  // event streams return, so the server can stop
    }
    events_changed_.notify_all();
    platform_.http->stop();
    for (const int fd : {control_listener_, local_listener_, stop_fd_, discovery_}) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    if (local_listener_ >= 0) {
        ::unlink(socket_path_.c_str());
    }
}

Result<void> Pmd::open() {
    // spill.budget: the RAM cap unless set, as in the job processes started here.
    if (config_.spill_budget == 0) {
        const char* budget =
            std::getenv("PARAMESH_CFG_SPILL_BUDGET");  // NOLINT(concurrency-mt-unsafe)
        config_.spill_budget =
            budget != nullptr ? std::strtoull(budget, nullptr, 10) : config_.cap_ram;
    }
    if (config_.dashboard.empty()) {
        // The source tree's copy: the nearest tools/dashboard/index.html above the executable
        // (build/<preset>/src/pmd/pmd, build/src/pmd/pmd, ...).
        std::error_code ignored;
        std::filesystem::path dir = std::filesystem::read_symlink("/proc/self/exe", ignored);
        for (int up = 0; up < 6 && !dir.empty() && config_.dashboard.empty(); up++) {
            dir = dir.parent_path();
            if (std::filesystem::exists(dir / "tools/dashboard/index.html", ignored)) {
                config_.dashboard = (dir / "tools/dashboard/index.html").string();
            }
        }
    }
    std::error_code failed;
    std::filesystem::create_directories(config_.state_dir, failed);
    // The node ID: the flag's, or the one made on the first run and kept here.
    std::random_device random;
    if (!failed && config_.node == kNoNode) {
        const std::string saved = config_.state_dir + "/node_id";
        unsigned id = 0;
        std::ifstream{saved} >> id;
        if (id == 0 || id >= kAllNodes.value) {
            id = std::uniform_int_distribution<unsigned>{1, kAllNodes.value - 1U}(random);
            std::ofstream{saved, std::ios::trunc} << id << '\n';
        }
        config_.node = NodeId{static_cast<std::uint16_t>(id)};
    }
    if (!failed) {
        Result<Ledger> ledger = Ledger::open(config_.state_dir + "/ledger.json", *platform_.json);
        if (!ledger.ok()) {
            return ledger.error();
        }
        ledger_ = std::move(ledger).value();
        refresh_ledger_status();
    }
    // ledger.scale: the balance, in core-seconds, that doubles a peer's weight; and
    // mem.cache_share, to know the home store's part of --cap-ram.
    if (const char* scale =
            std::getenv("PARAMESH_CFG_LEDGER_SCALE")) {  // NOLINT(concurrency-mt-unsafe)
        ledger_scale_ = std::strtod(scale, nullptr);
    }
    if (const char* share =
            std::getenv("PARAMESH_CFG_MEM_CACHE_SHARE")) {  // NOLINT(concurrency-mt-unsafe)
        cache_share_ = std::min<std::uint64_t>(std::strtoul(share, nullptr, 10), 100);
    }
    // ledger.mem_rate: core-seconds of credit for one GB-hour of segments hosted.
    if (const char* rate =
            std::getenv("PARAMESH_CFG_LEDGER_MEM_RATE")) {  // NOLINT(concurrency-mt-unsafe)
        rates_.core_seconds_per_gb_hour = std::strtod(rate, nullptr);
    }
    instance_ = static_cast<std::uint32_t>(random());
    table_ = std::make_unique<PeerTable>(config_.node, instance_);
    socket_path_ = config_.state_dir + "/pmd.sock";
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    if (failed) {
        return Error{Errc::kIo, failed.value(), "make the state directory"};
    }
    if (socket_path_.size() >= sizeof local.sun_path) {
        return Error{Errc::kInvalidArgument, 0,
                     "use that state directory: its path is too long for a socket"};
    }
    std::copy(socket_path_.begin(), socket_path_.end(), std::begin(local.sun_path));
    ::unlink(socket_path_.c_str());  // left by a daemon that did not end in order
    local_listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (local_listener_ < 0 ||
        ::bind(local_listener_, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0 ||
        ::chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) != 0 ||
        ::listen(local_listener_, SOMAXCONN) != 0) {
        return Error{Errc::kIo, errno, "open the local socket"};
    }

    sockaddr_in control{};
    control.sin_family = AF_INET;
    control.sin_addr.s_addr = htonl(INADDR_ANY);
    control.sin_port = htons(config_.control_port);
    socklen_t length = sizeof control;
    const int on = 1;
    control_listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (control_listener_ < 0 ||
        ::setsockopt(control_listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
        ::bind(control_listener_, reinterpret_cast<const sockaddr*>(&control), sizeof control) !=
            0 ||
        ::listen(control_listener_, SOMAXCONN) != 0 ||
        ::getsockname(control_listener_, reinterpret_cast<sockaddr*>(&control), &length) != 0) {
        return Error{Errc::kIo, errno, "listen on the daemon control port"};
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    config_.control_port = ntohs(control.sin_port);

    stop_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_fd_ < 0) {
        return Error{Errc::kIo, errno, "create the stop eventfd"};
    }
    if (const Result<void> discovery = open_discovery(); !discovery.ok()) {
        return discovery;
    }
    // The dashboard (M6-3), read at each request: one static page that reads this API.
    platform_.http->get("/", [this](std::string_view /*path*/) {
        std::ifstream file{config_.dashboard, std::ios::binary};
        if (!file) {
            return HttpResponse{
                404, "text/plain",
                "no dashboard at " + config_.dashboard + "; pmd --dashboard FILE\n"};
        }
        return HttpResponse{200, "text/html; charset=utf-8",
                            std::string{std::istreambuf_iterator<char>{file}, {}}};
    });
    platform_.http->get("/v1/jobs", [this](std::string_view /*path*/) {
        const std::scoped_lock hold{status_mu_};
        return HttpResponse{200, "application/json", jobs_json_};
    });
    platform_.http->get(R"(/v1/jobs/(\d+)/pages)", [this](std::string_view path) {
        const std::uint32_t job = static_cast<std::uint32_t>(
            std::strtoul(std::string{path.substr(9)}.c_str(), nullptr, 10));  // "/v1/jobs/"
        const std::scoped_lock hold{status_mu_};
        const auto found = pages_json_.find(job);
        return found == pages_json_.end()
                   ? HttpResponse{404, "application/json", R"({"error":"no such job here"})"}
                   : HttpResponse{200, "application/json", found->second};
    });
    platform_.http->get_stream(
        "/v1/events", "text/event-stream",
        [this](std::string_view /*path*/, const HttpWrite& write) { stream_events(write); });
    platform_.http->get("/v1/ledger", [this](std::string_view /*path*/) {
        const std::scoped_lock hold{status_mu_};
        return HttpResponse{200, "application/json", ledger_json_};
    });
    platform_.http->get("/v1/peers", [this](std::string_view /*path*/) {
        return HttpResponse{200, "application/json", peers_json()};
    });
    platform_.http->post("/v1/join", [this](std::string_view /*path*/, std::string_view body) {
        return ask_join(body);
    });
    platform_.http->post("/v1/leave", [this](std::string_view /*path*/, std::string_view /*body*/) {
        return ask_leave();
    });
    joined_ = !config_.standby;
    member_ = joined_ ? "joined" : "standby";
    const Result<std::uint16_t> http = platform_.http->start("127.0.0.1", config_.http_port);
    if (!http.ok()) {
        return Error{Errc::kIo, http.error().os_error, "serve the status API on 127.0.0.1"};
    }
    config_.http_port = http.value();
    platform_.logger->log(
        LogLevel::kInfo, "pmd_start",
        JsonObject{{"control_port", JsonValue{std::int64_t{config_.control_port}}},
                   {"socket", JsonValue{socket_path_}},
                   {"node", JsonValue{std::int64_t{config_.node.value}}},
                   {"discovery_port", JsonValue{std::int64_t{config_.discovery_port}}},
                   {"http_port", JsonValue{std::int64_t{config_.http_port}}},
                   {"cap_cores", JsonValue{std::int64_t{config_.cap_cores}}}});
    return {};
}

void Pmd::leave() const noexcept {
    leave_asked_ = true;
    const std::uint64_t one = 1;
    const ssize_t written = ::write(stop_fd_, &one, sizeof one);
    static_cast<void>(written);
}

void Pmd::stop() const noexcept {
    stop_asked_ = true;
    const std::uint64_t one = 1;
    const ssize_t written = ::write(stop_fd_, &one, sizeof one);
    static_cast<void>(written);
}

void Pmd::run() {
    for (;;) {
        std::vector<pollfd> fds{{stop_fd_, POLLIN, 0},
                                {control_listener_, POLLIN, 0},
                                {local_listener_, POLLIN, 0},
                                {discovery_, POLLIN, 0}};
        for (const Connection& connection : connections_) {
            fds.push_back({connection.fd, POLLIN, 0});
        }
        if (::poll(fds.data(), fds.size(), kReapEveryMs) < 0 && errno != EINTR) {
            return;
        }
        if (fds[0].revents != 0) {
            std::uint64_t count = 0;
            [[maybe_unused]] const ssize_t got = ::read(stop_fd_, &count, sizeof count);
            if (stop_asked_) {
                say_bye();
                return;
            }
            if (leave_asked_.exchange(false) && !leaving_) {
                begin_leave(kLeaveInterrupt);
            }
            act_on_asks();
        }
        if ((fds[3].revents & POLLIN) != 0) {
            hear();
        }
        if (const Nanos now = platform_.clock->now(); now - last_beacon_ >= kBeaconEvery) {
            beacon(now);
        }
        // The connections polled are the first ones of connections_; accepting only adds more.
        for (std::size_t i = 4; i < fds.size(); i++) {
            Connection& connection = connections_[i - 4];
            if (fds[i].revents != 0 && !read_from(connection)) {
                ::close(connection.fd);
                connection.fd = -1;
                on_closed(connection);
            }
        }
        std::erase_if(connections_, [](const Connection& c) { return c.fd < 0; });
        // A leaving node goes once every worker of it has left its job, and no job it launched
        // is still running (pm leave --force ended them).
        if (leaving_ && workers_.empty() &&
            std::none_of(runs_.begin(), runs_.end(),
                         [](const Run& r) { return r.launcher != 0; })) {
            say_bye();
            if (!std::exchange(back_to_standby_, false)) {
                return;
            }
            leaving_ = false;  // left from the dashboard: out of the pool, still serving it
            joined_ = false;
            set_member("standby", "left the pool");
        }
        if ((fds[1].revents & POLLIN) != 0) {
            accept_on(control_listener_, false);
        }
        if ((fds[2].revents & POLLIN) != 0) {
            accept_on(local_listener_, true);
        }
        reap();
        if (platform_.clock->now() - last_save_ >= kLedgerSaveEvery) {
            save_ledger();
            tick_status();
        }
    }
}

void Pmd::accept_on(int listener, bool local) {
    const int fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd >= 0) {
        connections_.push_back(Connection{next_connection_++, fd, local, {}});
    }
}

// Takes what has arrived and handles every whole frame. False if the connection is to close.
bool Pmd::read_from(Connection& connection) {
    std::array<std::byte, kReadAtOnce> chunk{};
    const ssize_t got = ::recv(connection.fd, chunk.data(), chunk.size(), MSG_DONTWAIT);
    if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
        return false;
    }
    if (got > 0) {
        connection.in.insert(connection.in.end(), chunk.begin(), chunk.begin() + got);
    }
    while (connection.in.size() >= kFrameHeaderSize) {
        const Result<FrameHeader> header =
            wire_decode_header(std::span{connection.in}.first<kFrameHeaderSize>());
        if (!header.ok()) {
            return false;
        }
        const std::size_t whole = kFrameHeaderSize + header.value().payload_len;
        if (connection.in.size() < whole) {
            break;
        }
        const std::vector<std::byte> payload(
            connection.in.begin() + kFrameHeaderSize,
            connection.in.begin() + static_cast<std::ptrdiff_t>(whole));
        connection.in.erase(connection.in.begin(),
                            connection.in.begin() + static_cast<std::ptrdiff_t>(whole));
        if (!wire_check_payload(*platform_.checksum, header.value(), payload).ok()) {
            return false;
        }
        handle(connection, header.value(), payload);
    }
    return true;
}

void Pmd::handle(Connection& connection, const FrameHeader& header,
                 std::span<const std::byte> payload) {
    if (!connection.local && header.opcode == Opcode::kSpawnReq) {
        on_spawn_req(connection, header, payload);
    } else if (!connection.local &&
               (header.opcode == Opcode::kSpawnOk || header.opcode == Opcode::kSpawnDecline)) {
        on_spawn_answer(header, payload);
    } else if (connection.local && header.opcode == Opcode::kLRegister) {
        on_register(connection, header, payload);
    } else if (connection.local && header.opcode == Opcode::kLRunReq) {
        on_run_req(connection, header, payload);
    } else if (connection.local && header.opcode == Opcode::kLAdmitReq) {
        on_admit_req(connection, header, payload);
    } else if (connection.local && header.opcode == Opcode::kLLeaveReq) {
        on_leave_req(connection, payload);
    } else if (connection.local && header.opcode == Opcode::kLChunk) {
        on_chunk(connection, header, payload);
    } else if (connection.local && header.opcode == Opcode::kLSegments) {
        on_segments(connection, header, payload);
    } else if (connection.local && header.opcode == Opcode::kLStats) {
        on_stats(connection, header, payload);
    } else if (!connection.local && header.opcode == Opcode::kLedgerSync) {
        on_ledger_sync(header, payload);
    } else if (!connection.local && header.opcode == Opcode::kLeaveIntent) {
        on_leave_intent(header, payload);
    } else {
        platform_.logger->log(
            LogLevel::kWarn, "frame_ignored",
            JsonObject{
                {"opcode", JsonValue{std::int64_t{static_cast<std::uint8_t>(header.opcode)}}},
                {"local", JsonValue{connection.local}}});
    }
}

// SPAWN_REQ: check the executable against the launcher's hash, then start one worker.
void Pmd::on_spawn_req(Connection& connection, const FrameHeader& header,
                       std::span<const std::byte> payload) {
    Spawn spawn{header.job, 0, connection.id, header.src, header.req, {}, 0, {}};
    const Result<SpawnReqPayload> decoded = wire_decode_spawn_req(payload);
    if (!decoded.ok()) {
        decline(spawn, Status::kProtocol, "a malformed SPAWN_REQ");
        return;
    }
    const SpawnReqPayload& request = decoded.value();
    spawn.hash = request.binary_hash;
    spawn.want = request.threads_per_node;
    const std::string here = " on node " + std::to_string(config_.node.value);
    if (!joined_) {
        decline(spawn, Status::kDeclined,
                "node " + std::to_string(config_.node.value) + " has not joined the pool");
        return;
    }
    if (leaving_) {
        decline(spawn, Status::kLeaving,
                "node " + std::to_string(config_.node.value) + " is leaving");
        return;
    }
    if (config_.cap_ram != 0) {  // a node with no RAM cap commits nothing and takes any job
        const JobCommit free = free_now();
        const bool joining = (request.flags & kSpawnMidRun) != 0;  // home for nothing: a cache
        const std::optional<JobCommit> commit =
            joining ? joiner_commit(request.region_bytes, request.nodes, free.ram, cache_share_)
                    : job_commit(request.region_bytes, request.nodes, free.ram, free.spill,
                                 cache_share_);
        if (!commit) {
            decline(spawn, Status::kNoCapacity,
                    "node " + std::to_string(config_.node.value) + " cannot commit " +
                        mb(job_need(request.region_bytes, request.nodes)) + " to the job: " +
                        mb(free.ram) + " of RAM and " + mb(free.spill) + " of spill free");
            return;
        }
        spawn.commit = *commit;
    }

    std::ifstream file{request.path, std::ios::binary};
    const std::vector<char> image{std::istreambuf_iterator<char>{file},
                                  std::istreambuf_iterator<char>{}};
    if (!file.is_open() || image.empty()) {
        decline(spawn, Status::kNotFound, "no executable at " + request.path + here);
        return;
    }
    if (platform_.hasher->sha256(std::as_bytes(std::span{image})) != request.binary_hash) {
        decline(spawn, Status::kBinaryMismatch,
                "binary mismatch: " + request.path + here + " is not the launcher's executable");
        return;
    }

    // Everything the child needs is made before fork(): after it, only system calls.
    std::vector<std::string> arguments{request.path};
    arguments.insert(arguments.end(), request.argv.begin(), request.argv.end());
    // The environment of docs/PROTOCOL.md, section 10. This daemon's own PARAMESH_ variables
    // are not passed on, except the tunables.
    std::vector<std::string> environment;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; entry++) {
        const std::string_view text{*entry};
        if (!text.starts_with("PARAMESH_") || text.starts_with("PARAMESH_CFG_")) {
            environment.emplace_back(text);
        }
    }
    environment.emplace_back("PARAMESH_ROLE=worker");
    environment.push_back("PARAMESH_JOB_ID=" + std::to_string(header.job.value));
    environment.push_back("PARAMESH_NODE_ID=" + std::to_string(config_.node.value));
    environment.push_back("PARAMESH_PMD_SOCKET=" + socket_path_);
    environment.push_back("PARAMESH_LAUNCHER=" + dotted(request.launcher_addr) + ":" +
                          std::to_string(request.launcher_port));
    std::vector<char*> argv;
    std::vector<char*> envp;
    argv.reserve(arguments.size() + 1);
    envp.reserve(environment.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    for (std::string& variable : environment) {
        envp.push_back(variable.data());
    }
    argv.push_back(nullptr);
    envp.push_back(nullptr);

    spawn.pid = ::fork();
    if (spawn.pid == 0) {
        if (request.cwd.empty() || ::chdir(request.cwd.c_str()) == 0) {
            ::execve(request.path.c_str(), argv.data(), envp.data());
        }
        ::_exit(127);
    }
    if (spawn.pid < 0) {
        decline(spawn, Status::kInternal, "could not start a process" + here);
        return;
    }
    spawns_.push_back(spawn);
    children_.push_back(spawn.pid);
    platform_.logger->log(LogLevel::kInfo, "spawn",
                          JsonObject{{"job", JsonValue{std::int64_t{header.job.value}}},
                                     {"pid", JsonValue{std::int64_t{spawn.pid}}},
                                     {"path", JsonValue{request.path}}});
}

// L_REGISTER: a job process is up. If this daemon started it, the peer that asked is told;
// either way the process is told its quota: its share of the slots, and what this node
// committed to its job. A launcher learns its commitment after admission.
void Pmd::on_register(Connection& connection, const FrameHeader& header,
                      std::span<const std::byte> payload) {
    const Result<LRegisterPayload> decoded = wire_decode_l_register(payload);
    if (!decoded.ok()) {
        return;
    }
    const LRegisterPayload& process = decoded.value();
    const auto spawn = std::find_if(spawns_.begin(), spawns_.end(), [&](const Spawn& s) {
        return s.job == header.job && static_cast<std::uint32_t>(s.pid) == process.pid;
    });
    if (spawn != spawns_.end()) {
        if (process.binary_hash != spawn->hash) {
            decline(*spawn, Status::kBinaryMismatch,
                    "binary mismatch: the worker's own hash is not the launcher's");
            ::kill(spawn->pid, SIGKILL);
        } else {
            reply(spawn->asked_on, Opcode::kSpawnOk, spawn->job, spawn->asker, spawn->req,
                  SpawnOkPayload{config_.node, process.data_port, config_.cap_cores,
                                 spawn->commit.ram, spawn->commit.spill});
            workers_.push_back(Worker{spawn->job,
                                      connection.id,
                                      spawn->asked_on,
                                      spawn->asker,
                                      {},
                                      platform_.clock->now(),
                                      spawn->want,
                                      0,
                                      0,
                                      {},
                                      spawn->commit});
        }
        spawns_.erase(spawn);
    }
    if (Run* run = run_of(header.job); run != nullptr && process.role == 1) {
        run->launcher = connection.id;  // the launcher pmrun started for a job of this node
        run->members.assign(
            1, SegMapMember{config_.node, Slot{0}, kMemberLauncher, 0, process.data_port, 0});
    }
    share_slots(connection.id);  // its share comes in this reply, the others' are pushed
    std::uint16_t slots = config_.cap_cores;  // a process this node has no job for: all
    JobCommit commit{config_.cap_ram, config_.spill_budget, 0, false};
    for (const Worker& w : workers_) {
        slots = w.local == connection.id ? w.slots : slots;
        commit = w.local == connection.id ? w.commit : commit;
    }
    if (const Run* run = run_of(header.job); run != nullptr && run->launcher == connection.id) {
        slots = run->slots;
    }
    reply(connection.id, Opcode::kLQuota, header.job, kNoNode, kNoReq, quota_of(slots, commit));
    platform_.logger->log(LogLevel::kInfo, "register",
                          JsonObject{{"job", JsonValue{std::int64_t{header.job.value}}},
                                     {"pid", JsonValue{std::int64_t{process.pid}}},
                                     {"role", JsonValue{std::int64_t{process.role}}}});
}

// The UDP socket for beacons, and the interfaces multicast goes out on: every one that is up
// and can, loopback aside, or only --iface (docs/PROTOCOL.md, [GATE P7]).
Result<void> Pmd::open_discovery() {
    discovery_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    at.sin_port = htons(config_.discovery_port);
    socklen_t length = sizeof at;
    const int on = 1;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (discovery_ < 0 || ::setsockopt(discovery_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
        ::bind(discovery_, reinterpret_cast<const sockaddr*>(&at), sizeof at) != 0 ||
        ::getsockname(discovery_, reinterpret_cast<sockaddr*>(&at), &length) != 0) {
        return Error{Errc::kIo, errno, "open the discovery port"};
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    config_.discovery_port = ntohs(at.sin_port);
    if (!config_.multicast) {
        return {};
    }
    ifaddrs* all = nullptr;
    if (::getifaddrs(&all) != 0) {
        return Error{Errc::kIo, errno, "list the network interfaces"};
    }
    for (const ifaddrs* each = all; each != nullptr; each = each->ifa_next) {
        const bool wanted = config_.iface.empty() ? (each->ifa_flags & IFF_MULTICAST) != 0 &&
                                                        (each->ifa_flags & IFF_LOOPBACK) == 0
                                                  : config_.iface == each->ifa_name;
        if (each->ifa_addr == nullptr || each->ifa_addr->sa_family != AF_INET ||
            (each->ifa_flags & IFF_UP) == 0 || !wanted) {
            continue;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
        const in_addr address = reinterpret_cast<const sockaddr_in*>(each->ifa_addr)->sin_addr;
        ip_mreq join{};
        join.imr_multiaddr.s_addr = htonl(kDiscoveryGroup);
        join.imr_interface = address;
        if (::setsockopt(discovery_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof join) == 0) {
            interfaces_.push_back(ntohl(address.s_addr));
        }
    }
    ::freeifaddrs(all);
    if (interfaces_.empty() && !config_.iface.empty()) {
        return Error{Errc::kInvalidArgument, 0, "use --iface: no such interface can multicast"};
    }
    const unsigned char ttl = 1;   // the local network only
    const unsigned char loop = 1;  // daemons on this machine hear each other
    ::setsockopt(discovery_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    ::setsockopt(discovery_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    return {};
}

void Pmd::send_datagram(Opcode opcode, std::span<const std::byte> payload, std::uint32_t ipv4,
                        std::uint16_t port) {
    FrameHeader header;
    header.opcode = opcode;
    header.src = config_.node;
    header.dst = kAllNodes;
    header.payload_len = static_cast<std::uint32_t>(payload.size());
    header.payload_crc = wire_payload_crc(*platform_.checksum, payload);
    std::vector<std::byte> datagram(kFrameHeaderSize + payload.size());
    wire_encode_header(header, std::span{datagram}.first<kFrameHeaderSize>());
    std::copy(payload.begin(), payload.end(), datagram.begin() + kFrameHeaderSize);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(ipv4);
    to.sin_port = htons(port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    static_cast<void>(::sendto(discovery_, datagram.data(), datagram.size(), 0,
                               reinterpret_cast<const sockaddr*>(&to), sizeof to));
}

// Sends one discovery frame on every multicast interface and to every seed.
void Pmd::announce(Opcode opcode, std::span<const std::byte> payload) {
    for (const std::uint32_t address : interfaces_) {
        in_addr out{};
        out.s_addr = htonl(address);
        ::setsockopt(discovery_, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof out);
        send_datagram(opcode, payload, kDiscoveryGroup, config_.discovery_port);
    }
    for (std::size_t i = 0; i < config_.peers.size(); i++) {
        send_datagram(opcode, payload, config_.peers[i].ipv4,
                      i < config_.peer_discovery_ports.size() ? config_.peer_discovery_ports[i]
                                                              : kDiscoveryPort);
    }
}

// Once a second: HELLO, and the peers silent too long leave the table.
void Pmd::beacon(Nanos now) {
    last_beacon_ = now;
    if (!joined_) {
        return;  // standby: heard from, but says nothing
    }
    HelloPayload hello;
    hello.instance = instance_;
    hello.control_port = config_.control_port;
    hello.cap_cores = config_.cap_cores;
    hello.cap_ram = config_.cap_ram;
    // ponytail: the slot allocation (M5-3) shares the cores among the jobs, so none is taken.
    const JobCommit free = free_now();
    hello.free_cores = config_.cap_cores;
    hello.free_ram = free.ram;
    hello.spill_budget = config_.spill_budget;
    hello.free_spill = free.spill;
    hello.jobs = static_cast<std::uint16_t>(std::min<std::size_t>(runs_.size(), 65535));
    hello.flags = leaving_ ? kHelloLeaving : std::uint16_t{0};
    if (ledger_) {  // this node's raw totals with up to 7 peers, for display and the alarm
        for (const auto& [peer, jobs] : ledger_->data()) {
            if (hello.totals.size() < 7) {
                hello.totals.push_back(ledger_totals(*ledger_, NodeId{peer}));
            }
        }
    }
    std::array<std::byte, 300> bytes{};
    if (const Result<std::size_t> size = wire_encode(hello, bytes); size.ok()) {
        announce(Opcode::kHello, std::span{bytes}.first(size.value()));
    }
    std::vector<Peer> gone;
    {
        const std::scoped_lock hold{peers_mu_};
        gone = table_->expire(now);
    }
    for (const Peer& peer : gone) {
        platform_.logger->log(LogLevel::kInfo, "peer_gone",
                              JsonObject{{"peer", JsonValue{std::int64_t{peer.node.value}}}});
    }
}

// BYE, when the daemon stops.
void Pmd::save_ledger() {
    last_save_ = platform_.clock->now();
    if (ledger_) {
        ledger_->apply_credits(rates_);
        refresh_ledger_status();
    }
    if (ledger_ && ledger_->dirty()) {
        if (const Result<void> saved = ledger_->save(); !saved.ok()) {
            platform_.logger->log(
                LogLevel::kWarn, "ledger_not_saved",
                JsonObject{{"why", JsonValue{std::string{saved.error().what}}},
                           {"errno", JsonValue{std::int64_t{saved.error().os_error}}}});
        }
    }
}

void Pmd::say_bye() {
    save_ledger();
    std::array<std::byte, 4> bytes{};
    static_cast<void>(wire_encode(ByePayload{instance_}, bytes));
    announce(Opcode::kBye, bytes);
}

// Every beacon that has arrived.
void Pmd::hear() {
    std::array<std::byte, 2048> datagram{};
    for (;;) {
        sockaddr_in from{};
        socklen_t length = sizeof from;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
        const ssize_t got = ::recvfrom(discovery_, datagram.data(), datagram.size(), 0,
                                       reinterpret_cast<sockaddr*>(&from), &length);
        if (got < 0) {
            return;  // nothing more for now
        }
        if (static_cast<std::size_t>(got) < kFrameHeaderSize) {
            continue;
        }
        const std::span<const std::byte> whole =
            std::span{datagram}.first(static_cast<std::size_t>(got));
        const Result<FrameHeader> header = wire_decode_header(whole.first<kFrameHeaderSize>());
        const std::span<const std::byte> payload = whole.subspan(kFrameHeaderSize);
        if (!header.ok() ||
            !wire_check_payload(*platform_.checksum, header.value(), payload).ok()) {
            continue;  // not a frame, or damaged: the next beacon comes in a second
        }
        const NodeId node = header.value().src;
        const std::uint32_t address = ntohl(from.sin_addr.s_addr);
        if (header.value().opcode == Opcode::kBye) {
            if (const Result<ByePayload> bye = wire_decode_bye(payload); bye.ok()) {
                const std::scoped_lock hold{peers_mu_};
                table_->bye(node, bye.value().instance);
            }
            continue;
        }
        const Result<HelloPayload> hello = wire_decode_hello(payload);
        if (header.value().opcode != Opcode::kHello || !hello.ok()) {
            continue;
        }
        PeerTable::Heard heard = PeerTable::Heard::kOwn;
        {
            const std::scoped_lock hold{peers_mu_};
            heard = table_->hello(node, address, hello.value(), platform_.clock->now());
            const bool reported = std::any_of(clashes_.begin(), clashes_.end(), [&](const Peer& p) {
                return p.node == node && p.hello.instance == hello.value().instance;
            });
            if (heard == PeerTable::Heard::kClash && reported) {
                heard = PeerTable::Heard::kKnown;  // reported once already
            } else if (heard == PeerTable::Heard::kClash) {
                clashes_.push_back(Peer{node, address, hello.value(), platform_.clock->now()});
            }
        }
        for (const LedgerTotal& total : hello.value().totals) {
            if (total.peer == config_.node) {  // what the peer says about this node, kept
                announced_.totals[node.value] = total;
            }
        }
        if (heard == PeerTable::Heard::kNew && (hello.value().flags & kHelloLeaving) == 0) {
            refill(node, Endpoint{address, hello.value().control_port});
        }
        if (heard == PeerTable::Heard::kNew || heard == PeerTable::Heard::kClash) {
            const bool fresh = heard == PeerTable::Heard::kNew;
            platform_.logger->log(fresh ? LogLevel::kInfo : LogLevel::kWarn,
                                  fresh ? "peer_new" : "node_id_clash",
                                  JsonObject{{"peer", JsonValue{std::int64_t{node.value}}},
                                             {"address", JsonValue{dotted(address)}}});
        }
    }
}

// GET /v1/peers: this node, the peers it hears, and any ID clash it has seen.
std::string Pmd::peers_json() {
    const Nanos now = platform_.clock->now();
    const auto entry = [now](const Peer& peer) {
        const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(now - peer.heard);
        return JsonValue{JsonObject{
            {"node", JsonValue{std::int64_t{peer.node.value}}},
            {"address", JsonValue{dotted(peer.address)}},
            {"control_port", JsonValue{std::int64_t{peer.hello.control_port}}},
            {"state", JsonValue{std::string{PeerTable::suspect(peer, now) ? "suspect" : "alive"}}},
            {"leaving", JsonValue{(peer.hello.flags & kHelloLeaving) != 0}},
            {"cap_cores", JsonValue{std::int64_t{peer.hello.cap_cores}}},
            {"free_cores", JsonValue{std::int64_t{peer.hello.free_cores}}},
            {"cap_ram", JsonValue{static_cast<std::int64_t>(peer.hello.cap_ram)}},
            {"free_ram", JsonValue{static_cast<std::int64_t>(peer.hello.free_ram)}},
            {"jobs", JsonValue{std::int64_t{peer.hello.jobs}}},
            {"silent_ms", JsonValue{static_cast<std::int64_t>(silent.count())}}}};
    };
    JsonArray peers;
    JsonArray clashes;
    const std::scoped_lock hold{peers_mu_};  // the table, and the caps a join changes
    for (const Peer& peer : table_->peers()) {
        peers.push_back(entry(peer));
    }
    for (const Peer& clash : clashes_) {
        clashes.push_back(entry(clash));
    }
    return platform_.json->encode(JsonValue{
        JsonObject{{"node", JsonValue{std::int64_t{config_.node.value}}},
                   {"cap_cores", JsonValue{std::int64_t{config_.cap_cores}}},
                   {"cap_ram", JsonValue{static_cast<std::int64_t>(config_.cap_ram)}},
                   {"member", JsonValue{member_}},
                   {"note", JsonValue{member_note_}},
                   {"machine_cores", JsonValue{std::int64_t{std::thread::hardware_concurrency()}}},
                   {"machine_ram", JsonValue{static_cast<std::int64_t>(machine_ram())}},
                   {"peers", JsonValue{std::move(peers)}},
                   {"clashes", JsonValue{std::move(clashes)}}}});
}

// ---- joining and leaving from the dashboard (X-dashboard-join) ----------------------------------

// POST /v1/join, on the HTTP thread: {"cap_cores": N, "cap_ram": bytes, "lead": "address"},
// the lead optional (a node of the pool, for a network without multicast). Checked here, done
// by the main thread.
HttpResponse Pmd::ask_join(std::string_view body) {
    const auto refuse = [this](int status, const std::string& why) {
        return HttpResponse{
            status, "application/json",
            platform_.json->encode(JsonValue{JsonObject{{"error", JsonValue{why}}}})};
    };
    const Result<JsonValue> parsed = platform_.json->decode(body);
    const auto* fields = parsed.ok() ? std::get_if<JsonObject>(&parsed.value().v) : nullptr;
    const auto number = [fields](std::string_view key) -> std::int64_t {
        const auto found = fields->find(key);
        const auto* n =
            found != fields->end() ? std::get_if<std::int64_t>(&found->second.v) : nullptr;
        return n != nullptr ? *n : -1;
    };
    if (fields == nullptr) {
        return refuse(400, "the body is not a JSON object");
    }
    JoinAsk ask;
    const std::int64_t cores = number("cap_cores");
    const std::int64_t ram = number("cap_ram");
    if (cores < 1 || cores > std::int64_t{std::thread::hardware_concurrency()}) {
        return refuse(400, "the cores to give must be 1 to " +
                               std::to_string(std::thread::hardware_concurrency()));
    }
    if (ram < std::int64_t{kPageSize} || static_cast<std::uint64_t>(ram) > machine_ram()) {
        return refuse(400, "the RAM to give must be at least 4 KB and at most this machine's RAM");
    }
    ask.cores = static_cast<std::uint16_t>(cores);
    ask.ram = static_cast<std::uint64_t>(ram);
    if (const auto lead = fields->find("lead"); lead != fields->end()) {
        const auto* text = std::get_if<std::string>(&lead->second.v);
        Endpoint at;
        if (text == nullptr || (!text->empty() && !to_peer(*text, at, ask.lead_discovery))) {
            return refuse(400,
                          "the lead node must be an IPv4 address, optionally with :control "
                          "port[:discovery port]");
        }
        if (!text->empty()) {
            ask.lead = at;
        }
    }
    {
        const std::scoped_lock hold{peers_mu_};
        if (member_ != "standby" || join_ask_) {
            return refuse(409, "this node is " + member_ + ", not standing by");
        }
        join_ask_ = ask;
    }
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t written = ::write(stop_fd_, &one, sizeof one);
    return HttpResponse{202, "application/json", R"({"member":"joining"})"};
}

// POST /v1/leave, on the HTTP thread: a graceful leave, back to standby when it is done.
HttpResponse Pmd::ask_leave() {
    {
        const std::scoped_lock hold{peers_mu_};
        if (member_ != "joined") {
            return HttpResponse{409, "application/json",
                                platform_.json->encode(JsonValue{
                                    JsonObject{{"error", JsonValue{"this node is " + member_}}}})};
        }
        leave_ask_ = true;
    }
    const std::uint64_t one = 1;
    [[maybe_unused]] const ssize_t written = ::write(stop_fd_, &one, sizeof one);
    return HttpResponse{202, "application/json", R"({"member":"leaving"})"};
}

// The main thread: carries out what the dashboard asked.
void Pmd::act_on_asks() {
    std::optional<JoinAsk> join;
    bool leave = false;
    {
        const std::scoped_lock hold{peers_mu_};
        join = std::exchange(join_ask_, std::nullopt);
        leave = std::exchange(leave_ask_, false);
        if (join && !joined_) {
            config_.cap_cores = join->cores;
            config_.cap_ram = join->ram;
            if (join->lead) {
                config_.peers.push_back(*join->lead);
                config_.peer_discovery_ports.push_back(join->lead_discovery);
            }
        }
    }
    if (join && !joined_) {
        const char* budget =
            std::getenv("PARAMESH_CFG_SPILL_BUDGET");  // NOLINT(concurrency-mt-unsafe)
        config_.spill_budget =
            budget != nullptr ? std::strtoull(budget, nullptr, 10) : config_.cap_ram;
        joined_ = true;
        set_member("joined", "joined with " + std::to_string(config_.cap_cores) + " cores and " +
                                 mb(config_.cap_ram) + " of RAM");
        beacon(platform_.clock->now());
    }
    if (leave && joined_ && !leaving_) {
        const auto launched =
            std::find_if(runs_.begin(), runs_.end(), [](const Run& r) { return r.launcher != 0; });
        if (launched != runs_.end()) {
            set_member("joined", "job " + std::to_string(launched->job.value) +
                                     ", launched here, is still running: leave when it ends");
            return;
        }
        back_to_standby_ = true;
        begin_leave(kLeaveAsked);
    }
}

void Pmd::set_member(std::string state, std::string note) {
    const std::scoped_lock hold{peers_mu_};
    member_ = std::move(state);
    member_note_ = std::move(note);
}

Pmd::Run* Pmd::run_of(JobId job) {
    const auto found =
        std::find_if(runs_.begin(), runs_.end(), [job](const Run& r) { return r.job == job; });
    return found != runs_.end() ? &*found : nullptr;
}

// The low half of the next job ID: a counter kept in the state directory, never 0 and never
// that of a job still running here (docs/PROTOCOL.md, L_RUN_OK).
std::uint32_t Pmd::next_job_number() {
    const std::string path = config_.state_dir + "/job_counter";
    std::uint32_t number = 0;
    std::ifstream{path} >> number;
    const auto in_use = [this](std::uint32_t low) {
        return run_of(JobId{(std::uint32_t{config_.node.value} << 16U) | low}) != nullptr;
    };
    number = number % 65535 + 1;
    while (in_use(number)) {
        number = number % 65535 + 1;
    }
    std::ofstream{path, std::ios::trunc} << number << '\n';
    return number;
}

// L_RUN_REQ from pmrun: name the job. Nothing starts until its launcher asks for admission.
void Pmd::on_run_req(Connection& connection, const FrameHeader& /*header*/,
                     std::span<const std::byte> payload) {
    Result<LRunReqPayload> request = wire_decode_l_run_req(payload);
    if (!request.ok()) {
        reply(connection.id, Opcode::kLRunRefused, JobId{0}, kNoNode, kNoReq,
              StatusTextPayload{Status::kProtocol, "a malformed L_RUN_REQ"});
        return;
    }
    if (leaving_) {
        reply(connection.id, Opcode::kLRunRefused, JobId{0}, kNoNode, kNoReq,
              StatusTextPayload{Status::kLeaving, "this node is leaving"});
        return;
    }
    if (!joined_) {
        reply(connection.id, Opcode::kLRunRefused, JobId{0}, kNoNode, kNoReq,
              StatusTextPayload{Status::kDeclined,
                                "this node has not joined the pool: join on the dashboard"});
        return;
    }
    Run run;
    run.job = JobId{(std::uint32_t{config_.node.value} << 16U) | next_job_number()};
    run.request = std::move(request).value();
    run.pmrun = connection.id;
    reply(connection.id, Opcode::kLRunOk, run.job, kNoNode, kNoReq,
          LRunOkPayload{run.job, config_.node});
    platform_.logger->log(LogLevel::kInfo, "run",
                          JsonObject{{"job", JsonValue{std::int64_t{run.job.value}}},
                                     {"nodes", JsonValue{std::int64_t{run.request.nodes}}},
                                     {"path", JsonValue{run.request.path}}});
    runs_.push_back(std::move(run));
}

namespace {

// docs/PLAN.md: admission headroom 90%.
std::uint64_t admissible(std::uint64_t capacity) {
    return capacity / 10 * 9;
}

}  // namespace

// RAM and spill not committed to a job: the caps less the commitments of the workers starting
// and running here and of the jobs launched here.
JobCommit Pmd::free_now() const {
    JobCommit taken;
    const auto add = [&taken](const JobCommit& c) {
        taken.ram += c.ram;
        taken.spill += c.spill;
    };
    std::for_each(spawns_.begin(), spawns_.end(), [&](const Spawn& s) { add(s.commit); });
    std::for_each(workers_.begin(), workers_.end(), [&](const Worker& w) { add(w.commit); });
    std::for_each(runs_.begin(), runs_.end(), [&](const Run& r) { add(r.commit); });
    return {config_.cap_ram - std::min(taken.ram, config_.cap_ram),
            config_.spill_budget - std::min(taken.spill, config_.spill_budget), 0, false};
}

// L_ADMIT_REFUSED: the region does not fit the nodes chosen.
void Pmd::refuse(Run& run, std::uint64_t available, std::size_t nodes) {
    refuse(run, available,
           "the region needs " + mb(run.wants.region_bytes) + " and the " + std::to_string(nodes) +
               (nodes == 1 ? " node chosen gives " : " nodes chosen give ") + mb(available) +
               " (90% of their RAM caps and spill budgets)");
}

void Pmd::refuse(Run& run, std::uint64_t available, const std::string& message) {
    const std::uint64_t needed = run.wants.region_bytes;
    run.admitting = false;
    reply(run.launcher, Opcode::kLAdmitRefused, run.job, kNoNode, kNoReq,
          LAdmitRefusedPayload{needed, available, Status::kNoCapacity, message});
    platform_.logger->log(
        LogLevel::kWarn, "refused",
        JsonObject{{"job", JsonValue{std::int64_t{run.job.value}}},
                   {"needed", JsonValue{static_cast<std::int64_t>(needed)}},
                   {"available", JsonValue{static_cast<std::int64_t>(available)}}});
}

// L_ADMIT_REQ from the launcher (docs/PROTOCOL.md, 8.1). The peers to ask are those heard
// through discovery, the one with most free capacity first, then the seeds of --peers not
// heard yet. If the peers that would be chosen are all known and the region does not fit
// 90% of their RAM and spill with this node's, the job is refused before any worker starts.
// A node with no RAM cap (--cap-ram 0, in tests) takes any region.
void Pmd::on_admit_req(Connection& connection, const FrameHeader& header,
                       std::span<const std::byte> payload) {
    Run* run = run_of(header.job);
    const Result<LAdmitReqPayload> wants = wire_decode_l_admit_req(payload);
    if (run == nullptr || run->launcher != connection.id || run->admitting || !wants.ok()) {
        return;
    }
    run->wants = wants.value();
    run->admitting = true;
    share_slots();  // its thread bound is known now; the share goes out in L_ADMIT_OK
    run->unlimited = config_.cap_ram == 0;
    const JobCommit free = free_now();

    std::vector<Peer> heard;
    {
        const std::scoped_lock hold{peers_mu_};
        heard = table_->peers();
    }
    std::erase_if(heard, [](const Peer& p) { return (p.hello.flags & kHelloLeaving) != 0; });
    const auto free_of = [](const Peer& p) { return p.hello.free_ram + p.hello.free_spill; };
    std::stable_sort(heard.begin(), heard.end(),
                     [&](const Peer& a, const Peer& b) { return free_of(a) > free_of(b); });
    const std::size_t others = run->request.nodes - 1U;
    std::uint64_t known = free.ram + free.spill;
    bool limited = !run->unlimited;
    for (std::size_t i = 0; i < heard.size(); i++) {
        run->candidates.push_back(Endpoint{heard[i].address, heard[i].hello.control_port});
        if (i < others) {
            known += free_of(heard[i]);
            limited = limited && heard[i].hello.cap_ram != 0;
        }
    }
    for (const Endpoint& seed : config_.peers) {
        if (std::none_of(run->candidates.begin(), run->candidates.end(), [&](const Endpoint& c) {
                return c.ipv4 == seed.ipv4 && c.port == seed.port;
            })) {
            run->candidates.push_back(seed);
        }
    }
    const bool all_known = heard.size() >= std::min(others, run->candidates.size());
    if (limited && all_known && run->wants.region_bytes > admissible(known)) {
        refuse(*run, admissible(known), 1 + std::min(others, heard.size()));
        return;
    }
    if (!run->unlimited) {  // this node's own share, as each peer commits its own
        const std::optional<JobCommit> commit = job_commit(
            run->wants.region_bytes, run->request.nodes, free.ram, free.spill, cache_share_);
        if (!commit) {
            refuse(*run, free.ram + free.spill,
                   "node " + std::to_string(config_.node.value) + " cannot commit its share, " +
                       mb(job_need(run->wants.region_bytes, run->request.nodes)) + ": " +
                       mb(free.ram) + " of RAM and " + mb(free.spill) + " of spill free");
            return;
        }
        run->commit = *commit;
    }
    run->committed = run->commit.ram + run->commit.spill;
    ask_peers(*run);
    finish_admission(*run);
}

// Asks peers until the job has, or has asked for, all the members it wants, or no peer is left.
void Pmd::ask_peers(Run& run) {
    while (run.members.size() + run.asked < run.request.nodes &&
           run.next_peer < run.candidates.size()) {
        ask_next_peer(run);
    }
}

// Sends SPAWN_REQ to the next candidate this job has not asked. A peer that cannot be
// reached counts as one that declined: the caller's loop goes on to the one after.
void Pmd::ask_next_peer(Run& run) {
    ask_peer(run, run.candidates[run.next_peer++], 0);
}

// A running job with fewer members than it asked for is offered to a peer newly heard
// (docs/PROTOCOL.md, 8.3): SPAWN_REQ with MID_RUN, one peer at a time for each job.
void Pmd::refill(NodeId node, Endpoint peer) {
    for (Run& run : runs_) {
        const bool member = std::any_of(run.members.begin(), run.members.end(),
                                        [node](const SegMapMember& m) { return m.node == node; });
        if (!leaving_ && run.launcher != 0 && run.wants.region_bytes != 0 && !run.admitting &&
            run.asked == 0 && run.members.size() < run.request.nodes && !member) {
            ask_peer(run, peer, kSpawnMidRun);
        }
    }
}

void Pmd::ask_peer(Run& run, Endpoint peer, std::uint16_t flags) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(peer.ipv4);
    to.sin_port = htons(peer.port);
    sockaddr_in mine{};
    socklen_t length = sizeof mine;
    // ponytail: one connection for each request, and a blocking connect on the daemon's one
    // thread; keep a connection to each peer and connect without blocking when a peer can be
    // far away or slow.
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (fd < 0 || ::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&mine), &length) != 0) {
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        if (fd >= 0) {
            ::close(fd);
        }
        platform_.logger->log(LogLevel::kWarn, "peer_unreachable",
                              JsonObject{{"peer", JsonValue{dotted(peer.ipv4)}},
                                         {"port", JsonValue{std::int64_t{peer.port}}}});
        return;
    }
    connections_.push_back(Connection{next_connection_++, fd, false, {}});
    const std::uint64_t connection = connections_.back().id;

    SpawnReqPayload request;
    request.launcher_node = config_.node;
    request.flags = flags;
    request.launcher_addr = ntohl(mine.sin_addr.s_addr);  // this node, as that peer reaches it
    request.launcher_port = run.members.front().port;
    request.threads_per_node = run.wants.threads_per_node;
    request.region_bytes = run.wants.region_bytes;
    request.binary_hash = run.request.binary_hash;
    request.nodes = run.request.nodes;
    request.path = run.request.path;
    request.cwd = run.request.cwd;
    request.argv = run.request.argv;
    const ReqId req{next_req_++};
    asks_[req.value] = Ask{run.job, connection, peer, flags != 0};
    run.asked++;
    reply(connection, Opcode::kSpawnReq, run.job, kNoNode, req, request);
}

// SPAWN_OK or SPAWN_DECLINE from a peer this daemon asked.
void Pmd::on_spawn_answer(const FrameHeader& header, std::span<const std::byte> payload) {
    const auto asked = asks_.find(header.req.value);
    if (asked == asks_.end()) {
        return;
    }
    const Ask ask = asked->second;
    asks_.erase(asked);
    Run* run = run_of(ask.job);
    if (run == nullptr) {
        return;  // the job ended while the peer was starting its worker
    }
    run->asked--;
    const Result<SpawnOkPayload> ok = wire_decode_spawn_ok(payload);
    if (ask.mid_run) {
        if (header.opcode == Opcode::kSpawnOk && ok.ok()) {
            const SegMapMember joined{ok.value().node,      Slot{0},
                                      kMemberNoHomes,       ask.peer.ipv4,
                                      ok.value().data_port, ok.value().ram_commit};
            run->members.push_back(joined);
            reply(run->launcher, Opcode::kLMember, run->job, kNoNode, kNoReq,
                  LMemberPayload{joined});
        }
        platform_.logger->log(
            LogLevel::kInfo, "refill",
            JsonObject{{"job", JsonValue{std::int64_t{run->job.value}}},
                       {"peer", JsonValue{dotted(ask.peer.ipv4)}},
                       {"joined", JsonValue{header.opcode == Opcode::kSpawnOk && ok.ok()}}});
        return;
    }
    if (header.opcode == Opcode::kSpawnOk && ok.ok()) {
        run->links[ok.value().node.value] = ask.connection;  // for LEDGER_SYNC at the end
        run->members.push_back(SegMapMember{ok.value().node, Slot{0}, 0, ask.peer.ipv4,
                                            ok.value().data_port, ok.value().ram_commit});
        run->committed += ok.value().ram_commit + ok.value().spill_commit;
        run->unlimited = run->unlimited || ok.value().ram_commit == 0;
    } else {
        const Result<StatusTextPayload> why = wire_decode_spawn_decline(payload);
        platform_.logger->log(
            LogLevel::kWarn, "peer_declined",
            JsonObject{{"job", JsonValue{std::int64_t{run->job.value}}},
                       {"peer", JsonValue{dotted(ask.peer.ipv4)}},
                       {"why", JsonValue{why.ok() ? why.value().message : std::string{"?"}}}});
        ask_peers(*run);  // the next peer takes its place
    }
    finish_admission(*run);
}

// Answers the launcher once no request is outstanding: with N members, or with as many as
// accepted when the peers ran out (docs/PROTOCOL.md, [GATE P8]).
void Pmd::finish_admission(Run& run) {
    if (!run.admitting || run.asked != 0) {
        return;
    }
    // The members' own commitments decide ([GATE P8]), with what they have free when fewer
    // accepted than were asked: each committed its share of N, and grows when the map makes it
    // home for more (L_SEGMENTS). A seed not heard before may give less than the job needs. The
    // launcher then ends, and the workers already started end with it.
    std::uint64_t capacity = run.committed;
    if (run.members.size() < run.request.nodes) {
        const JobCommit free = free_now();
        capacity += free.ram + free.spill;
        const std::scoped_lock hold{peers_mu_};
        for (const Peer& peer : table_->peers()) {
            const bool member =
                std::any_of(run.members.begin(), run.members.end(),
                            [&peer](const SegMapMember& m) { return m.node == peer.node; });
            capacity += member ? peer.hello.free_ram + peer.hello.free_spill : 0;
        }
    }
    if (!run.unlimited && run.wants.region_bytes > admissible(capacity)) {
        refuse(run, admissible(capacity), run.members.size());
        return;
    }
    run.admitting = false;
    run.answered = true;
    reply(run.launcher, Opcode::kLAdmitOk, run.job, kNoNode, kNoReq,
          LAdmitOkPayload{run.slots, run.members});
    reply(run.launcher, Opcode::kLQuota, run.job, kNoNode, kNoReq,  // what this node committed
          quota_of(run.slots, run.commit));
    platform_.logger->log(
        LogLevel::kInfo, "admitted",
        JsonObject{{"job", JsonValue{std::int64_t{run.job.value}}},
                   {"members", JsonValue{static_cast<std::int64_t>(run.members.size())}},
                   {"asked_for", JsonValue{std::int64_t{run.request.nodes}}}});
}

// L_LEAVE_REQ from `pm leave`. A node that launched a job still running does not leave
// unless forced, which ends those jobs (docs/PLAN.md, changes from the HLD, 10).
void Pmd::on_leave_req(Connection& connection, std::span<const std::byte> payload) {
    const Result<LLeaveReqPayload> asked = wire_decode_l_leave_req(payload);
    const auto running =
        std::find_if(runs_.begin(), runs_.end(), [](const Run& r) { return r.launcher != 0; });
    StatusTextPayload answer{Status::kOk,
                             "node " + std::to_string(config_.node.value) + " is leaving"};
    if (!asked.ok()) {
        answer = {Status::kProtocol, "a malformed L_LEAVE_REQ"};
    } else if (leaving_) {
        answer = {Status::kLeaving, "this node is already leaving"};
    } else if (running != runs_.end() && !asked.value().force) {
        answer = {Status::kDeclined, "this node launched job " +
                                         std::to_string(running->job.value) +
                                         ", which is still running; pm leave --force ends it"};
    }
    reply(connection.id, Opcode::kLLeaveReply, JobId{0}, kNoNode, kNoReq, answer);
    if (answer.status != Status::kOk) {
        return;
    }
    for (const Run& run : runs_) {
        if (run.launcher != 0) {
            reply(run.launcher, Opcode::kLAbort, run.job, kNoNode, kNoReq,
                  StatusTextPayload{Status::kUserAbort, answer.message});
        }
    }
    begin_leave(kLeaveAsked);
}

// Leave, step 1: the launcher's pmd of every job served here is told (LEAVE_INTENT, on the
// control connection its SPAWN_REQ came on), and so is the worker itself (L_LEAVE). Beacons say
// "leaving" from now on, so nobody asks this node for a worker again.
void Pmd::begin_leave(std::uint8_t reason) {
    leaving_ = true;
    set_member("leaving", "");
    // A worker still starting is not given to its job: declined, and stopped.
    for (const Spawn& spawn : spawns_) {
        decline(spawn, Status::kLeaving,
                "node " + std::to_string(config_.node.value) + " is leaving");
        ::kill(spawn.pid, SIGKILL);
    }
    spawns_.clear();
    for (const Worker& worker : workers_) {
        reply(worker.control, Opcode::kLeaveIntent, worker.job, kNoNode, kNoReq,
              LeaveIntentPayload{config_.node, reason});
        reply(worker.local, Opcode::kLLeave, worker.job, kNoNode, kNoReq, LLeavePayload{reason});
    }
    platform_.logger->log(
        LogLevel::kInfo, "leaving",
        JsonObject{{"workers", JsonValue{static_cast<std::int64_t>(workers_.size())}},
                   {"reason", JsonValue{std::int64_t{reason}}}});
}

// ---- the ledger (M5-1) -----------------------------------------------------------------------

// L_CHUNK from a job process here. A worker's own chunk is this node's work for the launcher's
// job; a launcher reports its peers' chunks, their work for this node's job. A node's own work
// on its own job is no peer's business and is not recorded.
void Pmd::on_chunk(const Connection& connection, const FrameHeader& header,
                   std::span<const std::byte> payload) {
    const Result<LChunkPayload> chunk = wire_decode_l_chunk(payload);
    if (!chunk.ok() || !ledger_) {
        return;
    }
    const LChunkPayload& c = chunk.value();
    share_slots();  // a chunk ended: a task boundary, where a slot may change hands
    const bool ours =
        std::any_of(workers_.begin(), workers_.end(),
                    [&](const Worker& w) { return w.local == connection.id; }) ||
        (run_of(header.job) != nullptr && run_of(header.job)->launcher == connection.id);
    if (ours && c.node == config_.node) {  // what a chunk of the task costs here (M5-2)
        ledger_->add_own_chunk(c.task_id, c.indexes, Nanos{static_cast<std::int64_t>(c.cpu_ns)});
    }
    const auto worker = std::find_if(workers_.begin(), workers_.end(), [&](const Worker& w) {
        return w.local == connection.id && w.job == header.job;
    });
    Run* run = run_of(header.job);
    if (worker != workers_.end() && c.node == config_.node && worker->launcher != config_.node) {
        ledger_->add_chunk(worker->launcher, header.job, Direction::kForPeer, c.task_id, c.indexes,
                           Nanos{static_cast<std::int64_t>(c.cpu_ns)});
    } else if (run != nullptr && run->launcher == connection.id && c.node != config_.node) {
        ledger_->add_chunk(c.node, header.job, Direction::kByPeer, c.task_id, c.indexes, Nanos{0});
    }
}

// L_SEGMENTS: the homes at a new epoch. What the previous counts held until now is added first.
void Pmd::on_segments(const Connection& connection, const FrameHeader& header,
                      std::span<const std::byte> payload) {
    Result<LSegmentsPayload> segments = wire_decode_l_segments(payload);
    if (!segments.ok()) {
        return;
    }
    JobCommit* commit = nullptr;
    std::uint16_t slots = 0;
    for (Worker& w : workers_) {
        if (w.local == connection.id && w.job == header.job) {
            accrue(w.job, w.launcher, w.segments, w.since);
            w.segments = segments.value().entries;
            commit = &w.commit;
            slots = w.slots;
        }
    }
    if (Run* run = run_of(header.job); run != nullptr && run->launcher == connection.id) {
        accrue(run->job, config_.node, run->segments, run->since);
        run->segments = segments.value().entries;
        commit = &run->commit;
        slots = run->slots;
    }
    std::uint64_t home = 0;  // what the new map makes this node home for
    for (const LSegmentsEntry& entry : segments.value().entries) {
        home += entry.node == config_.node ? std::uint64_t{entry.segments} * kSegmentSize : 0;
    }
    // More than its commitment holds, after a leave or on fewer nodes than asked for: grow it,
    // or end the job (docs/PROTOCOL.md, L_SEGMENTS).
    if (commit != nullptr && config_.cap_ram != 0 &&
        home > commit->ram - commit->cache + commit->spill) {
        const JobCommit free = free_now();
        const bool grown = grow_commit(*commit, home, free.ram, free.spill);
        if (grown) {
            reply(connection.id, Opcode::kLQuota, header.job, kNoNode, kNoReq,
                  quota_of(slots, *commit));
        } else {
            reply(connection.id, Opcode::kLAbort, header.job, kNoNode, kNoReq,
                  StatusTextPayload{Status::kNoCapacity,
                                    "node " + std::to_string(config_.node.value) +
                                        " cannot hold the " + mb(home) +
                                        " it is made home for: " + mb(free.ram) + " of RAM and " +
                                        mb(free.spill) + " of spill free"});
        }
        platform_.logger->log(
            grown ? LogLevel::kInfo : LogLevel::kWarn, grown ? "commit_grown" : "commit_short",
            JsonObject{{"job", JsonValue{std::int64_t{header.job.value}}},
                       {"home", JsonValue{static_cast<std::int64_t>(home)}},
                       {"ram", JsonValue{static_cast<std::int64_t>(commit->ram)}},
                       {"spill", JsonValue{static_cast<std::int64_t>(commit->spill)}}});
    }
    share_slots();  // the home-store RAM a job holds here has changed
}

// Segment-seconds from `since` until now. On a worker's node, the segments this node hosts for
// the launcher's job; on the launcher's, those each peer hosts for this node's job.
void Pmd::accrue(JobId job, NodeId launcher, std::vector<LSegmentsEntry>& segments, Nanos& since) {
    const Nanos now = platform_.clock->now();
    const double seconds = std::chrono::duration<double>(now - since).count();
    since = now;
    if (!ledger_) {
        return;
    }
    for (const LSegmentsEntry& entry : segments) {
        const double hosted = seconds * entry.segments;
        if (launcher != config_.node && entry.node == config_.node) {
            ledger_->add_segment_seconds(launcher, job, Direction::kForPeer, hosted);
        } else if (launcher == config_.node && entry.node != config_.node) {
            ledger_->add_segment_seconds(entry.node, job, Direction::kByPeer, hosted);
        }
    }
}

// ---- announcements and the alarm (M5-4)
// ----------------------------------------------------------

// LEDGER_SYNC to `peer`'s pmd on `connection`: this node's counts with it for the job just ended.
void Pmd::send_ledger_sync(std::uint64_t connection, NodeId peer, JobId job) {
    if (ledger_ && connection != 0) {
        reply(connection, Opcode::kLedgerSync, job, peer, kNoReq,
              LedgerSyncPayload{{ledger_job_counts(*ledger_, peer, job)}});
    }
}

// LEDGER_SYNC from a peer: its counts with this node, kept for display and compared.
void Pmd::on_ledger_sync(const FrameHeader& header, std::span<const std::byte> payload) {
    const Result<LedgerSyncPayload> sync = wire_decode_ledger_sync(payload);
    if (!sync.ok() || !ledger_) {
        return;
    }
    for (const LedgerSyncEntry& theirs : sync.value().entries) {
        announced_.jobs[{header.src.value, theirs.job.value}] = theirs;
    }
    // Compared at the next refresh, not here: the last L_CHUNKs of the job's process on this
    // node may still be waiting to be read.
}

// The JSON GET /v1/ledger serves, made here on pmd's thread from the ledger and announcements.
void Pmd::refresh_ledger_status() {
    if (!ledger_) {
        return;
    }
    // The alarm, logged once for each peer, or peer and job, that starts to differ.
    std::set<std::pair<std::uint16_t, std::uint32_t>> off;  // (peer, job); job 0 for totals
    for (const auto& [peer, theirs] : announced_.totals) {
        if (totals_disagree(ledger_totals(*ledger_, NodeId{peer}), theirs)) {
            off.insert({peer, 0});
        }
    }
    for (const auto& [key, theirs] : announced_.jobs) {
        if (job_disagrees(ledger_job_counts(*ledger_, NodeId{key.first}, JobId{key.second}),
                          theirs)) {
            off.insert(key);
        }
    }
    for (const auto& [peer, job] : off) {
        if (!disagreeing_.contains({peer, job})) {
            platform_.logger->log(LogLevel::kWarn, "ledger_disagreement",
                                  JsonObject{{"peer", JsonValue{std::int64_t{peer}}},
                                             {"job", JsonValue{std::int64_t{job}}}});
        }
    }
    disagreeing_ = std::move(off);
    std::string json =
        platform_.json->encode(ledger_status(*ledger_, announced_, config_.node, ledger_scale_));
    const std::scoped_lock hold{status_mu_};
    ledger_json_ = std::move(json);
}

// ---- the status API: jobs, pages, events (M6-1)
// --------------------------------------------------

// L_STATS from a job process here: kept, two at a time, for counts and rates.
void Pmd::on_stats(const Connection& connection, const FrameHeader& header,
                   std::span<const std::byte> payload) {
    Result<LStatsPayload> stats = wire_decode_l_stats(payload);
    if (!stats.ok()) {
        return;
    }
    const Nanos now = platform_.clock->now();
    for (Worker& w : workers_) {
        if (w.local == connection.id) {
            w.stats.take(stats.value(), now);
        }
    }
    if (Run* run = run_of(header.job); run != nullptr && run->launcher == connection.id) {
        run->stats.take(std::move(stats).value(), now);
    }
}

std::vector<JobView> Pmd::job_views() const {
    std::vector<JobView> views;
    for (const Run& run : runs_) {
        if (run.launcher != 0) {
            views.push_back({run.job, true, config_.node, run.slots, &run.stats});
        }
    }
    for (const Worker& w : workers_) {
        views.push_back({w.job, false, w.launcher, w.slots, &w.stats});
    }
    return views;
}

// Once a second: the JSON the status API serves, remade on pmd's thread; and the events of the
// second: the rates of every job here, and peers and jobs that came or went since the last.
void Pmd::tick_status() {
    const std::vector<JobView> views = job_views();
    JsonArray jobs;
    JsonArray rates;
    std::map<std::uint32_t, std::string> pages;
    std::set<std::uint32_t> jobs_now;
    for (const JobView& view : views) {
        jobs.push_back(job_status(view));
        rates.push_back(job_rates(view));
        pages[view.job.value] = platform_.json->encode(pages_status(view, config_.node));
        jobs_now.insert(view.job.value);
    }
    std::set<std::uint16_t> peers_now;
    {
        const std::scoped_lock hold{peers_mu_};
        for (const Peer& peer : table_->peers()) {
            peers_now.insert(peer.node.value);
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();  // wall-clock time, for the reader
    std::vector<std::string> fresh;
    const auto changes = [&](const auto& before, const auto& after, const char* came,
                             const char* went, const char* key) {
        for (const auto id : after) {
            if (!before.contains(id)) {
                fresh.push_back(sse_event(
                    came, platform_.json->encode(JsonValue{
                              JsonObject{{key, JsonValue{static_cast<std::int64_t>(id)}}}})));
            }
        }
        for (const auto id : before) {
            if (!after.contains(id)) {
                fresh.push_back(sse_event(
                    went, platform_.json->encode(JsonValue{
                              JsonObject{{key, JsonValue{static_cast<std::int64_t>(id)}}}})));
            }
        }
    };
    changes(seen_peers_, peers_now, "join", "leave", "peer");
    changes(seen_jobs_, jobs_now, "job_start", "job_end", "job");
    seen_peers_ = std::move(peers_now);
    seen_jobs_ = std::move(jobs_now);
    fresh.push_back(sse_event("stats", platform_.json->encode(JsonValue{JsonObject{
                                           {"node", JsonValue{std::int64_t{config_.node.value}}},
                                           {"time_ms", JsonValue{static_cast<std::int64_t>(ms)}},
                                           {"jobs", JsonValue{std::move(rates)}}}})));
    std::string jobs_json = platform_.json->encode(
        JsonValue{JsonObject{{"node", JsonValue{std::int64_t{config_.node.value}}},
                             {"jobs", JsonValue{std::move(jobs)}}}});
    {
        const std::scoped_lock hold{status_mu_};
        jobs_json_ = std::move(jobs_json);
        pages_json_ = std::move(pages);
        for (std::string& event : fresh) {
            events_.push_back(std::move(event));
        }
        // ponytail: the last 600 events are kept (minutes of a quiet pool); a stream that
        // falls further behind skips to the oldest kept.
        while (events_.size() > 600) {
            events_.erase(events_.begin());
            events_before_++;
        }
    }
    events_changed_.notify_all();
}

// GET /v1/events, on a server thread: every event from now on, until the client goes or pmd
// stops.
void Pmd::stream_events(const HttpWrite& write) {
    std::unique_lock hold{status_mu_};
    std::uint64_t next = events_before_ + events_.size();  // only what comes after
    hold.unlock();
    if (!write("retry: 1000\n\n")) {
        return;
    }
    hold.lock();
    while (!streams_end_) {
        events_changed_.wait_for(hold, std::chrono::seconds{2});
        next = std::max(next, events_before_);
        std::string pending;
        for (; next < events_before_ + events_.size(); next++) {
            pending += events_[next - events_before_];
        }
        hold.unlock();
        if (!pending.empty() && !write(pending)) {
            return;  // the client has gone
        }
        hold.lock();
    }
}

// ---- worker slots (M5-3) ----------------------------------------------------------------------

namespace {

// The part of `home` bytes of home-store RAM that the segments a job has homed on `self` take.
double ram_share(const std::vector<LSegmentsEntry>& segments, NodeId self, std::uint64_t home) {
    std::uint64_t held = 0;
    for (const LSegmentsEntry& entry : segments) {
        held += entry.node == self ? std::uint64_t{entry.segments} * kSegmentSize : 0;
    }
    return home == 0 ? 0 : std::min(1.0, static_cast<double>(held) / static_cast<double>(home));
}

}  // namespace

// Shares this node's worker slots among its jobs (pmd/scheduler.h) and tells each process
// whose share changed with L_QUOTA; a launcher only once its admission is answered. The process
// `registering` gets its share in the reply to its L_REGISTER instead.
void Pmd::share_slots(std::uint64_t registering) {
    struct Holder {
        std::uint64_t connection;
        JobId job;
        std::uint16_t* slots;
        std::uint32_t* waited;
        bool may_push;
        JobCommit commit;
    };
    const std::uint64_t home = config_.cap_ram - config_.cap_ram * cache_share_ / 100;
    const auto want = [this](std::uint16_t bound) {
        return bound == 0 ? config_.cap_cores : std::min(bound, config_.cap_cores);
    };
    std::vector<SlotDemand> demands;
    std::vector<Holder> holders;
    for (Run& run : runs_) {
        if (run.launcher != 0) {
            demands.push_back({true, want(run.wants.threads_per_node),
                               ram_share(run.segments, config_.node, home), 1, run.waited});
            holders.push_back(
                {run.launcher, run.job, &run.slots, &run.waited, run.answered, run.commit});
        }
    }
    for (Worker& w : workers_) {
        const double weight = ledger_ ? slot_weight(*ledger_, w.launcher, ledger_scale_) : 1.0;
        demands.push_back(
            {false, want(w.want), ram_share(w.segments, config_.node, home), weight, w.waited});
        holders.push_back({w.local, w.job, &w.slots, &w.waited, true, w.commit});
    }
    const std::vector<std::uint16_t> slots = allocate_slots(config_.cap_cores, demands);
    for (std::size_t i = 0; i < holders.size(); i++) {
        const Holder& h = holders[i];
        *h.waited = slots[i] == 0 ? *h.waited + 1 : 0;
        if (slots[i] != *h.slots) {
            *h.slots = slots[i];
            if (h.may_push && h.connection != registering) {
                reply(h.connection, Opcode::kLQuota, h.job, kNoNode, kNoReq,
                      quota_of(slots[i], h.commit));
            }
        }
    }
}

// LEAVE_INTENT from a worker's pmd: passed to the launcher of that job.
void Pmd::on_leave_intent(const FrameHeader& header, std::span<const std::byte> payload) {
    const Result<LeaveIntentPayload> intent = wire_decode_leave_intent(payload);
    Run* run = run_of(header.job);
    if (intent.ok() && run != nullptr && run->launcher != 0) {
        reply(run->launcher, Opcode::kLLeaveIntent, run->job, kNoNode, kNoReq, intent.value());
        // The job is a member short now: a peer heard from now on may refill it (M4-7).
        std::erase_if(run->members,
                      [&](const SegMapMember& m) { return m.node == intent.value().node; });
    }
}

// A connection has closed. pmrun going away while its job runs means the user interrupted it:
// the launcher is told to end the job. The launcher going away ends the job here. A peer
// going away with a request outstanding counts as one that declined.
void Pmd::on_closed(const Connection& connection) {
    std::erase_if(workers_, [&](Worker& w) {
        if (w.local == connection.id) {
            accrue(w.job, w.launcher, w.segments, w.since);  // its last segments, until now
            send_ledger_sync(w.control, w.launcher, w.job);
        }
        return w.local == connection.id;
    });
    for (auto ask = asks_.begin(); ask != asks_.end();) {
        if (ask->second.connection != connection.id) {
            ++ask;
            continue;
        }
        Run* run = run_of(ask->second.job);
        ask = asks_.erase(ask);
        if (run != nullptr) {
            run->asked--;
            ask_peers(*run);
            finish_admission(*run);
        }
    }
    for (Run& run : runs_) {
        if (run.pmrun == connection.id) {
            run.pmrun = 0;
            if (run.launcher != 0) {
                reply(run.launcher, Opcode::kLAbort, run.job, kNoNode, kNoReq,
                      StatusTextPayload{Status::kUserAbort, "interrupted"});
            }
        }
    }
    std::erase_if(runs_, [&](Run& run) {
        if (run.launcher == connection.id) {
            accrue(run.job, config_.node, run.segments, run.since);
            for (const auto& [peer, link] : run.links) {
                send_ledger_sync(link, NodeId{peer}, run.job);
            }
        }
        return run.launcher == connection.id || (run.launcher == 0 && run.pmrun == 0);
    });
    share_slots();  // a job may have ended here: its slots go to the others
}

// Collects the workers that have exited. One that never registered is reported to its asker.
void Pmd::reap() {
    std::erase_if(children_, [this](pid_t child) {
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) != child) {
            return false;
        }
        const auto spawn = std::find_if(spawns_.begin(), spawns_.end(),
                                        [child](const Spawn& s) { return s.pid == child; });
        if (spawn != spawns_.end()) {
            decline(*spawn, Status::kInternal,
                    "the worker on node " + std::to_string(config_.node.value) +
                        " exited before it registered");
            spawns_.erase(spawn);
        }
        return true;
    });
}

template <typename Payload>
void Pmd::reply(std::uint64_t connection, Opcode opcode, JobId job, NodeId to, ReqId req,
                const Payload& payload) {
    const auto found =
        std::find_if(connections_.begin(), connections_.end(),
                     [connection](const Connection& c) { return c.id == connection; });
    std::vector<std::byte> bytes(kMaxSpawnReq);  // the largest frame it sends, SPAWN_REQ
    const Result<std::size_t> size = wire_encode(payload, bytes);
    if (found == connections_.end() || found->fd < 0 || !size.ok()) {
        return;  // whoever asked has gone
    }
    FrameHeader header;
    header.opcode = opcode;
    header.job = job;
    header.src = config_.node;
    header.dst = to;
    header.req = req;
    // ponytail: a blocking write on the daemon's one thread; a peer that stops reading stalls
    // it. Queue and poll for writability when the daemon carries more than these few frames.
    static_cast<void>(
        frame_write(found->fd, *platform_.checksum, header, std::span{bytes}.first(size.value())));
}

void Pmd::decline(const Spawn& spawn, Status status, const std::string& message) {
    reply(spawn.asked_on, Opcode::kSpawnDecline, spawn.job, spawn.asker, spawn.req,
          SpawnDeclinePayload{status, message.substr(0, 512)});
    platform_.logger->log(
        LogLevel::kWarn, "spawn_declined",
        JsonObject{{"job", JsonValue{std::int64_t{spawn.job.value}}}, {"why", JsonValue{message}}});
}

}  // namespace paramesh
