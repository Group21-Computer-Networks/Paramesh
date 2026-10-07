// The one place that turns configured names into implementations. Code elsewhere includes
// the interface headers and this one, never an adapter.
#ifndef PARAMESH_PLATFORM_FACTORY_H
#define PARAMESH_PLATFORM_FACTORY_H

#include "platform/checksum.h"
#include "platform/clock.h"
#include "platform/config.h"
#include "platform/hasher.h"
#include "platform/http_server.h"
#include "platform/ids.h"
#include "platform/json.h"
#include "platform/logger.h"
#include "platform/result.h"

#include <memory>
#include <string>

namespace paramesh {

// What adapters need that is not another capability.
struct PlatformOptions {
    NodeId node;
    JobId job;
    int log_fd = 2;  // where log lines go; stderr
    LogLevel log_level = LogLevel::kInfo;
};

// One of each capability. Later members may use earlier ones (the logger uses the clock and
// the JSON codec), so they are declared, built and destroyed in this order.
struct Platform {
    std::unique_ptr<Clock> clock;
    std::unique_ptr<Hasher> hasher;
    std::unique_ptr<Checksum> checksum;
    std::unique_ptr<JsonCodec> json;
    std::unique_ptr<Logger> logger;
    std::unique_ptr<HttpServer> http;
};

// Builds a capability. `made` holds the capabilities built before this one.
template <typename T>
using PlatformMaker = std::unique_ptr<T> (*)(const Platform& made, const PlatformOptions& options);

// Adds an implementation under a name. To replace a vendored library: write the adapter and
// add one such line (the built-in ones are in factory.cpp).
void platform_register(std::string name, PlatformMaker<Clock> maker);
void platform_register(std::string name, PlatformMaker<Hasher> maker);
void platform_register(std::string name, PlatformMaker<Checksum> maker);
void platform_register(std::string name, PlatformMaker<JsonCodec> maker);
void platform_register(std::string name, PlatformMaker<Logger> maker);
void platform_register(std::string name, PlatformMaker<HttpServer> maker);

// Picks each implementation by the name under its configuration key, or the default:
//   platform.clock = real          platform.json   = nlohmann
//   platform.hasher = picosha2     platform.logger = json-lines
//   platform.checksum = crc32c     platform.http   = httplib
// Errc::kNotFound if a configured name is not registered.
Result<Platform> platform_open(const ConfigMap& config, const PlatformOptions& options);

// The configuration loader is needed before there is a configuration, so it is not chosen by
// name.
std::unique_ptr<ConfigLoader> platform_config_loader();

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_FACTORY_H
