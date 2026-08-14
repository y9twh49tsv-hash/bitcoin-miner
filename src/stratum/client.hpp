#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mining/stats.hpp"
#include "mining/work.hpp"
#include "stratum/message.hpp"

namespace azd::stratum {

struct ClientConfig {
    std::string host;
    uint16_t port = 3333;
    std::string username;
    std::string password;
    std::string userAgent = "azd-bitcoin-miner/0.1";

    /// Reconnect backoff, in seconds. Doubles on each consecutive failure and
    /// is clamped to the maximum.
    double reconnectInitialDelay = 2.0;
    double reconnectMaxDelay = 60.0;

    /// Drop the connection if the pool sends nothing at all for this long.
    double readTimeoutSeconds = 300.0;
};

/// A share the miner wants to submit. Every field is a real result of real
/// hashing -- the client never manufactures one.
struct ShareSubmission {
    std::string jobId;
    std::string extranonce2Hex;
    uint32_t nTime = 0;
    uint32_t nonce = 0;

    /// Difficulty this share actually achieved, computed from its real hash.
    /// The exact integer is what "best difficulty" is ranked by; the double is
    /// only ever used for display.
    bitcoin::UInt256 achievedDifficultyExact;
    double achievedDifficulty = 0.0;
};

/// Stratum V1 client.
///
/// Owns one TCP connection and one background thread. The connection lifecycle
/// is: connect -> mining.subscribe -> mining.authorize -> receive jobs. Any
/// failure drops back to a reconnect with exponential backoff; job state is
/// discarded on disconnect so stale work is never mined.
///
/// The client NEVER fabricates pool activity. If it is not connected, the pool
/// state says disconnected and no jobs exist.
class StratumClient {
public:
    struct Callbacks {
        /// New job from mining.notify (with extranonce/difficulty filled in).
        std::function<void(const mining::MiningJob& job)> onJob;
        /// mining.set_difficulty
        std::function<void(double difficulty)> onDifficulty;
        /// Result of one of our mining.submit calls.
        std::function<void(const ShareSubmission& share, bool accepted,
                           const std::string& reason)>
            onSubmitResult;
        /// Connection state transitions.
        std::function<void(mining::PoolState state, const std::string& detail)> onStateChange;
    };

    StratumClient(ClientConfig config, Callbacks callbacks);
    ~StratumClient();

    StratumClient(const StratumClient&) = delete;
    StratumClient& operator=(const StratumClient&) = delete;

    /// Starts the background connection thread. Returns false if already running
    /// or if the configuration is incomplete (no host / no username).
    bool start();

    /// Stops the thread and closes the socket. Safe to call more than once.
    void stop();

    bool running() const { return running_.load(); }
    mining::PoolState state() const { return state_.load(); }

    /// Queue a share for submission. Returns false when not connected -- the
    /// share is then dropped and counted as stale by the caller, never as
    /// accepted.
    bool submitShare(const ShareSubmission& share);

    /// Description like "stratum+tcp://host:port" for display. Never includes
    /// credentials.
    std::string description() const;

    /// Number of currently unanswered mining.submit calls.
    size_t pendingSubmissions() const;

private:
    struct PendingSubmit {
        ShareSubmission share;
    };

    void threadMain();
    /// One full connection attempt. Returns when the connection ends.
    /// `connected` is set to true if the session got as far as authorizing.
    void runConnection(bool& reachedAuthorized);

    void setState(mining::PoolState state, const std::string& detail);
    bool sendLine(const std::string& line);
    void handleMessage(const Message& message);
    void handleResponse(const Message& message);
    void handleNotification(const Message& message);
    void flushOutboundQueue();

    int64_t nextId() { return nextId_++; }

    ClientConfig config_;
    Callbacks callbacks_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<mining::PoolState> state_{mining::PoolState::Disconnected};

    // Socket handle; -1 / INVALID_SOCKET when closed. Stored as intptr_t so the
    // header does not need to include platform socket headers.
    std::atomic<std::intptr_t> socket_{-1};

    LineBuffer lineBuffer_;

    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::deque<ShareSubmission> outbound_;
    std::map<int64_t, PendingSubmit> pendingSubmits_;

    int64_t subscribeId_ = 0;
    int64_t authorizeId_ = 0;
    std::atomic<int64_t> nextId_{1};

    // Session state, reset on every reconnect.
    std::vector<uint8_t> extranonce1_;
    size_t extranonce2Size_ = 0;
    double currentDifficulty_ = 0.0;
    bool authorized_ = false;
};

} // namespace azd::stratum
