// src/platform/: published test vectors for CRC-32C and SHA-256, each adapter through its
// interface, and the factory choosing an implementation by configured name.

#include "platform/factory.h"
#include "platform/sandbox.h"

#include <doctest/doctest.h>
#include <httplib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using paramesh::Errc;
using paramesh::JsonArray;
using paramesh::JsonObject;
using paramesh::JsonValue;

paramesh::Platform open_platform(const paramesh::ConfigMap& config = {},
                                 const paramesh::PlatformOptions& options = {}) {
    auto opened = paramesh::platform_open(config, options);
    REQUIRE_MESSAGE(opened.ok(), opened.error().what);
    return std::move(opened).value();
}

std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::string hex(const paramesh::Sha256& digest) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (const std::byte b : digest) {
        out.push_back(kDigits.at(std::to_integer<std::size_t>(b) >> 4U));
        out.push_back(kDigits.at(std::to_integer<std::size_t>(b) & 0xFU));
    }
    return out;
}

// A second hasher, to show the factory swaps implementations by name.
class DummyHasher final : public paramesh::Hasher {
public:
    [[nodiscard]] paramesh::Sha256 sha256(std::span<const std::byte> /*data*/) const override {
        paramesh::Sha256 out{};
        out.fill(std::byte{0xEE});
        return out;
    }
};

// The calling code: the same for every implementation.
std::string hash_of_abc(const paramesh::Platform& platform) {
    return hex(platform.hasher->sha256(bytes("abc")));
}

}  // namespace

TEST_CASE("CRC-32C matches the vectors of RFC 3720, appendix B.4, and the check value") {
    const paramesh::Platform platform = open_platform();
    std::array<std::byte, 32> data{};

    CHECK(platform.checksum->crc32c(data) == 0x8A9136AAU);  // 32 bytes of zeros
    data.fill(std::byte{0xFF});
    CHECK(platform.checksum->crc32c(data) == 0x62A8AB43U);  // 32 bytes of ones
    for (std::size_t i = 0; i < data.size(); i++) {
        data.at(i) = static_cast<std::byte>(i);
    }
    CHECK(platform.checksum->crc32c(data) == 0x46DD794EU);  // 0x00 up to 0x1F
    for (std::size_t i = 0; i < data.size(); i++) {
        data.at(i) = static_cast<std::byte>(data.size() - 1 - i);
    }
    CHECK(platform.checksum->crc32c(data) == 0x113FDB5CU);  // 0x1F down to 0x00
    CHECK(platform.checksum->crc32c(bytes("123456789")) == 0xE3069283U);
    CHECK(platform.checksum->crc32c({}) == 0U);
}

TEST_CASE("SHA-256 matches the FIPS 180 example vectors") {
    const paramesh::Platform platform = open_platform();
    CHECK(hex(platform.hasher->sha256(bytes("abc"))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex(platform.hasher->sha256(bytes(""))) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(platform.hasher->sha256(
              bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("a second adapter is selected by configuration name with no change to calling code") {
    paramesh::platform_register(
        "dummy", [](const paramesh::Platform&, const paramesh::PlatformOptions&) {
            return std::unique_ptr<paramesh::Hasher>{std::make_unique<DummyHasher>()};
        });
    const auto loader = paramesh::platform_config_loader();

    const auto standard = loader->parse("# nothing chosen: the defaults\n");
    REQUIRE(standard.ok());
    CHECK(hash_of_abc(open_platform(standard.value())) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const auto swapped = loader->parse("platform.hasher = dummy\n");
    REQUIRE(swapped.ok());
    CHECK(hash_of_abc(open_platform(swapped.value())) == std::string(64, 'e'));

    const auto unknown = paramesh::platform_open({{"platform.hasher", "md5"}}, {});
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().code == Errc::kNotFound);
}

TEST_CASE("the configuration loader reads key = value lines and rejects anything else") {
    const auto loader = paramesh::platform_config_loader();
    const auto parsed = loader->parse(
        "# tunables\n\n  log.level = debug  \nledger.scale=3600\nlog.level = info\nempty =\n");
    REQUIRE(parsed.ok());
    CHECK(parsed.value() ==
          paramesh::ConfigMap{{"empty", ""}, {"ledger.scale", "3600"}, {"log.level", "info"}});

    CHECK(loader->parse("no equals sign\n").error().code == Errc::kInvalidArgument);
    CHECK(loader->parse("= value without a key\n").error().code == Errc::kInvalidArgument);
    CHECK(loader->parse("").value().empty());
}

TEST_CASE("paramesh.conf in the state directory becomes PARAMESH_CFG_ variables; set ones win") {
    std::array<char, 32> dir = {"/tmp/pm-settings-XXXXXX"};
    REQUIRE(::mkdtemp(dir.data()) != nullptr);
    const std::string state_dir = dir.data();
    // NOLINTBEGIN(concurrency-mt-unsafe): no other thread of this process is running
    CHECK(paramesh::platform_load_settings(state_dir).ok());  // no file: nothing to do
    ::setenv("PARAMESH_CFG_LOG_LEVEL", "error", 1);
    ::unsetenv("PARAMESH_CFG_TASK_PREFETCH");
    ::unsetenv("PARAMESH_CFG_THRASH_HOLD_MAX");
    std::ofstream{state_dir + "/paramesh.conf"}
        << "# tunables\ntask.prefetch = 3\nthrash.hold_max=32\nlog.level = debug\n";
    REQUIRE(paramesh::platform_load_settings(state_dir).ok());
    const auto set = [](const char* name) {
        const char* value = std::getenv(name);
        return std::string{value != nullptr ? value : "(unset)"};
    };
    CHECK(set("PARAMESH_CFG_TASK_PREFETCH") == "3");
    CHECK(set("PARAMESH_CFG_THRASH_HOLD_MAX") == "32");
    CHECK(set("PARAMESH_CFG_LOG_LEVEL") == "error");  // already set
    std::ofstream{state_dir + "/paramesh.conf"} << "no equals sign\n";
    CHECK(paramesh::platform_load_settings(state_dir).error().code == Errc::kInvalidArgument);
    ::unsetenv("PARAMESH_CFG_LOG_LEVEL");
    ::unsetenv("PARAMESH_CFG_TASK_PREFETCH");
    ::unsetenv("PARAMESH_CFG_THRASH_HOLD_MAX");
    // NOLINTEND(concurrency-mt-unsafe)
    std::filesystem::remove_all(state_dir);
}

TEST_CASE("the JSON codec round-trips a nested value and rejects text that is not JSON") {
    const paramesh::Platform platform = open_platform();
    const JsonValue value{JsonObject{
        {"name", {std::string{"node \"7\"\n"}}},
        {"count", {std::int64_t{-42}}},
        {"ratio", {0.5}},
        {"up", {true}},
        {"none", {nullptr}},
        {"peers", {JsonArray{{std::int64_t{1}}, {JsonObject{{"id", {std::int64_t{2}}}}}}}},
    }};
    const std::string text = platform.json->encode(value);
    CHECK(text.find('\n') == std::string::npos);
    const auto back = platform.json->decode(text);
    REQUIRE(back.ok());
    CHECK(back.value() == value);

    CHECK(platform.json->decode("{\"unterminated\": ").error().code == Errc::kInvalidArgument);
}

TEST_CASE("the logger writes one JSON object per line with the six required members") {
    std::array<int, 2> ends{};
    REQUIRE(::pipe(ends.data()) == 0);
    paramesh::PlatformOptions options;
    options.node = paramesh::NodeId{7};
    options.job = paramesh::JobId{42};
    options.log_fd = ends[1];
    const paramesh::Platform platform = open_platform({}, options);

    platform.logger->log(paramesh::LogLevel::kDebug, "below the level", {});
    platform.logger->log(paramesh::LogLevel::kWarn, "page.fetch", {{"page", {std::int64_t{9}}}});
    ::close(ends[1]);

    std::string text(4096, '\0');
    const ssize_t got = ::read(ends[0], text.data(), text.size());
    ::close(ends[0]);
    REQUIRE(got > 0);
    text.resize(static_cast<std::size_t>(got));
    REQUIRE(text.back() == '\n');
    CHECK(std::count(text.begin(), text.end(), '\n') == 1);  // the debug line was filtered

    const auto line = platform.json->decode(text);
    REQUIRE(line.ok());
    const auto& object = std::get<JsonObject>(line.value().v);
    CHECK(object.size() == 6);
    CHECK(std::get<std::int64_t>(object.at("time").v) > 0);
    CHECK(object.at("node") == JsonValue{std::int64_t{7}});
    CHECK(object.at("job") == JsonValue{std::int64_t{42}});
    CHECK(object.at("level") == JsonValue{std::string{"warn"}});
    CHECK(object.at("event") == JsonValue{std::string{"page.fetch"}});
    CHECK(object.at("fields") == JsonValue{JsonObject{{"page", {std::int64_t{9}}}}});
}

TEST_CASE("the real clock moves forward and the fake clock moves only when told") {
    const paramesh::Platform platform = open_platform();
    const paramesh::Nanos before = platform.clock->now();
    CHECK(platform.clock->now() >= before);
    CHECK(platform.clock->wall() > std::chrono::hours{24 * 365 * 50});  // after 2020

    paramesh::FakeClock fake;
    CHECK(fake.now() == paramesh::Nanos{0});
    fake.advance(std::chrono::milliseconds{5});
    CHECK(fake.now() == std::chrono::milliseconds{5});
}

TEST_CASE("the HTTP server answers a registered GET and refuses an unknown path") {
    const paramesh::Platform platform = open_platform();
    platform.http->get("/v1/ping", [](std::string_view path) {
        return paramesh::HttpResponse{200, "application/json",
                                      R"({"path":")" + std::string{path} + R"("})"};
    });
    const auto port = platform.http->start("127.0.0.1", 0);
    REQUIRE(port.ok());

    httplib::Client client{"127.0.0.1", port.value()};
    const auto found = client.Get("/v1/ping");
    REQUIRE(found);
    CHECK(found->status == 200);
    CHECK(found->body == R"({"path":"/v1/ping"})");
    CHECK(found->get_header_value("Content-Type") == "application/json");

    const auto missing = client.Get("/v1/nothing");
    REQUIRE(missing);
    CHECK(missing->status == 404);
    platform.http->stop();
}

TEST_CASE("the HTTP server takes a POST only from its own page: the header and the Host") {
    const paramesh::Platform platform = open_platform();
    platform.http->post("/v1/echo", [](std::string_view path, std::string_view body) {
        return paramesh::HttpResponse{200, "text/plain",
                                      std::string{path} + " " + std::string{body}};
    });
    const auto port = platform.http->start("127.0.0.1", 0);
    REQUIRE(port.ok());
    httplib::Client client{"127.0.0.1", port.value()};

    const auto ours = client.Post("/v1/echo", {{"X-ParaMesh", "1"}}, "{}", "application/json");
    REQUIRE(ours);
    CHECK(ours->status == 200);
    CHECK(ours->body == "/v1/echo {}");
    const auto by_name = client.Post(
        "/v1/echo", {{"X-ParaMesh", "1"}, {"Host", "localhost:" + std::to_string(port.value())}},
        "{}", "application/json");
    REQUIRE(by_name);
    CHECK(by_name->status == 200);

    // A form on another site cannot set the header; a rebound name gives another Host.
    const auto no_header = client.Post("/v1/echo", "{}", "application/json");
    REQUIRE(no_header);
    CHECK(no_header->status == 403);
    const auto rebound = client.Post("/v1/echo", {{"X-ParaMesh", "1"}, {"Host", "evil.example:80"}},
                                     "{}", "application/json");
    REQUIRE(rebound);
    CHECK(rebound->status == 403);
    CHECK(client.Get("/v1/echo")->status == 404);  // POST only
    platform.http->stop();
}

// ---- the playground's sandbox (X-playground) ----------------------------------------------------

namespace {

// What `probe` returns in a forked child confined as a job.
template <typename Probe>
int in_sandbox(const std::filesystem::path& directory, Probe probe) {
    const pid_t child = ::fork();
    if (child == 0) {
        if (!paramesh::sandbox_enter(directory.c_str(), paramesh::SandboxProfile::kJob)) {
            ::_exit(99);
        }
        ::_exit(probe());
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 100;
}

bool tcp_refused_by_sandbox(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    return ::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0 && errno == EACCES;
}

bool can_sandbox() {
    return ::syscall(SYS_landlock_create_ruleset, nullptr, 0, 1) >= 4 &&
           std::string_view{PARAMESH_SANITIZER_NAME}.empty();
}

}  // namespace

TEST_CASE("a sandboxed job cannot leave its directory, start a process or reach a low port" *
          doctest::skip(!can_sandbox())) {
    const auto directory =
        std::filesystem::temp_directory_path() / ("pm-sandbox." + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    const auto inside = directory / "file";
    CHECK(in_sandbox(directory, [&] { return std::ofstream{inside}.good() ? 0 : 1; }) == 0);
    CHECK(in_sandbox(directory, [] { return std::ifstream{"/etc/passwd"}.good() ? 1 : 0; }) == 0);
    CHECK(in_sandbox(directory,
                     [] { return std::ofstream{"/tmp/pm-sandbox-escape"}.good() ? 1 : 0; }) == 0);
    CHECK(in_sandbox(directory, [] { return ::fork() < 0 && errno == EPERM ? 0 : 1; }) == 0);
    CHECK(in_sandbox(directory, [] {
              std::array<char*, 1> argv{};
              return ::execv("/bin/true", argv.data()) != 0 ? 0 : 1;
          }) == 0);
    CHECK(in_sandbox(directory, [] { return ::kill(1, 0) != 0 && errno == EPERM ? 0 : 1; }) == 0);
    CHECK(in_sandbox(directory, [] { return ::socket(AF_INET, SOCK_DGRAM, 0) < 0 ? 0 : 1; }) == 0);
    CHECK(in_sandbox(directory, [] { return tcp_refused_by_sandbox(22) ? 0 : 1; }) == 0);
    CHECK(in_sandbox(directory, [] { return tcp_refused_by_sandbox(47002) ? 0 : 1; }) == 0);
    CHECK(in_sandbox(directory, [] {  // threads, which the runtime needs, still work
              std::atomic<int> ran{0};
              std::thread{[&ran] { ran = 1; }}.join();
              return ran == 1 ? 0 : 1;
          }) == 0);
    std::filesystem::remove_all(directory);
}
