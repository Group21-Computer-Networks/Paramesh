#include "platform/adapters/adapters.h"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <string>

namespace paramesh {
namespace {

constexpr std::array<const char*, 4> kLevelNames = {"debug", "info", "warn", "error"};

class JsonLinesLogger final : public Logger {
public:
    JsonLinesLogger(const Clock& clock, const JsonCodec& json,
                    const PlatformOptions& options) noexcept
        : clock_(&clock), json_(&json), options_(options) {}

    void log(LogLevel level, std::string_view event, const JsonObject& fields) override {
        if (level < options_.log_level) {
            return;
        }
        JsonObject line;
        line.emplace("time", JsonValue{static_cast<std::int64_t>(clock_->wall().count())});
        line.emplace("node", JsonValue{std::int64_t{options_.node.value}});
        line.emplace("job", JsonValue{std::int64_t{options_.job.value}});
        line.emplace("level",
                     JsonValue{std::string{kLevelNames.at(static_cast<std::size_t>(level))}});
        line.emplace("event", JsonValue{std::string{event}});
        line.emplace("fields", JsonValue{fields});
        std::string text = json_->encode(JsonValue{std::move(line)});
        text.push_back('\n');
        // One write per line, so lines from different threads do not interleave.
        // ponytail: a short or failed write loses the line; buffer and retry if logs go to a slow
        // pipe.
        const ssize_t written = ::write(options_.log_fd, text.data(), text.size());
        static_cast<void>(written);
    }

private:
    const Clock* clock_;
    const JsonCodec* json_;
    PlatformOptions options_;
};

}  // namespace

std::unique_ptr<Logger> make_json_lines_logger(const Platform& made,
                                               const PlatformOptions& options) {
    return std::make_unique<JsonLinesLogger>(*made.clock, *made.json, options);
}

}  // namespace paramesh
