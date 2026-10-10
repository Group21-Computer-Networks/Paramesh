#include "pmd/pmd.h"

#include "pmd/frame_io.h"

#include <arpa/inet.h>
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
#include <thread>

// POSIX leaves the declaration to the program.
// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;

namespace paramesh {

namespace {

constexpr int kReapEveryMs = 200;  // how often run() looks for workers that have exited
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

// "10.0.0.2" or "10.0.0.2:47002".
bool to_peer(std::string_view text, Endpoint& out) {
    const auto colon = text.find(':');
    const std::string address{text.substr(0, colon)};
    in_addr parsed{};
    out.port = kControlPort;
    if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1 ||
        (colon != std::string_view::npos && !to_u16(text.substr(colon + 1), 1, out.port))) {
        return false;
    }
    out.ipv4 = ntohl(parsed.s_addr);
    return true;
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
    return "usage: pmd --node-id N [--cap-cores N] [--peers address[:port],...]\n"
           "           [--control-port N] [--state-dir DIRECTORY]\n"
           "  --node-id       this node's ID, 1 to 65534\n"
           "  --cap-cores     the most worker threads a job may run here (default: all "
           "processors)\n"
           "  --peers         the daemons of the other nodes; the port defaults to 47001\n"
           "  --control-port  the TCP port for other daemons (default 47001)\n"
           "  --state-dir     where pmd.sock is made (default ~/.local/state/paramesh)\n";
}

Result<PmdConfig> pmd_parse_args(std::span<const std::string_view> args) {
    PmdConfig config;
    for (std::size_t i = 0; i < args.size(); i += 2) {
        if (i + 1 == args.size()) {
            return bad_flag("the last flag has no value");
        }
        const std::string_view flag = args[i];
        const std::string_view value = args[i + 1];
        if (flag == "--node-id") {
            if (!to_u16(value, 1, config.node.value) || config.node == kAllNodes) {
                return bad_flag("--node-id must be 1 to 65534");
            }
        } else if (flag == "--cap-cores") {
            if (!to_u16(value, 1, config.cap_cores)) {
                return bad_flag("--cap-cores must be 1 to 65535");
            }
        } else if (flag == "--control-port") {
            if (!to_u16(value, 0, config.control_port)) {
                return bad_flag("--control-port must be 0 to 65535");
            }
        } else if (flag == "--state-dir") {
            config.state_dir = value;
        } else if (flag == "--peers") {
            for (std::string_view rest = value; !rest.empty();) {
                const auto comma = rest.find(',');
                Endpoint peer;
                if (!to_peer(rest.substr(0, comma), peer)) {
                    return bad_flag("--peers must be address[:port], separated by commas");
                }
                config.peers.push_back(peer);
                rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 1);
            }
        } else {
            return bad_flag("a flag pmd does not know");
        }
    }
    if (config.node == kNoNode) {
        return bad_flag("--node-id is required");
    }
    if (config.state_dir.empty()) {
        const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): at start-up
        config.state_dir = std::string{home != nullptr ? home : "."} + "/.local/state/paramesh";
    }
    if (config.cap_cores == 0) {
        config.cap_cores = static_cast<std::uint16_t>(
            std::clamp<unsigned>(std::thread::hardware_concurrency(), 1, 65535));
    }
    return config;
}

Pmd::Pmd(PmdConfig config, const Platform& platform)
    : config_(std::move(config)), platform_(platform) {}

Pmd::~Pmd() {
    for (const Connection& connection : connections_) {
        ::close(connection.fd);
    }
    for (const int fd : {control_listener_, local_listener_, stop_fd_}) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    if (local_listener_ >= 0) {
        ::unlink(socket_path_.c_str());
    }
}

Result<void> Pmd::open() {
    std::error_code failed;
    std::filesystem::create_directories(config_.state_dir, failed);
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
    platform_.logger->log(
        LogLevel::kInfo, "pmd_start",
        JsonObject{{"control_port", JsonValue{std::int64_t{config_.control_port}}},
                   {"socket", JsonValue{socket_path_}},
                   {"cap_cores", JsonValue{std::int64_t{config_.cap_cores}}}});
    return {};
}

void Pmd::stop() const noexcept {
    const std::uint64_t one = 1;
    const ssize_t written = ::write(stop_fd_, &one, sizeof one);
    static_cast<void>(written);
}

void Pmd::run() {
    for (;;) {
        std::vector<pollfd> fds{
            {stop_fd_, POLLIN, 0}, {control_listener_, POLLIN, 0}, {local_listener_, POLLIN, 0}};
        for (const Connection& connection : connections_) {
            fds.push_back({connection.fd, POLLIN, 0});
        }
        if (::poll(fds.data(), fds.size(), kReapEveryMs) < 0 && errno != EINTR) {
            return;
        }
        if (fds[0].revents != 0) {
            return;
        }
        // The connections polled are the first ones of connections_; accepting only adds more.
        for (std::size_t i = 3; i < fds.size(); i++) {
            Connection& connection = connections_[i - 3];
            if (fds[i].revents != 0 && !read_from(connection)) {
                ::close(connection.fd);
                connection.fd = -1;
            }
        }
        std::erase_if(connections_, [](const Connection& c) { return c.fd < 0; });
        if ((fds[1].revents & POLLIN) != 0) {
            accept_on(control_listener_, false);
        }
        if ((fds[2].revents & POLLIN) != 0) {
            accept_on(local_listener_, true);
        }
        reap();
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
    } else if (connection.local && header.opcode == Opcode::kLRegister) {
        on_register(connection, header, payload);
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
    Spawn spawn{header.job, 0, connection.id, header.src, header.req, {}};
    const Result<SpawnReqPayload> decoded = wire_decode_spawn_req(payload);
    if (!decoded.ok()) {
        decline(spawn, Status::kProtocol, "a malformed SPAWN_REQ");
        return;
    }
    const SpawnReqPayload& request = decoded.value();
    spawn.hash = request.binary_hash;
    const std::string here = " on node " + std::to_string(config_.node.value);

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
// either way the process is told its quota, which in M3 is --cap-cores for every job.
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
                  SpawnOkPayload{config_.node, process.data_port, config_.cap_cores, 0, 0});
        }
        spawns_.erase(spawn);
    }
    reply(connection.id, Opcode::kLQuota, header.job, kNoNode, kNoReq,
          LQuotaPayload{config_.cap_cores});
    platform_.logger->log(LogLevel::kInfo, "register",
                          JsonObject{{"job", JsonValue{std::int64_t{header.job.value}}},
                                     {"pid", JsonValue{std::int64_t{process.pid}}},
                                     {"role", JsonValue{std::int64_t{process.role}}}});
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
    std::array<std::byte, 600> bytes{};  // the largest reply, SPAWN_DECLINE, is 516 bytes
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
