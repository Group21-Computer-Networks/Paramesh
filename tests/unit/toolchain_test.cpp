// Checks on the build itself: the language standard, the vendored headers, and that the
// sanitizer CMake was asked for is really compiled in.

#include <doctest/doctest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <string>
#include <string_view>

namespace {

// GCC defines __SANITIZE_*__; Clang answers __has_feature().
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAsanOn = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAsanOn = true;
#else
constexpr bool kAsanOn = false;
#endif
#else
constexpr bool kAsanOn = false;
#endif

#if defined(__SANITIZE_THREAD__)
constexpr bool kTsanOn = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kTsanOn = true;
#else
constexpr bool kTsanOn = false;
#endif
#else
constexpr bool kTsanOn = false;
#endif

// Set by tests/unit/CMakeLists.txt from PARAMESH_SANITIZER; empty when there is none.
constexpr const char* kSanitizer = PARAMESH_SANITIZER_NAME;

}  // namespace

TEST_CASE("compiled as C++20") {
    CHECK(__cplusplus >= 202002L);
}

TEST_CASE("the requested sanitizer is compiled in") {
    // UBSan has no feature macro on GCC, so only address and thread are checked directly.
    const std::string_view requested{kSanitizer};
    CHECK((requested == "address") == kAsanOn);
    CHECK((requested == "thread") == kTsanOn);
}

TEST_CASE("nlohmann/json round-trips a document") {
    const auto doc = nlohmann::json::parse(R"({"node":7,"event":"hello","ok":true})");
    CHECK(doc.at("node").get<int>() == 7);
    CHECK(nlohmann::json::parse(doc.dump()) == doc);
}

TEST_CASE("PicoSHA2 hashes the empty string") {
    const std::string empty;
    CHECK(picosha2::hash256_hex_string(empty) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("cpp-httplib server can be constructed and stopped without listening") {
    httplib::Server server;
    server.Get("/v1/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });
    CHECK_FALSE(server.is_running());
    server.stop();
}
