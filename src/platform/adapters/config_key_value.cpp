#include "platform/adapters/adapters.h"

#include <string>
#include <string_view>

namespace paramesh {
namespace {

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

class KeyValueConfigLoader final : public ConfigLoader {
public:
    [[nodiscard]] Result<ConfigMap> parse(std::string_view text) const override {
        ConfigMap out;
        while (!text.empty()) {
            const auto end = text.find('\n');
            const std::string_view line = trim(text.substr(0, end));
            text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
            if (line.empty() || line.front() == '#') {
                continue;
            }
            const auto equals = line.find('=');
            const std::string_view key = trim(line.substr(0, equals));
            if (equals == std::string_view::npos || key.empty()) {
                return Error{Errc::kInvalidArgument, 0,
                             "parse configuration: a line is not 'key = value'"};
            }
            out.insert_or_assign(std::string{key}, std::string{trim(line.substr(equals + 1))});
        }
        return out;
    }
};

}  // namespace

std::unique_ptr<ConfigLoader> make_key_value_config_loader() {
    return std::make_unique<KeyValueConfigLoader>();
}

}  // namespace paramesh
