// A small HTTP server for the status API (GET only).
#ifndef PARAMESH_PLATFORM_HTTP_SERVER_H
#define PARAMESH_PLATFORM_HTTP_SERVER_H

#include "platform/result.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace paramesh {

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

// Called on a server thread with the request's path.
using HttpHandler = std::function<HttpResponse(std::string_view path)>;

class HttpServer {
public:
    HttpServer() = default;
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;
    virtual ~HttpServer() = default;

    // Serves GET requests whose path matches `pattern`, a regular expression. Call before
    // start().
    virtual void get(const std::string& pattern, HttpHandler handler) = 0;
    // Binds and serves on a thread of its own. Port 0 picks a free port. Returns the port.
    virtual Result<std::uint16_t> start(const std::string& host, std::uint16_t port) = 0;
    // Stops serving and joins the thread. Also done by the destructor.
    virtual void stop() noexcept = 0;
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_HTTP_SERVER_H
