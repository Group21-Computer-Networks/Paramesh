// JSON as a plain value, and a codec to and from text. No third-party type appears here.
#ifndef PARAMESH_PLATFORM_JSON_H
#define PARAMESH_PLATFORM_JSON_H

#include "platform/result.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace paramesh {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, JsonArray, JsonObject> v;
    friend bool operator==(const JsonValue&, const JsonValue&) = default;
};

class JsonCodec {
public:
    JsonCodec() = default;
    JsonCodec(const JsonCodec&) = delete;
    JsonCodec& operator=(const JsonCodec&) = delete;
    JsonCodec(JsonCodec&&) = delete;
    JsonCodec& operator=(JsonCodec&&) = delete;
    virtual ~JsonCodec() = default;

    // One line, no trailing newline.
    [[nodiscard]] virtual std::string encode(const JsonValue& value) const = 0;
    // Errc::kInvalidArgument for text that is not JSON.
    [[nodiscard]] virtual Result<JsonValue> decode(std::string_view text) const = 0;
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_JSON_H
