#include "stratum/client.hpp"

#include <chrono>
#include <cstring>

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
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
#define AZD_INVALID_SOCKET (-1)
#define AZD_CLOSE_SOCKET ::close
#endif

#include "util/hex.hpp"
#include "util/logging.hpp"

namespace azd::stratum {
namespace {

constexpr const char* kComponent = "stratum";

/// One-time Winsock initialization. A no-op elsewhere.
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

std::string lastSocketError() {
#if defined(_WIN32)
    return "winsock error " + std::to_string(WSAGetLastError());
#else
    return std::string(std::strerror(errno));
#endif
}

socket_t connectTo(const std::string& host, uint16_t port, std::string& error) {
    ensureSocketSubsystem();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const std::string portText = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), portText.c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
        error = "cannot resolve host";
        return AZD_INVALID_SOCKET;
    }

    socket_t fd = AZD_INVALID_SOCKET;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd == AZD_INVALID_SOCKET) continue;

        if (::connect(fd, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
            break;
        }
        AZD_CLOSE_SOCKET(fd);
        fd = AZD_INVALID_SOCKET;
    }
    freeaddrinfo(results);

    if (fd == AZD_INVALID_SOCKET) {
        error = "connection failed: " + lastSocketError();
        return AZD_INVALID_SOCKET;
    }

    // Stratum is latency sensitive and messages are tiny.
    int flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag),
                 sizeof(flag));
    return fd;
}

/// Wait until the socket is readable. Returns 1 readable, 0 timeout, -1 error.
int waitReadable(socket_t fd, double seconds) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);

    timeval tv{};
    tv.tv_sec = static_cast<long>(seconds);
    tv.tv_usec = static_cast<long>((seconds - static_cast<double>(tv.tv_sec)) * 1e6);

#if defined(_WIN32)
    const int rc = ::select(0, &readSet, nullptr, nullptr, &tv);
#else
    const int rc = ::select(static_cast<int>(fd) + 1, &readSet, nullptr, nullptr, &tv);
#endif
    if (rc < 0) return -1;
    return rc > 0 ? 1 : 0;
}

} // namespace

StratumClient::StratumClient(ClientConfig config, Callbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)) {}

StratumClient::~StratumClient() { stop(); }

std::string StratumClient::description() const {
    // Deliberately excludes username and password.
    return "stratum+tcp://" + config_.host + ":" + std::to_string(config_.port);
}

size_t StratumClient::pendingSubmissions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingSubmits_.size();
}

bool StratumClient::start() {
    if (running_.load()) return false;
    if (config_.host.empty() || config_.username.empty()) {
        setState(mining::PoolState::Error, "pool host and username must be configured");
        return false;
    }
    stopRequested_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void StratumClient::stop() {
    if (!running_.load() && !thread_.joinable()) return;

    stopRequested_.store(true);
    wakeup_.notify_all();

    const std::intptr_t fd = socket_.exchange(-1);
    if (fd != -1) {
#if defined(_WIN32)
        ::shutdown(static_cast<socket_t>(fd), SD_BOTH);
        AZD_CLOSE_SOCKET(static_cast<socket_t>(fd));
#else
        ::shutdown(static_cast<socket_t>(fd), SHUT_RDWR);
        AZD_CLOSE_SOCKET(static_cast<socket_t>(fd));
#endif
    }

    if (thread_.joinable()) thread_.join();
    running_.store(false);
    setState(mining::PoolState::Disconnected, "stopped");
}

void StratumClient::setState(mining::PoolState state, const std::string& detail) {
    state_.store(state);
    if (callbacks_.onStateChange) callbacks_.onStateChange(state, detail);
}

void StratumClient::threadMain() {
    double delay = config_.reconnectInitialDelay;

    while (!stopRequested_.load()) {
        bool reachedAuthorized = false;
        runConnection(reachedAuthorized);

        if (stopRequested_.load()) break;

        // A session that got as far as authorizing was healthy; reset backoff
        // so a transient drop reconnects promptly.
        if (reachedAuthorized) delay = config_.reconnectInitialDelay;

        setState(mining::PoolState::Disconnected,
                 "reconnecting in " + std::to_string(static_cast<int>(delay)) + "s");
        util::logInfo(kComponent, "reconnecting in " + std::to_string(static_cast<int>(delay)) +
                                      " second(s)");

        std::unique_lock<std::mutex> lock(mutex_);
        wakeup_.wait_for(lock, std::chrono::milliseconds(static_cast<int64_t>(delay * 1000.0)),
                         [this] { return stopRequested_.load(); });
        lock.unlock();

        delay = delay * 2.0;
        if (delay > config_.reconnectMaxDelay) delay = config_.reconnectMaxDelay;
    }

    running_.store(false);
}

void StratumClient::runConnection(bool& reachedAuthorized) {
    // Reset all per-session state. Nothing survives a reconnect: stale jobs are
    // worthless and stale extranonces are dangerous.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingSubmits_.clear();
        outbound_.clear();
    }
    lineBuffer_.clear();
    extranonce1_.clear();
    extranonce2Size_ = 0;
    currentDifficulty_ = 0.0;
    authorized_ = false;

    setState(mining::PoolState::Connecting, description());
    util::logInfo(kComponent, "connecting to " + description());

    std::string error;
    const socket_t fd = connectTo(config_.host, config_.port, error);
    if (fd == AZD_INVALID_SOCKET) {
        setState(mining::PoolState::Error, error);
        util::logWarn(kComponent, "connect failed: " + error);
        return;
    }
    socket_.store(static_cast<std::intptr_t>(fd));
    setState(mining::PoolState::Connected, description());

    subscribeId_ = nextId();
    if (!sendLine(buildSubscribe(subscribeId_, config_.userAgent))) {
        setState(mining::PoolState::Error, "failed to send mining.subscribe");
    } else {
        authorizeId_ = nextId();
        // The password goes on the wire because the protocol requires it; it is
        // never logged.
        if (!sendLine(buildAuthorize(authorizeId_, config_.username, config_.password))) {
            setState(mining::PoolState::Error, "failed to send mining.authorize");
        } else {
            util::logInfo(kComponent, "sent subscribe + authorize for worker " +
                                          config_.username + " (password " +
                                          util::redactSecret(config_.password) + ")");

            auto lastActivity = std::chrono::steady_clock::now();
            char buffer[8192];

            while (!stopRequested_.load()) {
                flushOutboundQueue();

                const int ready = waitReadable(fd, 0.25);
                if (ready < 0) break;
                if (ready == 0) {
                    const double idle = std::chrono::duration<double>(
                                            std::chrono::steady_clock::now() - lastActivity)
                                            .count();
                    if (idle > config_.readTimeoutSeconds) {
                        util::logWarn(kComponent, "pool idle timeout, dropping connection");
                        break;
                    }
                    continue;
                }

#if defined(_WIN32)
                const int received = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
                const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
#endif
                if (received == 0) {
                    util::logInfo(kComponent, "pool closed the connection");
                    break;
                }
                if (received < 0) {
#if !defined(_WIN32)
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
                    util::logWarn(kComponent, "recv failed: " + lastSocketError());
                    break;
                }

                lastActivity = std::chrono::steady_clock::now();
                lineBuffer_.append(buffer, static_cast<size_t>(received));
                if (lineBuffer_.overflowed()) {
                    util::logError(kComponent, "oversized message from pool, dropping connection");
                    break;
                }

                std::string line;
                while (lineBuffer_.nextLine(line)) {
                    const Message message = Message::parse(line);
                    if (!message.valid) {
                        util::logWarn(kComponent, "ignoring bad message: " + message.parseError);
                        continue;
                    }
                    handleMessage(message);
                    if (authorized_) reachedAuthorized = true;
                }
            }
        }
    }

    const std::intptr_t stored = socket_.exchange(-1);
    if (stored != -1) AZD_CLOSE_SOCKET(static_cast<socket_t>(stored));

    authorized_ = false;
    setState(mining::PoolState::Disconnected, "connection closed");
}

bool StratumClient::sendLine(const std::string& line) {
    const std::intptr_t stored = socket_.load();
    if (stored == -1) return false;
    const socket_t fd = static_cast<socket_t>(stored);

    size_t sent = 0;
    while (sent < line.size()) {
#if defined(_WIN32)
        const int n = ::send(fd, line.data() + sent, static_cast<int>(line.size() - sent), 0);
#else
        const ssize_t n = ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void StratumClient::flushOutboundQueue() {
    std::deque<ShareSubmission> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(outbound_);
    }

    for (const ShareSubmission& share : pending) {
        const int64_t id = nextId();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingSubmits_[id] = PendingSubmit{share};
        }
        const std::string line =
            buildSubmit(id, config_.username, share.jobId, share.extranonce2Hex,
                        toBigEndianHex32(share.nTime), toBigEndianHex32(share.nonce));
        if (!sendLine(line)) {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingSubmits_.erase(id);
            util::logWarn(kComponent, "failed to send share for job " + share.jobId);
        } else {
            util::logInfo(kComponent, "submitted share for job " + share.jobId + " nonce " +
                                          toBigEndianHex32(share.nonce));
        }
    }
}

bool StratumClient::submitShare(const ShareSubmission& share) {
    if (state_.load() != mining::PoolState::Authorized) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outbound_.push_back(share);
    }
    wakeup_.notify_all();
    return true;
}

void StratumClient::handleMessage(const Message& message) {
    if (message.isNotification()) {
        handleNotification(message);
        return;
    }
    handleResponse(message);
}

void StratumClient::handleResponse(const Message& message) {
    if (message.id == subscribeId_) {
        if (message.isError()) {
            setState(mining::PoolState::Error, "subscribe rejected: " + message.errorMessage());
            util::logError(kComponent, "mining.subscribe rejected: " + message.errorMessage());
            return;
        }
        const SubscribeResult result = parseSubscribeResult(message.result);
        if (!result.ok) {
            setState(mining::PoolState::Error, "bad subscribe result: " + result.error);
            util::logError(kComponent, "mining.subscribe: " + result.error);
            return;
        }
        extranonce1_ = result.extranonce1;
        extranonce2Size_ = result.extranonce2Size;
        setState(mining::PoolState::Subscribed, description());
        util::logInfo(kComponent, "subscribed: extranonce1=" + util::toHex(extranonce1_) +
                                      " extranonce2_size=" + std::to_string(extranonce2Size_));
        return;
    }

    if (message.id == authorizeId_) {
        const bool ok = !message.isError() && message.result.asBool(false);
        if (!ok) {
            const std::string reason =
                message.isError() ? message.errorMessage() : "pool returned false";
            setState(mining::PoolState::Error, "authorization failed: " + reason);
            util::logError(kComponent, "mining.authorize failed: " + reason);
            return;
        }
        authorized_ = true;
        setState(mining::PoolState::Authorized, description());
        util::logInfo(kComponent, "authorized as " + config_.username);
        return;
    }

    // Otherwise: a share submission response.
    PendingSubmit pending;
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pendingSubmits_.find(message.id);
        if (it != pendingSubmits_.end()) {
            pending = it->second;
            pendingSubmits_.erase(it);
            known = true;
        }
    }
    if (!known) return;

    const bool accepted = !message.isError() && message.result.asBool(false);
    const std::string reason =
        accepted ? std::string() : (message.isError() ? message.errorMessage()
                                                      : "pool returned false");

    if (accepted) {
        util::logInfo(kComponent, "share ACCEPTED for job " + pending.share.jobId);
    } else {
        util::logWarn(kComponent, "share REJECTED for job " + pending.share.jobId + ": " + reason);
    }

    if (callbacks_.onSubmitResult) callbacks_.onSubmitResult(pending.share, accepted, reason);
}

void StratumClient::handleNotification(const Message& message) {
    if (message.method == "mining.set_difficulty") {
        if (!message.params.isArray() || message.params.size() < 1) return;
        const double difficulty = message.params.at(0).asNumber(0.0);
        if (!(difficulty > 0.0)) {
            util::logWarn(kComponent, "ignoring non-positive difficulty from pool");
            return;
        }
        currentDifficulty_ = difficulty;
        util::logInfo(kComponent, "difficulty set to " + std::to_string(difficulty));
        if (callbacks_.onDifficulty) callbacks_.onDifficulty(difficulty);
        return;
    }

    if (message.method == "mining.set_extranonce") {
        if (!message.params.isArray() || message.params.size() < 2) return;
        std::vector<uint8_t> extranonce1;
        if (!util::fromHex(message.params.at(0).asString(), extranonce1)) return;
        const int64_t size = message.params.at(1).asInt(-1);
        if (size <= 0 || size > 32) return;
        extranonce1_ = extranonce1;
        extranonce2Size_ = static_cast<size_t>(size);
        util::logInfo(kComponent, "extranonce updated: " + util::toHex(extranonce1_));
        return;
    }

    if (message.method == "mining.notify") {
        NotifyResult notify = parseNotify(message.params);
        if (!notify.ok) {
            util::logWarn(kComponent, "bad mining.notify: " + notify.error);
            return;
        }
        if (extranonce2Size_ == 0) {
            util::logWarn(kComponent, "job received before subscribe completed, ignoring");
            return;
        }

        notify.job.extranonce1 = extranonce1_;
        notify.job.extranonce2Size = extranonce2Size_;
        notify.job.difficulty = currentDifficulty_;

        util::logInfo(kComponent,
                      "job " + notify.job.jobId + (notify.job.cleanJobs ? " (clean)" : "") +
                          " nbits=" + toBigEndianHex32(notify.job.nBits));

        if (callbacks_.onJob) callbacks_.onJob(notify.job);
        return;
    }

    if (message.method == "client.reconnect") {
        util::logInfo(kComponent, "pool requested reconnect");
        const std::intptr_t stored = socket_.load();
        if (stored != -1) {
#if defined(_WIN32)
            ::shutdown(static_cast<socket_t>(stored), SD_BOTH);
#else
            ::shutdown(static_cast<socket_t>(stored), SHUT_RDWR);
#endif
        }
        return;
    }

    if (message.method == "client.show_message") {
        if (message.params.isArray() && message.params.size() > 0) {
            util::logInfo(kComponent, "pool message: " + message.params.at(0).asString());
        }
        return;
    }

    util::logDebug(kComponent, "unhandled method: " + message.method);
}

} // namespace azd::stratum
