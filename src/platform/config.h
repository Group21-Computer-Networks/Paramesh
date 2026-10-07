// The key=value configuration file.
#ifndef PARAMESH_PLATFORM_CONFIG_H
#define PARAMESH_PLATFORM_CONFIG_H

#include "platform/result.h"

#include <map>
#include <string>
#include <string_view>

namespace paramesh {

using ConfigMap = std::map<std::string, std::string, std::less<>>;

class ConfigLoader {
public:
    ConfigLoader() = default;
    ConfigLoader(const ConfigLoader&) = delete;
    ConfigLoader& operator=(const ConfigLoader&) = delete;
    ConfigLoader(ConfigLoader&&) = delete;
    ConfigLoader& operator=(ConfigLoader&&) = delete;
    virtual ~ConfigLoader() = default;

    // One "key = value" per line; blank lines and lines starting with # are skipped; spaces
    // around key and value are dropped; a later key replaces an earlier one.
    // Errc::kInvalidArgument for a line with no '=' or an empty key.
    [[nodiscard]] virtual Result<ConfigMap> parse(std::string_view text) const = 0;
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_CONFIG_H
