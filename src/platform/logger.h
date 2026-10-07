// Logging: one JSON object per line with time, node, job, level, event and fields.
#ifndef PARAMESH_PLATFORM_LOGGER_H
#define PARAMESH_PLATFORM_LOGGER_H

#include "platform/json.h"

#include <cstdint>
#include <string_view>

namespace paramesh {

enum class LogLevel : std::uint8_t { kDebug, kInfo, kWarn, kError };

class Logger {
public:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;
    virtual ~Logger() = default;

    // May be called from any thread.
    virtual void log(LogLevel level, std::string_view event, const JsonObject& fields) = 0;
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_LOGGER_H
