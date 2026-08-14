#include "mining/stats.hpp"

#include <algorithm>

namespace azd::mining {

const char* toString(MiningState state) {
    switch (state) {
        case MiningState::Stopped: return "stopped";
        case MiningState::Starting: return "starting";
        case MiningState::Running: return "running";
        case MiningState::Stopping: return "stopping";
        case MiningState::Error: return "error";
    }
    return "unknown";
}

const char* toString(PoolState state) {
    switch (state) {
        case PoolState::Disconnected: return "disconnected";
        case PoolState::Connecting: return "connecting";
        case PoolState::Connected: return "connected";
        case PoolState::Subscribed: return "subscribed";
        case PoolState::Authorized: return "authorized";
        case PoolState::Error: return "error";
    }
    return "unknown";
}

Statistics::Statistics() {
    processStart_ = Clock::now();
    sessionStart_ = processStart_;
}

void Statistics::addHashes(uint64_t count) {
    if (count == 0) return;
    totalHashes_.fetch_add(count, std::memory_order_relaxed);

    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    sessionHashes_ += count;
    samples_.push_back(Sample{now, count});
    pruneSamplesLocked(now);
}

void Statistics::pruneSamplesLocked(Clock::time_point now) {
    // Keep a little more than the longest window we report.
    const auto cutoff = now - std::chrono::seconds(70);
    while (!samples_.empty() && samples_.front().time < cutoff) {
        samples_.pop_front();
    }
}

double Statistics::hashrateOverWindow(double seconds) const {
    const auto now = Clock::now();
    const auto cutoff = now - std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000.0));

    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.empty()) return 0.0;

    uint64_t hashes = 0;
    Clock::time_point earliest = now;
    for (const Sample& sample : samples_) {
        if (sample.time < cutoff) continue;
        if (sample.time < earliest) earliest = sample.time;
        hashes += sample.hashes;
    }
    if (hashes == 0) return 0.0;

    // Divide by the real elapsed span covered by the samples, not by the
    // nominal window: a session younger than the window would otherwise report
    // an artificially low rate.
    double elapsed = std::chrono::duration<double>(now - earliest).count();
    if (elapsed <= 0.0) return 0.0;
    return static_cast<double>(hashes) / elapsed;
}

double Statistics::hashrate10s() const { return hashrateOverWindow(10.0); }
double Statistics::hashrate60s() const { return hashrateOverWindow(60.0); }

double Statistics::sessionAverageHashrate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sessionActive_ && sessionHashes_ == 0) return 0.0;
    const double elapsed = std::chrono::duration<double>(Clock::now() - sessionStart_).count();
    if (elapsed <= 0.0) return 0.0;
    return static_cast<double>(sessionHashes_) / elapsed;
}

double Statistics::uptimeSeconds() const {
    return std::chrono::duration<double>(Clock::now() - processStart_).count();
}

double Statistics::sessionSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sessionActive_) return 0.0;
    return std::chrono::duration<double>(Clock::now() - sessionStart_).count();
}

void Statistics::recordAcceptedShare(const bitcoin::UInt256& shareDifficulty,
                                     double shareDifficultyApprox) {
    acceptedShares_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    if (shareDifficulty > bestShareDifficulty_) {
        bestShareDifficulty_ = shareDifficulty;
        bestShareDifficultyApprox_ = shareDifficultyApprox;
    }
}

void Statistics::recordRejectedShare(const std::string& reason) {
    rejectedShares_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    lastRejectReason_ = reason;
}

void Statistics::recordStaleShare() { staleShares_.fetch_add(1, std::memory_order_relaxed); }

void Statistics::recordBlockFound() { blocksFound_.fetch_add(1, std::memory_order_relaxed); }

void Statistics::setMiningState(MiningState state) {
    miningState_.store(static_cast<int>(state), std::memory_order_relaxed);
}

void Statistics::setPoolState(PoolState state) {
    poolState_.store(static_cast<int>(state), std::memory_order_relaxed);
}

void Statistics::setPoolDescription(const std::string& description) {
    std::lock_guard<std::mutex> lock(mutex_);
    poolDescription_ = description;
}

void Statistics::setCurrentJobId(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentJobId_ = jobId;
}

void Statistics::setPoolDifficulty(double difficulty) {
    std::lock_guard<std::mutex> lock(mutex_);
    poolDifficulty_ = difficulty;
}

void Statistics::beginSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessionStart_ = Clock::now();
    sessionHashes_ = 0;
    sessionActive_ = true;
    samples_.clear();
    bestShareDifficulty_ = bitcoin::UInt256::zero();
    bestShareDifficultyApprox_ = 0.0;
    lastRejectReason_.clear();
    currentJobId_.clear();
}

void Statistics::endSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessionActive_ = false;
    samples_.clear();
}

MiningState Statistics::miningState() const {
    return static_cast<MiningState>(miningState_.load(std::memory_order_relaxed));
}

PoolState Statistics::poolState() const {
    return static_cast<PoolState>(poolState_.load(std::memory_order_relaxed));
}

bitcoin::UInt256 Statistics::bestShareDifficulty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bestShareDifficulty_;
}

double Statistics::bestShareDifficultyApprox() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bestShareDifficultyApprox_;
}

std::string Statistics::lastRejectReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastRejectReason_;
}

std::string Statistics::currentJobId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentJobId_;
}

double Statistics::poolDifficulty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return poolDifficulty_;
}

std::string Statistics::poolDescription() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return poolDescription_;
}

util::Json Statistics::toJson() const {
    util::Json j = util::Json::object();
    j.set("miningState", std::string(toString(miningState())));
    j.set("poolState", std::string(toString(poolState())));
    j.set("totalHashes", static_cast<int64_t>(totalHashes()));
    j.set("acceptedShares", static_cast<int64_t>(acceptedShares()));
    j.set("rejectedShares", static_cast<int64_t>(rejectedShares()));
    j.set("staleShares", static_cast<int64_t>(staleShares()));
    j.set("blocksFound", static_cast<int64_t>(blocksFound()));
    j.set("hashrate10s", hashrate10s());
    j.set("hashrate60s", hashrate60s());
    j.set("hashrateSessionAverage", sessionAverageHashrate());
    j.set("uptimeSeconds", uptimeSeconds());
    j.set("sessionSeconds", sessionSeconds());

    const bitcoin::UInt256 best = bestShareDifficulty();
    if (best.isZero()) {
        // No share has ever been found -- do not print a number.
        j.set("bestShareDifficulty", util::Json::null());
    } else {
        j.set("bestShareDifficulty", bestShareDifficultyApprox());
    }

    const std::string reject = lastRejectReason();
    j.set("lastRejectReason", reject.empty() ? util::Json::null() : util::Json(reject));

    const std::string job = currentJobId();
    j.set("currentJobId", job.empty() ? util::Json::null() : util::Json(job));

    const double difficulty = poolDifficulty();
    j.set("poolDifficulty", difficulty > 0.0 ? util::Json(difficulty) : util::Json::null());

    const std::string pool = poolDescription();
    j.set("pool", pool.empty() ? util::Json::null() : util::Json(pool));
    return j;
}

util::Json GpuTelemetry::toJson() const {
    util::Json j = util::Json::object();
    j.set("available", available);
    if (!available) {
        j.set("reason", unavailableReason);
    }
    j.set("name", name.empty() ? util::Json::null() : util::Json(name));
    j.set("temperatureCelsius",
          hasTemperature ? util::Json(temperatureCelsius) : util::Json::null());
    j.set("utilizationPercent",
          hasUtilization ? util::Json(utilizationPercent) : util::Json::null());
    j.set("powerWatts", hasPower ? util::Json(powerWatts) : util::Json::null());
    j.set("fanSpeedPercent", hasFanSpeed ? util::Json(fanSpeedPercent) : util::Json::null());
    j.set("memoryTotalBytes",
          hasMemory ? util::Json(static_cast<int64_t>(memoryTotalBytes)) : util::Json::null());
    return j;
}

} // namespace azd::mining
