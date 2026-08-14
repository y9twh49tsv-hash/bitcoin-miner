#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>

#include "mining/miner.hpp"

namespace azd::api {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    /// Extra headers, e.g. Cache-Control.
    std::map<std::string, std::string> headers;

    static HttpResponse json(int status, const std::string& body);
    static HttpResponse error(int status, const std::string& message);
};

/// Minimal HTTP/1.1 server for the local dashboard.
///
/// SECURITY POSTURE
///  * Binds to 127.0.0.1 by default and refuses any other address unless the
///    caller explicitly opts in -- there is no authentication, so this must not
///    be exposed to a network.
///  * GET responses NEVER contain the pool password. `PUT /api/config` accepts
///    one, but it is write-only from the API's perspective.
///  * Static files are served only from the configured dashboard directory,
///    with path traversal rejected.
///
/// Implementation is a straightforward thread-per-connection loop: the
/// dashboard is a handful of requests per second from one browser, so this is
/// the right complexity level.
class ApiServer {
public:
    ApiServer(mining::MinerController& miner, std::string bindHost, uint16_t port,
              std::string dashboardRoot);
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    /// Binds and starts serving on a background thread.
    /// Returns false with `error` set when the port cannot be bound.
    bool start(std::string& error);
    void stop();

    bool running() const { return running_.load(); }

    /// The port actually bound (useful when 0 was requested).
    uint16_t boundPort() const { return boundPort_; }

    /// Exposed for testing: route a request without any socket involved.
    HttpResponse handleRequest(const HttpRequest& request);

private:
    void acceptLoop();
    void serveConnection(std::intptr_t clientSocket);

    HttpResponse serveStatic(const std::string& path);

    mining::MinerController& miner_;
    std::string bindHost_;
    uint16_t port_;
    uint16_t boundPort_ = 0;
    std::string dashboardRoot_;

    std::atomic<bool> running_{false};
    std::atomic<std::intptr_t> listenSocket_{-1};
    std::thread thread_;
};

/// Parses a raw HTTP request. Returns false when the request is incomplete or
/// malformed.
bool parseHttpRequest(const std::string& raw, HttpRequest& out);

/// True when `path` stays inside the dashboard root (no traversal, no absolute
/// paths, no backslashes).
bool isSafeStaticPath(const std::string& path);

} // namespace azd::api
