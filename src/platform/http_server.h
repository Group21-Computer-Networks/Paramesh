// A small HTTP server for the status API: GET, and POST for the dashboard's buttons.
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

// A streamed reply (server-sent events): `write` sends one piece at once and returns false
// once the client has gone. The handler runs on a server thread for as long as it streams.
using HttpWrite = std::function<bool(std::string_view piece)>;
using HttpStreamHandler = std::function<void(std::string_view path, const HttpWrite& write)>;

// Called on a server thread with a POST request's path and body.
using HttpPostHandler = std::function<HttpResponse(std::string_view path, std::string_view body)>;

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
    // The same for a reply streamed with `content_type`, until the handler returns.
    virtual void get_stream(const std::string& pattern, const std::string& content_type,
                            HttpStreamHandler handler) = 0;
    // Serves POST requests whose path matches `pattern`, but only from a page this server sent:
    // the Host header names this server (127.0.0.1 or localhost, its port), against DNS
    // rebinding, and the request carries "X-ParaMesh: 1", which a page of another site cannot
    // send without a CORS preflight, which is never answered. Anything else gets 403.
    virtual void post(const std::string& pattern, HttpPostHandler handler) = 0;
    // Binds and serves on a thread of its own. Port 0 picks a free port. Returns the port.
    virtual Result<std::uint16_t> start(const std::string& host, std::uint16_t port) = 0;
    // Stops serving and joins the thread. Also done by the destructor.
    virtual void stop() noexcept = 0;
};

}  // namespace paramesh

#endif  // PARAMESH_PLATFORM_HTTP_SERVER_H
