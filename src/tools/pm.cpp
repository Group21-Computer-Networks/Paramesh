#include "tools/pm.h"

#include "platform/factory.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <string>
#include <variant>

namespace paramesh {

namespace {

constexpr std::uint16_t kStatusPort = 47002;  // pmd's --http-port default

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

int pm(std::span<const std::string_view> args) {
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
    if ((args.size() != 1 && args.size() != 3) || args[0] != "status") {
        static_cast<void>(std::fputs("usage: pm status [--http-port N]\n", stderr));
        return 2;
    }
    return status(port);
}

}  // namespace paramesh
