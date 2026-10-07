// Private to src/platform/: the built-in adapters' makers, for factory.cpp.
#ifndef PARAMESH_PLATFORM_ADAPTERS_ADAPTERS_H
#define PARAMESH_PLATFORM_ADAPTERS_ADAPTERS_H

#include "platform/factory.h"

#include <memory>

namespace paramesh {

std::unique_ptr<Clock> make_real_clock(const Platform& made, const PlatformOptions& options);
std::unique_ptr<Hasher> make_picosha2_hasher(const Platform& made, const PlatformOptions& options);
std::unique_ptr<Checksum> make_table_crc32c(const Platform& made, const PlatformOptions& options);
std::unique_ptr<JsonCodec> make_nlohmann_json(const Platform& made, const PlatformOptions& options);
std::unique_ptr<Logger> make_json_lines_logger(const Platform& made,
                                               const PlatformOptions& options);
std::unique_ptr<HttpServer> make_httplib_server(const Platform& made,
                                                const PlatformOptions& options);
std::unique_ptr<ConfigLoader> make_key_value_config_loader();

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_ADAPTERS_ADAPTERS_H
