#include "platform/adapters/adapters.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace paramesh {
namespace {

nlohmann::json to_library(const JsonValue& value) {
    struct Visitor {
        nlohmann::json operator()(std::nullptr_t) const { return nullptr; }
        nlohmann::json operator()(bool b) const { return b; }
        nlohmann::json operator()(std::int64_t i) const { return i; }
        nlohmann::json operator()(double d) const { return d; }
        nlohmann::json operator()(const std::string& s) const { return s; }
        nlohmann::json operator()(const JsonArray& array) const {
            nlohmann::json out = nlohmann::json::array();
            for (const JsonValue& item : array) {
                out.push_back(to_library(item));
            }
            return out;
        }
        nlohmann::json operator()(const JsonObject& object) const {
            nlohmann::json out = nlohmann::json::object();
            for (const auto& [key, item] : object) {
                out[key] = to_library(item);
            }
            return out;
        }
    };
    return std::visit(Visitor{}, value.v);
}

JsonValue from_library(const nlohmann::json& j) {
    if (j.is_boolean()) {
        return {j.get<bool>()};
    }
    if (j.is_number_integer()) {
        // ponytail: an unsigned value above INT64_MAX wraps; add a uint64 alternative if one
        // appears.
        return {j.get<std::int64_t>()};
    }
    if (j.is_number_float()) {
        return {j.get<double>()};
    }
    if (j.is_string()) {
        return {j.get<std::string>()};
    }
    if (j.is_array()) {
        JsonArray out;
        for (const nlohmann::json& item : j) {
            out.push_back(from_library(item));
        }
        return {std::move(out)};
    }
    if (j.is_object()) {
        JsonObject out;
        for (const auto& [key, item] : j.items()) {
            out.emplace(key, from_library(item));
        }
        return {std::move(out)};
    }
    return {nullptr};
}

class NlohmannJson final : public JsonCodec {
public:
    [[nodiscard]] std::string encode(const JsonValue& value) const override {
        return to_library(value).dump();
    }
    [[nodiscard]] Result<JsonValue> decode(std::string_view text) const override {
        const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_discarded()) {
            return Error{Errc::kInvalidArgument, 0, "decode JSON: the text is not valid JSON"};
        }
        return from_library(parsed);
    }
};

}  // namespace

std::unique_ptr<JsonCodec> make_nlohmann_json(const Platform& /*made*/,
                                              const PlatformOptions& /*options*/) {
    return std::make_unique<NlohmannJson>();
}

}  // namespace paramesh
