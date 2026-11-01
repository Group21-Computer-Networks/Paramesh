#include "tools/pm.h"

#include "net/frame_io.h"
#include "platform/factory.h"
#include "platform/sandbox.h"
#include "wire/payloads.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

namespace paramesh {

namespace {

constexpr std::uint16_t kStatusPort = 47002;  // pmd's --http-port default
constexpr const char* kUsage =
    "usage: pm status [--http-port N]\n"
    "       pm top [--http-port N]\n"
    "       pm leave [--force] [--state-dir DIRECTORY]\n";

std::int64_t number(const JsonObject& object, const char* key) {
    const auto found = object.find(key);
    const auto* value =
        found != object.end() ? std::get_if<std::int64_t>(&found->second.v) : nullptr;
    return value != nullptr ? *value : 0;
}

std::string text(const JsonObject& object, const char* key) {
    const auto found = object.find(key);
    const auto* value =
        found != object.end() ? std::get_if<std::string>(&found->second.v) : nullptr;
    return value != nullptr ? *value : std::string{};
}

const JsonArray* array(const JsonObject& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() ? std::get_if<JsonArray>(&found->second.v) : nullptr;
}

double real(const JsonObject& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return 0;
    }
    if (const auto* value = std::get_if<double>(&found->second.v)) {
        return *value;
    }
    const auto* whole = std::get_if<std::int64_t>(&found->second.v);
    return whole != nullptr ? static_cast<double>(*whole) : 0;
}

const JsonObject* object_at(const JsonObject& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() ? std::get_if<JsonObject>(&found->second.v) : nullptr;
}

// The ledger (GET /v1/ledger, M5-4): this node's raw counts with each peer, the balance and
// weight they give, and what the peer announced, flagged when they differ by more than 10%.
void print_ledger(std::uint16_t port, const JsonCodec& json) {
    const Result<std::string> body = http_get(port, "/v1/ledger");
    const Result<JsonValue> parsed =
        body.ok() ? json.decode(body.value()) : Result<JsonValue>{body.error()};
    const auto* top = parsed.ok() ? std::get_if<JsonObject>(&parsed.value().v) : nullptr;
    const JsonArray* peers = top != nullptr ? array(*top, "peers") : nullptr;
    if (peers == nullptr || peers->empty()) {
        return;
    }
    std::printf("\nledger: chunks and segment-seconds for each other, by this node's count\n");
    std::printf("%-6s %-10s %-10s %-10s %-10s %-10s %-7s %s\n", "PEER", "CHUNKS_FOR", "CHUNKS_BY",
                "SEGS_FOR", "SEGS_BY", "BALANCE", "WEIGHT", "ANNOUNCED");
    for (const JsonValue& each : *peers) {
        const auto* peer = std::get_if<JsonObject>(&each.v);
        const JsonObject* totals = peer != nullptr ? object_at(*peer, "totals") : nullptr;
        if (totals == nullptr) {
            continue;
        }
        const JsonObject* said = object_at(*peer, "announced");
        const auto* off = peer->find("disagree") != peer->end()
                              ? std::get_if<bool>(&peer->find("disagree")->second.v)
                              : nullptr;
        const char* announced = "-";
        if (said != nullptr) {
            announced = off != nullptr && *off ? "DISAGREES by more than 10%" : "agrees";
        }
        std::printf("%-6lld %-10lld %-10lld %-10lld %-10lld %-10.1f %-7.2f %s\n",
                    static_cast<long long>(number(*peer, "peer")),
                    static_cast<long long>(number(*totals, "chunks_for_peer")),
                    static_cast<long long>(number(*totals, "chunks_by_peer")),
                    static_cast<long long>(number(*totals, "segsec_for_peer")),
                    static_cast<long long>(number(*totals, "segsec_by_peer")),
                    real(*peer, "balance"), real(*peer, "weight"), announced);
    }
}

int status(std::uint16_t port) {
    const Result<std::string> body = http_get(port, "/v1/peers");
    if (!body.ok()) {
        static_cast<void>(std::fprintf(stderr,
                                       "pm: no pmd answers on 127.0.0.1:%u; is it running here?\n",
                                       static_cast<unsigned>(port)));
        return 1;
    }
    Result<Platform> platform =
        platform_open({}, PlatformOptions{kNoNode, JobId{0}, 2, LogLevel::kError});
    const Result<JsonValue> parsed = platform.ok() ? platform.value().json->decode(body.value())
                                                   : Result<JsonValue>{platform.error()};
    const auto* top = parsed.ok() ? std::get_if<JsonObject>(&parsed.value().v) : nullptr;
    if (top == nullptr) {
        static_cast<void>(std::fputs("pm: pmd sent an answer pm cannot read\n", stderr));
        return 1;
    }
    std::printf("node %lld (this node), %lld cores\n", static_cast<long long>(number(*top, "node")),
                static_cast<long long>(number(*top, "cap_cores")));
    std::printf("%-6s %-16s %-8s %-8s %-6s %-5s %-5s %s\n", "NODE", "ADDRESS", "CONTROL", "STATE",
                "CORES", "FREE", "JOBS", "SILENT");
    if (const JsonArray* peers = array(*top, "peers"); peers != nullptr) {
        for (const JsonValue& each : *peers) {
            const auto* peer = std::get_if<JsonObject>(&each.v);
            if (peer == nullptr) {
                continue;
            }
            std::printf(
                "%-6lld %-16s %-8lld %-8s %-6lld %-5lld %-5lld %.1f s\n",
                static_cast<long long>(number(*peer, "node")), text(*peer, "address").c_str(),
                static_cast<long long>(number(*peer, "control_port")), text(*peer, "state").c_str(),
                static_cast<long long>(number(*peer, "cap_cores")),
                static_cast<long long>(number(*peer, "free_cores")),
                static_cast<long long>(number(*peer, "jobs")),
                static_cast<double>(number(*peer, "silent_ms")) / 1000.0);
        }
    }
    print_ledger(port, *platform.value().json);
    if (const JsonArray* clashes = array(*top, "clashes"); clashes != nullptr) {
        for (const JsonValue& each : *clashes) {
            if (const auto* clash = std::get_if<JsonObject>(&each.v); clash != nullptr) {
                std::printf("ID clash: node %lld is also used by %s\n",
                            static_cast<long long>(number(*clash, "node")),
                            text(*clash, "address").c_str());
            }
        }
    }
    return 0;
}

}  // namespace

Result<std::string> http_get(std::uint16_t port, std::string_view path) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(port);
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (fd < 0 || ::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0) {
        if (fd >= 0) {
            ::close(fd);
        }
        return Error{Errc::kIo, 0, "connect to the status API"};
    }
    const std::string request =
        "GET " + std::string{path} + " HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::string answer;
    if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
        static_cast<ssize_t>(request.size())) {
        std::array<char, 4096> chunk{};
        for (ssize_t got = 0; (got = ::recv(fd, chunk.data(), chunk.size(), 0)) > 0;) {
            answer.append(chunk.data(), static_cast<std::size_t>(got));
        }
    }
    ::close(fd);
    const auto body = answer.find("\r\n\r\n");
    if (!answer.starts_with("HTTP/1.") || answer.find(" 200 ") > 12 || body == std::string::npos) {
        return Error{Errc::kProtocol, 0, "read the status API's answer"};
    }
    return answer.substr(body + 4);
}

// `pm leave [--force] [--state-dir DIRECTORY]`: asks the local pmd to leave.
int leave(std::span<const std::string_view> args) {
    bool force = false;
    std::string state_dir;
    for (std::size_t at = 1; at < args.size(); at++) {
        if (args[at] == "--force") {
            force = true;
        } else if (args[at] == "--state-dir" && at + 1 < args.size()) {
            state_dir = args[++at];
        } else {
            static_cast<void>(std::fputs(kUsage, stderr));
            return 2;
        }
    }
    if (state_dir.empty()) {
        const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe): one thread
        state_dir = std::string{home != nullptr ? home : "."} + "/.local/state/paramesh";
    }
    const std::string socket = state_dir + "/pmd.sock";
    sockaddr_un to{};
    to.sun_family = AF_UNIX;
    if (socket.size() < sizeof to.sun_path) {
        std::copy(socket.begin(), socket.end(), std::begin(to.sun_path));
    }
    const int pmd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Result<Platform> platform =
        platform_open({}, PlatformOptions{kNoNode, JobId{0}, 2, LogLevel::kWarn});
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    if (pmd < 0 || !platform.ok() ||
        ::connect(pmd, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0) {
        static_cast<void>(
            std::fprintf(stderr, "pm: cannot reach pmd at %s; is it running?\n", socket.c_str()));
        return 1;
    }
    std::array<std::byte, 4> request{};
    static_cast<void>(wire_encode(LLeaveReqPayload{force}, request));
    FrameHeader header;
    header.opcode = Opcode::kLLeaveReq;
    const Checksum& checksum = *platform.value().checksum;
    const Result<Frame> answer = frame_write(pmd, checksum, header, request).ok()
                                     ? frame_read(pmd, checksum)
                                     : Result<Frame>{Error{Errc::kIo}};
    ::close(pmd);
    const Result<StatusTextPayload> said = answer.ok()
                                               ? wire_decode_spawn_decline(answer.value().payload)
                                               : Result<StatusTextPayload>{answer.error()};
    if (!said.ok() || answer.value().header.opcode != Opcode::kLLeaveReply) {
        static_cast<void>(std::fputs("pm: pmd gave no answer\n", stderr));
        return 1;
    }
    static_cast<void>(std::printf("pm: %s\n", said.value().message.c_str()));
    return said.value().status == Status::kOk ? 0 : 1;
}

// `pm sandbox DIRECTORY program [arguments...]`: runs the compiler for the playground, confined
// to DIRECTORY and the toolchain.
int sandboxed(std::span<const std::string_view> args) {
    std::vector<std::string> words{args.begin() + 2, args.end()};
    std::vector<char*> argv;
    argv.reserve(words.size() + 1);
    for (std::string& word : words) {
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);
    if (!sandbox_enter(std::string{args[1]}.c_str(), SandboxProfile::kCompiler)) {
        static_cast<void>(std::fputs("pm: cannot sandbox the program\n", stderr));
        return 126;
    }
    ::execvp(argv[0], argv.data());
    static_cast<void>(std::fprintf(stderr, "pm: cannot run %s\n", argv[0]));
    return 127;
}

int pm(std::span<const std::string_view> args) {
    if (!args.empty() && args[0] == "leave") {
        return leave(args);
    }
    if (args.size() >= 3 && args[0] == "sandbox") {
        return sandboxed(args);
    }
    std::uint16_t port = kStatusPort;
    if (args.size() == 3 && args[1] == "--http-port") {
        unsigned value = 0;
        const auto [end, error] =
            std::from_chars(args[2].data(), args[2].data() + args[2].size(), value);
        if (error != std::errc{} || end != args[2].data() + args[2].size() || value == 0 ||
            value > 65535) {
            static_cast<void>(std::fputs("pm: --http-port must be 1 to 65535\n", stderr));
            return 2;
        }
        port = static_cast<std::uint16_t>(value);
    }
    if ((args.size() != 1 && args.size() != 3) || (args[0] != "status" && args[0] != "top")) {
        static_cast<void>(std::fputs(kUsage, stderr));
        return 2;
    }
    return args[0] == "top" ? top(port) : status(port);
}

}  // namespace paramesh
