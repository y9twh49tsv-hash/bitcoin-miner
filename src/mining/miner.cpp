#include "mining/miner.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "bitcoin/block_header.hpp"
#include "bitcoin/difficulty.hpp"
#include "bitcoin/merkle.hpp"
#include "stratum/message.hpp"
#include "util/hex.hpp"
#include "util/logging.hpp"

namespace azd::mining {
namespace {

constexpr const char* kComponent = "miner";

/// Nonces per backend call. Small enough that a stop request or a new job is
/// noticed quickly, large enough that timing overhead is negligible.
constexpr uint64_t kCpuChunkSize = 1u << 18;
constexpr uint64_t kGpuChunkSize = 1u << 24;

std::string formatDouble(double value, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// CpuReferenceMiner
// ---------------------------------------------------------------------------

DeviceInfo CpuReferenceMiner::deviceInfo() const {
    DeviceInfo info;
    info.valid = true;
    info.backend = "cpu-reference";
    info.name = "CPU reference (correctness oracle, not a production miner)";
    info.index = -1;
    return info;
}

MiningResult CpuReferenceMiner::mine(const MiningWork& work, uint32_t startNonce,
                                     uint64_t nonceCount, const std::atomic<bool>* stopFlag) {
    MiningResult result;
    if (!work.valid) return MiningResult::failure("invalid work");
    if (nonceCount == 0) return result;

    // Copy the header once; only the 4 nonce bytes change per iteration.
    std::array<uint8_t, bitcoin::BlockHeader::kSerializedSize> header = work.headerBytes;

    const auto started = std::chrono::steady_clock::now();
    uint64_t performed = 0;

    for (uint64_t i = 0; i < nonceCount; ++i) {
        if (stopFlag != nullptr && (i % 4096) == 0 && stopFlag->load()) break;

        const uint32_t nonce = static_cast<uint32_t>(startNonce + i);
        bitcoin::setHeaderNonce(header.data(), nonce);

        const bitcoin::Hash256 hash = bitcoin::doubleSha256(header.data(), header.size());
        ++performed;

        if (bitcoin::hashMeetsTarget(hash, work.shareTarget)) {
            result.found = true;
            result.nonce = nonce;
            result.hash = hash;
            result.meetsNetworkTarget = bitcoin::hashMeetsTarget(hash, work.networkTarget);
            break;
        }
    }

    result.hashesPerformed = performed;
    result.elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

bool CpuReferenceMiner::verify(const MiningWork& work, uint32_t nonce, bitcoin::Hash256& hashOut,
                               bool& meetsShareTarget, bool& meetsNetworkTarget) {
    if (!work.valid) {
        meetsShareTarget = false;
        meetsNetworkTarget = false;
        return false;
    }
    hashOut = hashWorkAtNonce(work, nonce);
    meetsShareTarget = bitcoin::hashMeetsTarget(hashOut, work.shareTarget);
    meetsNetworkTarget = bitcoin::hashMeetsTarget(hashOut, work.networkTarget);
    return meetsShareTarget;
}

// ---------------------------------------------------------------------------
// MinerController
// ---------------------------------------------------------------------------

MinerController::MinerController(core::Config config) : config_(std::move(config)) {
    stats_.setMiningState(MiningState::Stopped);
    stats_.setPoolState(PoolState::Disconnected);
}

MinerController::~MinerController() { stop(); }

core::Config MinerController::config() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

bool MinerController::updateConfig(const util::Json& patch, std::string& error) {
    if (running_.load()) {
        error = "stop mining before changing the configuration";
        return false;
    }
    std::lock_guard<std::mutex> lock(configMutex_);
    config_.applyPartialJson(patch);
    return true;
}

std::string MinerController::backendName() const { return useCuda_ ? "cuda" : "cpu-reference"; }

GpuTelemetry MinerController::gpuTelemetry() const { return cudaBackend_.telemetry(); }

DeviceInfo MinerController::activeDeviceInfo() const {
    if (useCuda_) return cudaBackend_.deviceInfo();
    return CpuReferenceMiner().deviceInfo();
}

bool MinerController::start(std::string& error) {
    if (running_.load()) {
        error = "miner is already running";
        return false;
    }

    const core::Config config = this->config();

    if (!config.poolConfigured()) {
        error =
            "no pool configured: set pool.host, pool.port and pool.username before starting. "
            "This miner never mines to a built-in address.";
        // NOT an error state. Nothing was started and nothing broke -- the
        // miner is simply still stopped, which is exactly what the status
        // should say. The reason is returned to the caller, which is where a
        // rejected start request belongs. Reporting a sticky "error" here
        // would misdescribe a perfectly healthy, unconfigured miner.
        stats_.setMiningState(MiningState::Stopped);
        return false;
    }

    stats_.setMiningState(MiningState::Starting);
    stats_.beginSession();

    // Choose a backend. CUDA is used only when it is compiled in AND a real
    // device answered; otherwise the CPU reference path runs, clearly labelled.
    useCuda_ = false;
    if (cuda::CudaMiningBackend::compiledWithCuda()) {
        if (cudaBackend_.selectDevice(config.miner.device) && cudaBackend_.available()) {
            useCuda_ = true;
            const DeviceInfo info = cudaBackend_.deviceInfo();
            util::logInfo(kComponent, "using CUDA device: " + info.name);
        } else {
            util::logWarn(kComponent,
                          "CUDA compiled in but unavailable: " + cudaBackend_.unavailableReason());
        }
    } else {
        util::logInfo(kComponent, std::string(cuda::kCudaNotCompiledMessage()) +
                                      " - falling back to the CPU reference search. "
                                      "This is for correctness testing only.");
    }

    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        hasJob_ = false;
        currentJob_ = MiningJob{};
        jobGeneration_ = 0;
    }
    extranonce2Counter_.store(0);

    stratum::ClientConfig poolConfig;
    poolConfig.host = config.pool.host;
    poolConfig.port = config.pool.port;
    poolConfig.username = config.pool.username;
    poolConfig.password = config.pool.password;
    poolConfig.userAgent = "azd-bitcoin-miner/0.1";

    stratum::StratumClient::Callbacks callbacks;
    callbacks.onJob = [this](const MiningJob& job) { onJob(job); };
    callbacks.onDifficulty = [this](double difficulty) { onDifficulty(difficulty); };
    callbacks.onSubmitResult = [this](const stratum::ShareSubmission& share, bool accepted,
                                      const std::string& reason) {
        onSubmitResult(share, accepted, reason);
    };
    callbacks.onStateChange = [this](PoolState state, const std::string& detail) {
        onPoolState(state, detail);
    };

    pool_ = std::make_unique<stratum::StratumClient>(poolConfig, callbacks);
    stats_.setPoolDescription(pool_->description());

    if (!pool_->start()) {
        error = "failed to start the pool client";
        stats_.setMiningState(MiningState::Error);
        stats_.endSession();
        pool_.reset();
        return false;
    }

    stopFlag_.store(false);
    running_.store(true);
    stats_.setMiningState(MiningState::Running);

    // The GPU path is driven by a single worker (the device is the parallel
    // unit); the CPU path uses the configured thread count.
    size_t workerCount = 1;
    if (!useCuda_) {
        const int configured = config.miner.cpuThreads;
        workerCount = configured <= 0 ? 0 : static_cast<size_t>(configured);
    }

    if (workerCount == 0) {
        util::logWarn(kComponent, "no worker threads configured: connected to the pool but not "
                                  "hashing (miner.cpuThreads = 0)");
    }

    for (size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this, i] { workerLoop(i); });
    }

    util::logInfo(kComponent, "mining started with backend " + backendName() + " and " +
                                  std::to_string(workerCount) + " worker(s)");
    return true;
}

void MinerController::stop() {
    if (!running_.exchange(false)) {
        // Still make sure a half-started pool client is torn down.
        if (pool_) {
            pool_->stop();
            pool_.reset();
        }
        stats_.setMiningState(MiningState::Stopped);
        return;
    }

    stats_.setMiningState(MiningState::Stopping);
    stopFlag_.store(true);

    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();

    if (pool_) {
        pool_->stop();
        pool_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        hasJob_ = false;
        currentJob_ = MiningJob{};
    }

    stats_.endSession();
    stats_.setMiningState(MiningState::Stopped);
    stats_.setPoolState(PoolState::Disconnected);
    util::logInfo(kComponent, "mining stopped");
}

bool MinerController::snapshotJob(MiningJob& job, uint64_t& generation) const {
    std::lock_guard<std::mutex> lock(jobMutex_);
    if (!hasJob_) return false;
    job = currentJob_;
    generation = jobGeneration_;
    return true;
}

void MinerController::onJob(const MiningJob& job) {
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        currentJob_ = job;
        hasJob_ = true;
        ++jobGeneration_;
    }
    stats_.setCurrentJobId(job.jobId);
}

void MinerController::onDifficulty(double difficulty) {
    stats_.setPoolDifficulty(difficulty);
    std::lock_guard<std::mutex> lock(jobMutex_);
    if (hasJob_) {
        currentJob_.difficulty = difficulty;
        ++jobGeneration_;
    }
}

void MinerController::onPoolState(PoolState state, const std::string& detail) {
    stats_.setPoolState(state);
    if (!detail.empty()) {
        util::logDebug(kComponent, std::string("pool state: ") + toString(state) + " " + detail);
    }
    if (state == PoolState::Disconnected || state == PoolState::Error) {
        // Never keep mining a job from a dead connection.
        std::lock_guard<std::mutex> lock(jobMutex_);
        hasJob_ = false;
        ++jobGeneration_;
    }
}

void MinerController::onSubmitResult(const stratum::ShareSubmission& share, bool accepted,
                                     const std::string& reason) {
    if (accepted) {
        // The exact integer difficulty was computed from the share's real hash
        // when it was found; nothing is re-derived or estimated here.
        stats_.recordAcceptedShare(share.achievedDifficultyExact, share.achievedDifficulty);
    } else {
        stats_.recordRejectedShare(reason);
    }
}

void MinerController::handleFoundNonce(const MiningWork& work, const MiningResult& result) {
    // RULE: never trust a backend result. Recompute on the CPU before anything
    // is submitted or counted.
    bitcoin::Hash256 hash;
    bool meetsShare = false;
    bool meetsNetwork = false;
    CpuReferenceMiner::verify(work, result.nonce, hash, meetsShare, meetsNetwork);

    if (!meetsShare) {
        util::logError(kComponent,
                       "backend reported a share that CPU verification rejected (job " +
                           work.jobId + ", nonce " + stratum::toBigEndianHex32(result.nonce) +
                           "). Not submitting. This indicates a backend bug.");
        return;
    }

    const double achieved = bitcoin::shareDifficultyOfHashApproximate(hash);
    util::logInfo(kComponent, "valid share found: job " + work.jobId + " nonce " +
                                  stratum::toBigEndianHex32(result.nonce) + " difficulty " +
                                  formatDouble(achieved, 3) + " hash " + hash.toDisplayHex());

    if (meetsNetwork) {
        util::logInfo(kComponent, "*** this share also meets the NETWORK target: " +
                                      hash.toDisplayHex() + " ***");
        stats_.recordBlockFound();
    }

    stratum::ShareSubmission submission;
    submission.jobId = work.jobId;
    submission.extranonce2Hex = util::toHex(work.extranonce2);
    submission.nTime = work.nTime();
    submission.nonce = result.nonce;
    submission.achievedDifficulty = achieved;
    // Ranked in 2^-32 fixed point so shares easier than difficulty 1 -- the
    // normal case on a pool -- still order correctly against each other.
    submission.achievedDifficultyExact = bitcoin::shareDifficultyOfHashFixed32(hash);

    if (!pool_ || !pool_->submitShare(submission)) {
        // Not connected any more: the share is lost. Count it as stale, never
        // as accepted.
        stats_.recordStaleShare();
        util::logWarn(kComponent, "share could not be submitted (pool not authorized); "
                                  "counted as stale");
    }
}

void MinerController::workerLoop(size_t workerIndex) {
    CpuReferenceMiner cpuMiner;
    const uint64_t chunkSize = useCuda_ ? kGpuChunkSize : kCpuChunkSize;

    while (running_.load() && !stopFlag_.load()) {
        MiningJob job;
        uint64_t generation = 0;
        if (!snapshotJob(job, generation)) {
            // No work yet. Sleep briefly; do NOT invent a job.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        const uint64_t counter = extranonce2Counter_.fetch_add(1);
        const MiningWork work = buildWork(job, counter);
        if (!work.valid) {
            util::logWarn(kComponent, "job " + job.jobId + " produced invalid work, skipping");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        // Each worker starts in a different region of the nonce space so two
        // threads never duplicate the same hashes.
        uint64_t nonce = (uint64_t(workerIndex) << 32) / (workers_.empty() ? 1 : workers_.size());
        nonce &= 0xFFFFFFFFull;

        uint64_t remaining = 0x100000000ull;

        while (remaining > 0 && running_.load() && !stopFlag_.load()) {
            // Abandon the range as soon as the pool sends new work.
            uint64_t currentGeneration = 0;
            MiningJob unusedJob;
            if (!snapshotJob(unusedJob, currentGeneration) || currentGeneration != generation) {
                break;
            }

            const uint64_t chunk = remaining < chunkSize ? remaining : chunkSize;

            const MiningResult result =
                useCuda_ ? cudaBackend_.mine(work, static_cast<uint32_t>(nonce), chunk, &stopFlag_)
                         : cpuMiner.mine(work, static_cast<uint32_t>(nonce), chunk, &stopFlag_);

            if (result.error) {
                util::logError(kComponent, "backend error: " + result.errorMessage);
                stats_.setMiningState(MiningState::Error);
                running_.store(false);
                return;
            }

            // Only ever count hashes the backend actually performed.
            stats_.addHashes(result.hashesPerformed);

            if (result.found) {
                handleFoundNonce(work, result);
                // Continue past the winning nonce with the same work.
                nonce = (result.nonce + 1ull) & 0xFFFFFFFFull;
                remaining = remaining > (result.hashesPerformed + 1)
                                ? remaining - (result.hashesPerformed + 1)
                                : 0;
                continue;
            }

            nonce = (nonce + chunk) & 0xFFFFFFFFull;
            remaining -= chunk;
        }
    }
}

bool MinerController::selfTest(std::string& report) {
    std::string out;
    bool ok = true;

    // 1. Genesis block: the canonical end-to-end check of SHA-256d, header
    //    layout and byte order.
    {
        bitcoin::BlockHeader header;
        header.version = 1;
        bitcoin::Hash256::fromDisplayHex(
            "0000000000000000000000000000000000000000000000000000000000000000",
            header.previousBlockHash);
        bitcoin::Hash256::fromDisplayHex(
            "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b", header.merkleRoot);
        header.timestamp = 1231006505;
        header.bits = 0x1d00ffff;
        header.nonce = 2083236893;

        const std::string expected =
            "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";
        const std::string actual = header.displayHash();
        const bool pass = actual == expected;
        ok = ok && pass;
        out += pass ? "  [PASS] genesis block hash\n"
                    : "  [FAIL] genesis block hash\n         got      " + actual +
                          "\n         expected " + expected + "\n";
    }

    // 2. CPU nonce search: take the genesis header, blank the nonce, and search
    //    for the real one at the real network target. This performs actual
    //    hashing and reports actual timing.
    {
        MiningWork work;
        work.jobId = "selftest";
        work.header.version = 1;
        bitcoin::Hash256::fromDisplayHex(
            "0000000000000000000000000000000000000000000000000000000000000000",
            work.header.previousBlockHash);
        bitcoin::Hash256::fromDisplayHex(
            "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b",
            work.header.merkleRoot);
        work.header.timestamp = 1231006505;
        work.header.bits = 0x1d00ffff;
        work.header.nonce = 0;
        work.header.serializeInto(work.headerBytes.data());
        work.networkTarget = bitcoin::targetFromCompactBits(0x1d00ffff);
        work.shareTarget = work.networkTarget;
        work.valid = true;

        // Start slightly below the known genesis nonce so the search is short
        // but still real: every hash below is genuinely computed.
        const uint32_t knownNonce = 2083236893;
        const uint32_t startNonce = knownNonce - 50000;

        CpuReferenceMiner miner;
        const MiningResult result = miner.mine(work, startNonce, 100000);

        const bool pass = result.found && result.nonce == knownNonce;
        ok = ok && pass;

        out += pass ? "  [PASS] CPU nonce search recovered the genesis nonce\n"
                    : "  [FAIL] CPU nonce search did not recover the genesis nonce\n";
        out += "         hashes performed: " + std::to_string(result.hashesPerformed) + "\n";
        out += "         elapsed:          " + formatDouble(result.elapsedSeconds, 3) + " s\n";
        out += "         measured rate:    " +
               formatDouble(result.hashesPerSecond() / 1000.0, 1) + " kH/s (CPU reference)\n";
    }

    // 3. Backend availability, stated honestly.
    {
        out += "  [INFO] CUDA compiled: ";
        out += cuda::CudaMiningBackend::compiledWithCuda() ? "yes\n" : "no\n";
        if (!cuda::CudaMiningBackend::compiledWithCuda()) {
            out += "         " + std::string(cuda::kCudaNotCompiledMessage()) +
                   " - GPU hashrate, temperature, power and utilization are unavailable.\n";
        } else {
            cuda::CudaMiningBackend backend;
            if (backend.available()) {
                const DeviceInfo info = backend.deviceInfo();
                out += "         device: " + info.name + "\n";
            } else {
                out += "         no usable device: " + backend.unavailableReason() + "\n";
            }
        }
    }

    report = out;
    return ok;
}

util::Json MinerController::statusJson() const {
    util::Json j = util::Json::object();
    j.set("name", config().miner.name);
    j.set("version", std::string("0.1.0"));
    j.set("backend", backendName());
    j.set("cudaCompiled", cuda::CudaMiningBackend::compiledWithCuda());
    j.set("running", running_.load());
    j.set("stats", stats_.toJson());
    return j;
}

util::Json MinerController::gpuJson() const {
    util::Json j = gpuTelemetry().toJson();

    const DeviceInfo info = cudaBackend_.deviceInfo();
    util::Json device = util::Json::object();
    device.set("valid", info.valid);
    device.set("backend", info.backend);
    if (info.valid) {
        device.set("name", info.name);
        device.set("index", static_cast<int64_t>(info.index));
        device.set("computeCapability",
                   info.hasComputeCapability
                       ? util::Json(std::to_string(info.computeCapabilityMajor) + "." +
                                    std::to_string(info.computeCapabilityMinor))
                       : util::Json::null());
        device.set("multiprocessors", info.hasMultiprocessorCount
                                          ? util::Json(static_cast<int64_t>(
                                                info.multiprocessorCount))
                                          : util::Json::null());
    }
    j.set("device", device);
    j.set("cudaCompiled", cuda::CudaMiningBackend::compiledWithCuda());
    j.set("activeBackend", backendName());
    return j;
}

util::Json MinerController::poolJson() const {
    const core::Config config = this->config();

    util::Json j = util::Json::object();
    j.set("configured", config.poolConfigured());
    j.set("host", config.pool.host.empty() ? util::Json::null() : util::Json(config.pool.host));
    j.set("port", static_cast<int64_t>(config.pool.port));
    j.set("username",
          config.pool.username.empty() ? util::Json::null() : util::Json(config.pool.username));
    // The password is NEVER included -- only whether one is set.
    j.set("passwordSet", !config.pool.password.empty());
    j.set("state", std::string(toString(stats_.poolState())));

    const std::string description = stats_.poolDescription();
    j.set("url", description.empty() ? util::Json::null() : util::Json(description));

    const double difficulty = stats_.poolDifficulty();
    j.set("difficulty", difficulty > 0.0 ? util::Json(difficulty) : util::Json::null());

    const std::string job = stats_.currentJobId();
    j.set("currentJobId", job.empty() ? util::Json::null() : util::Json(job));
    j.set("acceptedShares", static_cast<int64_t>(stats_.acceptedShares()));
    j.set("rejectedShares", static_cast<int64_t>(stats_.rejectedShares()));
    j.set("staleShares", static_cast<int64_t>(stats_.staleShares()));
    return j;
}

} // namespace azd::mining
