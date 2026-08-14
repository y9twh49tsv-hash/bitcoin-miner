#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "bitcoin/uint256.hpp"
#include "util/json.hpp"

namespace azd::mining {

enum class MiningState { Stopped, Starting, Running, Stopping, Error };
enum class PoolState { Disconnected, Connecting, Connected, Subscribed, Authorized, Error };

const char* toString(MiningState state);
const char* toString(PoolState state);

/// Thread-safe mining statistics.
///
/// HARD RULE: every counter starts at zero and only ever moves because the
/// program really did the thing being counted. There is no seeding, no
/// smoothing toward an expected value, no placeholder. If a number cannot be
/// measured (GPU temperature without CUDA, for example) it is reported as
/// *unavailable* rather than as a number -- see `GpuTelemetry`.
class Statistics {
public:
    Statistics();

    /// Record real completed hashes. Called by whichever backend actually
    /// performed them.
    void addHashes(uint64_t count);

    void recordAcceptedShare(const bitcoin::UInt256& shareDifficulty,
                             double shareDifficultyApprox);
    void recordRejectedShare(const std::string& reason);
    void recordStaleShare();
    void recordBlockFound();

    void setMiningState(MiningState state);
    void setPoolState(PoolState state);
    void setPoolDescription(const std::string& description);
    void setCurrentJobId(const std::string& jobId);
    void setPoolDifficulty(double difficulty);

    /// Marks the start of a mining session (resets session-scoped counters).
    void beginSession();
    void endSession();

    MiningState miningState() const;
    PoolState poolState() const;

    uint64_t totalHashes() const { return totalHashes_.load(std::memory_order_relaxed); }
    uint64_t acceptedShares() const { return acceptedShares_.load(std::memory_order_relaxed); }
    uint64_t rejectedShares() const { return rejectedShares_.load(std::memory_order_relaxed); }
    uint64_t staleShares() const { return staleShares_.load(std::memory_order_relaxed); }
    uint64_t blocksFound() const { return blocksFound_.load(std::memory_order_relaxed); }

    /// Hashes per second measured over the trailing window. Returns 0 when
    /// there is not yet enough real data -- never an estimate.
    double hashrate10s() const;
    double hashrate60s() const;
    double sessionAverageHashrate() const;

    /// Seconds since the process started / since mining started.
    double uptimeSeconds() const;
    double sessionSeconds() const;

    /// Best (highest) share difficulty actually observed this session, as an
    /// exact integer plus a display double. Zero until a real share is found.
    bitcoin::UInt256 bestShareDifficulty() const;
    double bestShareDifficultyApprox() const;

    std::string lastRejectReason() const;
    std::string currentJobId() const;
    double poolDifficulty() const;
    std::string poolDescription() const;

    /// Snapshot for the API/dashboard. Contains only measured values.
    util::Json toJson() const;

private:
    using Clock = std::chrono::steady_clock;

    struct Sample {
        Clock::time_point time;
        uint64_t hashes;
    };

    double hashrateOverWindow(double seconds) const;
    void pruneSamplesLocked(Clock::time_point now);

    std::atomic<uint64_t> totalHashes_{0};
    std::atomic<uint64_t> acceptedShares_{0};
    std::atomic<uint64_t> rejectedShares_{0};
    std::atomic<uint64_t> staleShares_{0};
    std::atomic<uint64_t> blocksFound_{0};

    std::atomic<int> miningState_{static_cast<int>(MiningState::Stopped)};
    std::atomic<int> poolState_{static_cast<int>(PoolState::Disconnected)};

    mutable std::mutex mutex_;
    std::deque<Sample> samples_;          // trailing hash-rate window
    uint64_t sessionHashes_ = 0;
    Clock::time_point processStart_;
    Clock::time_point sessionStart_;
    bool sessionActive_ = false;

    bitcoin::UInt256 bestShareDifficulty_;
    double bestShareDifficultyApprox_ = 0.0;
    std::string lastRejectReason_;
    std::string currentJobId_;
    std::string poolDescription_;
    double poolDifficulty_ = 0.0;
};

/// GPU telemetry. Every field is explicitly "unknown" unless a real backend
/// filled it in. There is deliberately no default numeric value.
struct GpuTelemetry {
    bool available = false;          // is a GPU backend compiled AND present?
    std::string unavailableReason = "CUDA backend not compiled";

    std::string name;
    bool hasTemperature = false;
    double temperatureCelsius = 0.0;
    bool hasUtilization = false;
    double utilizationPercent = 0.0;
    bool hasPower = false;
    double powerWatts = 0.0;
    bool hasFanSpeed = false;
    double fanSpeedPercent = 0.0;
    bool hasMemory = false;
    uint64_t memoryTotalBytes = 0;

    /// Serializes unknown values as JSON null so the dashboard renders "N/A"
    /// instead of a fabricated number.
    util::Json toJson() const;
};

} // namespace azd::mining
