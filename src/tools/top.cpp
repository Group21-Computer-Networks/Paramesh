// pm top (M6-2): this node's jobs, redrawn once a second with terminal escape codes: faults and
// megabytes per second, fault latency, and which pages the node holds. From GET /v1/jobs and
// /v1/jobs/{id}/pages (docs/STATUS_API.md). Ctrl+C ends it and gives the terminal back.

#include "platform/factory.h"
#include "tools/pm.h"

#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <string>
#include <variant>

namespace paramesh {

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): set by the signal handler
volatile std::sig_atomic_t g_quit = 0;

void on_quit(int /*signal*/) {
    g_quit = 1;
}

const JsonObject* as_object(const JsonValue& value) {
    return std::get_if<JsonObject>(&value.v);
}

double number_of(const JsonObject& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return 0;
    }
    if (const auto* real = std::get_if<double>(&found->second.v)) {
        return *real;
    }
    const auto* whole = std::get_if<std::int64_t>(&found->second.v);
    return whole != nullptr ? static_cast<double>(*whole) : 0;
}

std::string text_of(const JsonObject& object, const char* key) {
    const auto found = object.find(key);
    const auto* value =
        found != object.end() ? std::get_if<std::string>(&found->second.v) : nullptr;
    return value != nullptr ? *value : std::string{};
}

// The page ownership line of one job: segments homed here, and pages held S and M.
std::string ownership(std::uint16_t port, long long job, const JsonCodec& json) {
    const Result<std::string> body = http_get(port, "/v1/jobs/" + std::to_string(job) + "/pages");
    const Result<JsonValue> parsed =
        body.ok() ? json.decode(body.value()) : Result<JsonValue>{body.error()};
    const JsonObject* top = parsed.ok() ? as_object(parsed.value()) : nullptr;
    const auto found = top != nullptr ? top->find("segments") : JsonObject::const_iterator{};
    const auto* segments =
        top != nullptr && found != top->end() ? std::get_if<JsonArray>(&found->second.v) : nullptr;
    if (segments == nullptr) {
        return "      pages: -\n";
    }
    long long homed = 0;
    long long shared = 0;
    long long modified = 0;
    for (const JsonValue& each : *segments) {
        if (const JsonObject* s = as_object(each)) {
            const auto here = s->find("home_here");
            homed += here != s->end() && std::get<bool>(here->second.v) ? 1 : 0;
            shared += static_cast<long long>(number_of(*s, "shared"));
            modified += static_cast<long long>(number_of(*s, "modified"));
        }
    }
    return "      pages: home for " + std::to_string(homed) + " segments here; holds " +
           std::to_string(shared) + " shared (S), " + std::to_string(modified) + " modified (M)\n";
}

// One screen: the header, then a line and an ownership line for each job.
std::string frame(std::uint16_t port, const JsonCodec& json) {
    const std::time_t now = std::time(nullptr);
    std::array<char, 16> clock{};
    std::tm local{};
    localtime_r(&now, &local);
    static_cast<void>(std::strftime(clock.data(), clock.size(), "%H:%M:%S", &local));
    std::string out = "\x1b[H\x1b[2J";  // home, clear
    const Result<std::string> body = http_get(port, "/v1/jobs");
    const Result<JsonValue> parsed =
        body.ok() ? json.decode(body.value()) : Result<JsonValue>{body.error()};
    const JsonObject* top = parsed.ok() ? as_object(parsed.value()) : nullptr;
    if (top == nullptr) {
        return out + "pm top  " + clock.data() +
               "  no pmd answers on 127.0.0.1:" + std::to_string(port) + "  (Ctrl+C to quit)\n";
    }
    out += "pm top  node " + std::to_string(static_cast<long long>(number_of(*top, "node"))) +
           "  " + clock.data() + "  (Ctrl+C to quit)\n\n";
    std::array<char, 160> line{};
    static_cast<void>(std::snprintf(
        line.data(), line.size(), "%-8s %-8s %-8s %-5s %10s %9s %9s %8s %8s\n", "JOB", "ROLE",
        "LAUNCHER", "SLOTS", "FAULTS/S", "MB_IN/S", "MB_OUT/S", "P50_US", "P99_US"));
    out += line.data();
    const auto jobs = top->find("jobs");
    const auto* list = jobs != top->end() ? std::get_if<JsonArray>(&jobs->second.v) : nullptr;
    if (list == nullptr || list->empty()) {
        return out + "(no job on this node)\n";
    }
    for (const JsonValue& each : *list) {
        const JsonObject* job = as_object(each);
        if (job == nullptr) {
            continue;
        }
        const JsonObject* latency = nullptr;
        if (const auto found = job->find("fault_latency"); found != job->end()) {
            latency = as_object(found->second);
        }
        const auto id = static_cast<long long>(number_of(*job, "job"));
        static_cast<void>(std::snprintf(
            line.data(), line.size(), "%-8lld %-8s %-8lld %-5lld %10.1f %9.2f %9.2f %8lld %8lld\n",
            id, text_of(*job, "role").c_str(), static_cast<long long>(number_of(*job, "launcher")),
            static_cast<long long>(number_of(*job, "slots")), number_of(*job, "faults_per_s"),
            number_of(*job, "mb_in_per_s"), number_of(*job, "mb_out_per_s"),
            latency != nullptr ? static_cast<long long>(number_of(*latency, "p50_us")) : 0,
            latency != nullptr ? static_cast<long long>(number_of(*latency, "p99_us")) : 0));
        out += line.data();
        out += ownership(port, id, json);
    }
    return out;
}

}  // namespace

int top(std::uint16_t port) {
    Result<Platform> platform =
        platform_open({}, PlatformOptions{kNoNode, JobId{0}, 2, LogLevel::kError});
    if (!platform.ok()) {
        static_cast<void>(std::fputs("pm: cannot start\n", stderr));
        return 1;
    }
    g_quit = 0;
    struct sigaction quit {};
    quit.sa_handler = on_quit;
    sigemptyset(&quit.sa_mask);
    sigaction(SIGINT, &quit, nullptr);
    sigaction(SIGTERM, &quit, nullptr);
    static_cast<void>(std::fputs("\x1b[?1049h\x1b[?25l", stdout));  // own screen, no cursor
    while (g_quit == 0) {
        const std::string screen = frame(port, *platform.value().json);
        static_cast<void>(std::fputs(screen.c_str(), stdout));
        static_cast<void>(std::fflush(stdout));
        for (int tenths = 0; tenths < 10 && g_quit == 0; tenths++) {
            ::usleep(100000);
        }
    }
    static_cast<void>(std::fputs("\x1b[?25h\x1b[?1049l", stdout));  // the terminal as it was
    static_cast<void>(std::fflush(stdout));
    return 0;
}

}  // namespace paramesh
