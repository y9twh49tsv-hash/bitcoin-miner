#include "api/server.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define AZD_INVALID_SOCKET INVALID_SOCKET
#define AZD_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define AZD_INVALID_SOCKET (-1)
#define AZD_CLOSE_SOCKET ::close
#endif

#include "util/json.hpp"
#include "util/logging.hpp"

namespace azd::api {
namespace {

constexpr const char* kComponent = "api";

struct SocketSubsystem {
    SocketSubsystem() {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
    ~SocketSubsystem() {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};

void ensureSocketSubsystem() { static SocketSubsystem subsystem; }

std::string statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

std::string contentTypeForPath(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const std::string ext = path.substr(dot);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".png") return "image/png";
    return "application/octet-stream";
}

std::string trim(const std::string& in) {
    size_t begin = 0;
    size_t end = in.size();
    while (begin < end && (in[begin] == ' ' || in[begin] == '\t')) ++begin;
    while (end > begin && (in[end - 1] == ' ' || in[end - 1] == '\t' || in[end - 1] == '\r')) --end;
    return in.substr(begin, end - begin);
}

std::string toLower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

constexpr size_t kMaxRequestBytes = 256 * 1024;

} // namespace

HttpResponse HttpResponse::json(int status, const std::string& body) {
    HttpResponse response;
    response.status = status;
    response.contentType = "application/json";
    response.body = body;
    return response;
}

HttpResponse HttpResponse::error(int status, const std::string& message) {
    util::Json j = util::Json::object();
    j.set("error", message);
    return json(status, j.dump());
}

bool parseHttpRequest(const std::string& raw, HttpRequest& out) {
    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;

    std::istringstream stream(raw.substr(0, headerEnd));
    std::string requestLine;
    if (!std::getline(stream, requestLine)) return false;
    if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();

    std::istringstream lineStream(requestLine);
    std::string target;
    std::string version;
    if (!(lineStream >> out.method >> target >> version)) return false;

    const size_t questionMark = target.find('?');
    if (questionMark == std::string::npos) {
        out.path = target;
    } else {
        out.path = target.substr(0, questionMark);
        out.query = target.substr(questionMark + 1);
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        out.headers[toLower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }

    out.body = raw.substr(headerEnd + 4);
    return true;
}

bool isSafeStaticPath(const std::string& path) {
    if (path.empty() || path[0] != '/') return false;
    if (path.find("..") != std::string::npos) return false;
    if (path.find('\\') != std::string::npos) return false;
    if (path.find('\0') != std::string::npos) return false;
    // Reject anything that looks like an absolute Windows path smuggled in.
    if (path.size() > 2 && path[2] == ':') return false;
    return true;
}

ApiServer::ApiServer(mining::MinerController& miner, std::string bindHost, uint16_t port,
                     std::string dashboardRoot)
    : miner_(miner),
      bindHost_(std::move(bindHost)),
      port_(port),
      dashboardRoot_(std::move(dashboardRoot)) {}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::start(std::string& error) {
    if (running_.load()) {
        error = "server already running";
        return false;
    }
    ensureSocketSubsystem();

    if (bindHost_ != "127.0.0.1" && bindHost_ != "localhost") {
        // Not a hard failure -- the user may genuinely want LAN access -- but it
        // must be loud, because there is no authentication.
        util::logWarn(kComponent,
                      "dashboard is bound to " + bindHost_ +
                          " which is NOT localhost. The API has no authentication; anyone who "
                          "can reach this address can start and stop the miner.");
    }

    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == AZD_INVALID_SOCKET) {
        error = "cannot create socket";
        return false;
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    if (bindHost_ == "0.0.0.0") {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (bindHost_ == "localhost" || bindHost_ == "127.0.0.1") {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (::inet_pton(AF_INET, bindHost_.c_str(), &address.sin_addr) != 1) {
        AZD_CLOSE_SOCKET(fd);
        error = "invalid bind address: " + bindHost_;
        return false;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        AZD_CLOSE_SOCKET(fd);
        error = "cannot bind " + bindHost_ + ":" + std::to_string(port_) +
                " (is another instance running?)";
        return false;
    }
    if (::listen(fd, 16) != 0) {
        AZD_CLOSE_SOCKET(fd);
        error = "listen failed";
        return false;
    }

    sockaddr_in actual{};
#if defined(_WIN32)
    int actualLen = sizeof(actual);
#else
    socklen_t actualLen = sizeof(actual);
#endif
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actualLen) == 0) {
        boundPort_ = ntohs(actual.sin_port);
    } else {
        boundPort_ = port_;
    }

    listenSocket_.store(static_cast<std::intptr_t>(fd));
    running_.store(true);
    thread_ = std::thread([this] { acceptLoop(); });

    util::logInfo(kComponent, "dashboard listening on http://" + bindHost_ + ":" +
                                  std::to_string(boundPort_));
    return true;
}

void ApiServer::stop() {
    if (!running_.exchange(false)) return;

    const std::intptr_t fd = listenSocket_.exchange(-1);
    if (fd != -1) {
#if defined(_WIN32)
        ::shutdown(static_cast<socket_t>(fd), SD_BOTH);
#else
        ::shutdown(static_cast<socket_t>(fd), SHUT_RDWR);
#endif
        AZD_CLOSE_SOCKET(static_cast<socket_t>(fd));
    }
    if (thread_.joinable()) thread_.join();
}

void ApiServer::acceptLoop() {
    while (running_.load()) {
        const std::intptr_t stored = listenSocket_.load();
        if (stored == -1) break;

        sockaddr_in client{};
#if defined(_WIN32)
        int clientLen = sizeof(client);
#else
        socklen_t clientLen = sizeof(client);
#endif
        const socket_t fd = ::accept(static_cast<socket_t>(stored),
                                     reinterpret_cast<sockaddr*>(&client), &clientLen);
        if (fd == AZD_INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        // One request per connection keeps this simple and bounded.
        serveConnection(static_cast<std::intptr_t>(fd));
    }
}

void ApiServer::serveConnection(std::intptr_t clientSocket) {
    const socket_t fd = static_cast<socket_t>(clientSocket);

    std::string raw;
    char buffer[4096];
    size_t expectedBody = 0;
    bool headersComplete = false;

    while (true) {
#if defined(_WIN32)
        const int received = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
#endif
        if (received <= 0) break;
        raw.append(buffer, static_cast<size_t>(received));

        if (raw.size() > kMaxRequestBytes) {
            const HttpResponse response = HttpResponse::error(413, "request too large");
            std::string out = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: " +
                              std::to_string(response.body.size()) +
                              "\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" +
                              response.body;
            ::send(fd, out.data(), static_cast<int>(out.size()), 0);
            AZD_CLOSE_SOCKET(fd);
            return;
        }

        if (!headersComplete) {
            const size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd == std::string::npos) continue;
            headersComplete = true;

            HttpRequest probe;
            if (parseHttpRequest(raw, probe)) {
                const auto it = probe.headers.find("content-length");
                if (it != probe.headers.end()) {
                    expectedBody = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
                }
            }
            if (raw.size() - (headerEnd + 4) >= expectedBody) break;
        } else {
            const size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos && raw.size() - (headerEnd + 4) >= expectedBody) {
                break;
            }
        }
    }

    HttpRequest request;
    HttpResponse response;
    if (!parseHttpRequest(raw, request)) {
        response = HttpResponse::error(400, "malformed request");
    } else {
        response = handleRequest(request);
    }

    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                      statusText(response.status) + "\r\n";
    out += "Content-Type: " + response.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    // The dashboard is same-origin, so no CORS header is granted on purpose.
    out += "X-Content-Type-Options: nosniff\r\n";
    for (const auto& header : response.headers) {
        out += header.first + ": " + header.second + "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    out += response.body;

    size_t sent = 0;
    while (sent < out.size()) {
#if defined(_WIN32)
        const int n = ::send(fd, out.data() + sent, static_cast<int>(out.size() - sent), 0);
#else
        const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
#endif
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    AZD_CLOSE_SOCKET(fd);
}

HttpResponse ApiServer::serveStatic(const std::string& path) {
    if (dashboardRoot_.empty()) {
        return HttpResponse::error(404, "dashboard files are not configured");
    }
    if (!isSafeStaticPath(path)) {
        return HttpResponse::error(403, "forbidden path");
    }

    std::string relative = path == "/" ? "/index.html" : path;
    const std::string fullPath = dashboardRoot_ + relative;

    std::ifstream file(fullPath, std::ios::binary);
    if (!file) return HttpResponse::error(404, "not found");

    std::ostringstream contents;
    contents << file.rdbuf();

    HttpResponse response;
    response.status = 200;
    response.contentType = contentTypeForPath(relative);
    response.body = contents.str();
    return response;
}

HttpResponse ApiServer::handleRequest(const HttpRequest& request) {
    const std::string& path = request.path;
    const std::string& method = request.method;

    // --- Read-only endpoints. None of these ever contain a password. ---
    if (path == "/api/status") {
        if (method != "GET") return HttpResponse::error(405, "use GET");
        return HttpResponse::json(200, miner_.statusJson().dump());
    }
    if (path == "/api/gpu") {
        if (method != "GET") return HttpResponse::error(405, "use GET");
        return HttpResponse::json(200, miner_.gpuJson().dump());
    }
    if (path == "/api/pool") {
        if (method != "GET") return HttpResponse::error(405, "use GET");
        return HttpResponse::json(200, miner_.poolJson().dump());
    }
    if (path == "/api/stats") {
        if (method != "GET") return HttpResponse::error(405, "use GET");
        return HttpResponse::json(200, miner_.statistics().toJson().dump());
    }
    if (path == "/api/config") {
        if (method == "GET") {
            // includeSecrets = false: the password never leaves the process.
            return HttpResponse::json(200, miner_.config().toJson(false).dump());
        }
        if (method == "PUT") {
            util::Json patch;
            std::string parseError;
            if (!util::Json::parse(request.body, patch, &parseError)) {
                return HttpResponse::error(400, "invalid JSON: " + parseError);
            }
            std::string error;
            if (!miner_.updateConfig(patch, error)) {
                return HttpResponse::error(409, error);
            }
            util::Json result = util::Json::object();
            result.set("ok", true);
            result.set("config", miner_.config().toJson(false));
            return HttpResponse::json(200, result.dump());
        }
        return HttpResponse::error(405, "use GET or PUT");
    }

    // --- Control endpoints ---
    if (path == "/api/miner/start") {
        if (method != "POST") return HttpResponse::error(405, "use POST");
        std::string error;
        if (!miner_.start(error)) {
            return HttpResponse::error(409, error);
        }
        util::Json result = util::Json::object();
        result.set("ok", true);
        result.set("running", miner_.isRunning());
        result.set("backend", miner_.backendName());
        return HttpResponse::json(200, result.dump());
    }
    if (path == "/api/miner/stop") {
        if (method != "POST") return HttpResponse::error(405, "use POST");
        miner_.stop();
        util::Json result = util::Json::object();
        result.set("ok", true);
        result.set("running", miner_.isRunning());
        return HttpResponse::json(200, result.dump());
    }

    if (path.rfind("/api/", 0) == 0) {
        return HttpResponse::error(404, "unknown endpoint");
    }

    // --- Static dashboard files ---
    if (method != "GET") return HttpResponse::error(405, "use GET");
    return serveStatic(path);
}

} // namespace azd::api
