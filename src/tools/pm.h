// pm: talks to the local pmd. `pm status` prints the peers this node hears; `pm top` shows its
// jobs live; `pm leave` makes the node leave its jobs gracefully.
#ifndef PARAMESH_TOOLS_PM_H
#define PARAMESH_TOOLS_PM_H

#include "platform/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace paramesh {

// An HTTP/1.0 GET of `path` on 127.0.0.1:`port`. The body, if the status is 200.
Result<std::string> http_get(std::uint16_t port, std::string_view path);

// `pm top`: this node's jobs, redrawn once a second until Ctrl+C (src/tools/top.cpp).
int top(std::uint16_t port);

// `pm status|top [--http-port N]` or `pm leave [--force] [--state-dir DIRECTORY]`, without the
// program name. Returns the exit status for main().
int pm(std::span<const std::string_view> args);

}  // namespace paramesh

#endif  // PARAMESH_TOOLS_PM_H
