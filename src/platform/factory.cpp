#include "platform/factory.h"

#include "platform/adapters/adapters.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace paramesh {
namespace {

template <typename T>
using Makers = std::map<std::string, PlatformMaker<T>, std::less<>>;

// One registry per capability, each starting with the built-in adapter. Registration happens
// at start-up, before any thread uses the factory, so there is no lock.
Makers<Clock>& clocks() {
    static Makers<Clock> makers{{"real", &make_real_clock}};
    return makers;
}
Makers<Hasher>& hashers() {
    static Makers<Hasher> makers{{"picosha2", &make_picosha2_hasher}};
    return makers;
}
Makers<Checksum>& checksums() {
    static Makers<Checksum> makers{{"crc32c", &make_table_crc32c}};
    return makers;
}
Makers<JsonCodec>& json_codecs() {
    static Makers<JsonCodec> makers{{"nlohmann", &make_nlohmann_json}};
    return makers;
}
Makers<Logger>& loggers() {
    static Makers<Logger> makers{{"json-lines", &make_json_lines_logger}};
    return makers;
}
Makers<HttpServer>& http_servers() {
    static Makers<HttpServer> makers{{"httplib", &make_httplib_server}};
    return makers;
}

// Fills `slot` with the implementation named under `key`, or under `fallback` if the key is
// not configured. False if no implementation has that name.
template <typename T>
bool make(std::unique_ptr<T>& slot, const Makers<T>& makers, const ConfigMap& config,
          std::string_view key, std::string_view fallback, const Platform& made,
          const PlatformOptions& options) {
    const auto configured = config.find(key);
    const auto maker =
        makers.find(configured == config.end() ? fallback : std::string_view{configured->second});
    if (maker == makers.end()) {
        return false;
    }
    slot = maker->second(made, options);
    return true;
}

}  // namespace

void platform_register(std::string name, PlatformMaker<Clock> maker) {
    clocks().insert_or_assign(std::move(name), maker);
}
void platform_register(std::string name, PlatformMaker<Hasher> maker) {
    hashers().insert_or_assign(std::move(name), maker);
}
void platform_register(std::string name, PlatformMaker<Checksum> maker) {
    checksums().insert_or_assign(std::move(name), maker);
}
void platform_register(std::string name, PlatformMaker<JsonCodec> maker) {
    json_codecs().insert_or_assign(std::move(name), maker);
}
void platform_register(std::string name, PlatformMaker<Logger> maker) {
    loggers().insert_or_assign(std::move(name), maker);
}
void platform_register(std::string name, PlatformMaker<HttpServer> maker) {
    http_servers().insert_or_assign(std::move(name), maker);
}

Result<Platform> platform_open(const ConfigMap& config, const PlatformOptions& options) {
    Platform p;
    const bool found =
        make(p.clock, clocks(), config, "platform.clock", "real", p, options) &&
        make(p.hasher, hashers(), config, "platform.hasher", "picosha2", p, options) &&
        make(p.checksum, checksums(), config, "platform.checksum", "crc32c", p, options) &&
        make(p.json, json_codecs(), config, "platform.json", "nlohmann", p, options) &&
        make(p.logger, loggers(), config, "platform.logger", "json-lines", p, options) &&
        make(p.http, http_servers(), config, "platform.http", "httplib", p, options);
    if (!found) {
        return Error{Errc::kNotFound, 0,
                     "open the platform: a configured platform.* name is not registered"};
    }
    return p;
}

std::unique_ptr<ConfigLoader> platform_config_loader() {
    return make_key_value_config_loader();
}

}  // namespace paramesh
