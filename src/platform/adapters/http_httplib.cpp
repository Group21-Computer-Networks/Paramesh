#include "platform/adapters/adapters.h"

#include <httplib.h>

#include <string>
#include <thread>
#include <utility>

namespace paramesh {
namespace {

class HttplibServer final : public HttpServer {
public:
    HttplibServer() = default;
    HttplibServer(const HttplibServer&) = delete;
    HttplibServer& operator=(const HttplibServer&) = delete;
    HttplibServer(HttplibServer&&) = delete;
    HttplibServer& operator=(HttplibServer&&) = delete;
    ~HttplibServer() override { stop(); }

    void get(const std::string& pattern, HttpHandler handler) override {
        server_.Get(pattern, [handler = std::move(handler)](const httplib::Request& request,
                                                            httplib::Response& response) {
            const HttpResponse reply = handler(request.path);
            response.status = reply.status;
            response.set_content(reply.body, reply.content_type);
        });
    }

    void get_stream(const std::string& pattern, const std::string& content_type,
                    HttpStreamHandler handler) override {
        server_.Get(pattern, [content_type, handler = std::move(handler)](
                                 const httplib::Request& request, httplib::Response& response) {
            response.set_chunked_content_provider(
                content_type,
                [handler, path = request.path](std::size_t /*offset*/, httplib::DataSink& sink) {
                    handler(path, [&sink](std::string_view piece) {
                        return sink.is_writable() && sink.write(piece.data(), piece.size());
                    });
                    sink.done();
                    return true;
                });
        });
    }

    Result<std::uint16_t> start(const std::string& host, std::uint16_t port) override {
        int bound = -1;
        if (port == 0) {
            bound = server_.bind_to_any_port(host);
        } else if (server_.bind_to_port(host, port)) {
            bound = port;
        }
        if (bound <= 0) {
            return Error{Errc::kIo, 0, "start the HTTP server: cannot bind the address"};
        }
        thread_ = std::thread{[this] { server_.listen_after_bind(); }};
        server_.wait_until_ready();
        return static_cast<std::uint16_t>(bound);
    }

    void stop() noexcept override {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    httplib::Server server_;
    std::thread thread_;
};

}  // namespace

std::unique_ptr<HttpServer> make_httplib_server(const Platform& /*made*/,
                                                const PlatformOptions& /*options*/) {
    return std::make_unique<HttplibServer>();
}

}  // namespace paramesh
