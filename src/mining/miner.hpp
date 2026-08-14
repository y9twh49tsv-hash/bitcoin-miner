#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "cuda/cuda_backend.hpp"
#include "mining/backend.hpp"
#include "mining/stats.hpp"
#include "mining/work.hpp"
#include "stratum/client.hpp"
#include "util/json.hpp"

namespace azd::mining {

/// Single-threaded CPU nonce search over SHA-256d.
///
/// PURPOSE: correctness, not throughput. A CPU cannot mine Bitcoin
/// competitively and this class makes no claim otherwise. It exists so that
/// (a) the whole pipeline can be exercised without a GPU, and (b) it can serve
/// as the oracle every CUDA result is checked against.
///
/// It reports exactly the number of hashes it computed and the time it really
/// took -- both measured, neither projected.
class CpuReferenceMiner {
public:
    DeviceInfo deviceInfo() const;

    MiningResult mine(const MiningWork& work, uint32_t startNonce, uint64_t nonceCount,
                      const std::atomic<bool>* stopFlag = nullptr);

    /// Recompute one nonce and report whether it really meets the share target.
    /// This is the verification step every GPU result must pass.
    static bool verify(const MiningWork& work, uint32_t nonce, bitcoin::Hash256& hashOut,
                       bool& meetsShareTarget, bool& meetsNetworkTarget);
};

/// Owns the whole mining pipeline: pool connection, work generation, backends,
/// worker threads and statistics.
///
/// Invariants:
///  * No worker thread hashes anything unless state is Running.
///  * Every statistic changes only as a result of work that actually happened.
///  * A share is submitted only after the CPU has independently confirmed it
///    meets the share target.
class MinerController {
public:
    explicit MinerController(core::Config config);
    ~MinerController();

    MinerController(const MinerController&) = delete;
    MinerController& operator=(const MinerController&) = delete;

    /// Starts pool connection and workers. Returns false with `error` set when
    /// the miner cannot start (e.g. no pool configured).
    bool start(std::string& error);
    void stop();

    bool isRunning() const { return running_.load(); }

    Statistics& statistics() { return stats_; }
    const Statistics& statistics() const { return stats_; }

    core::Config config() const;
    /// Applies a partial config patch. Refused while mining is running so the
    /// pool credentials cannot change under a live connection.
    bool updateConfig(const util::Json& patch, std::string& error);

    GpuTelemetry gpuTelemetry() const;
    DeviceInfo activeDeviceInfo() const;
    std::string backendName() const;

    util::Json statusJson() const;
    util::Json gpuJson() const;
    util::Json poolJson() const;

    /// Runs the built-in correctness self-test (genesis block + a CPU nonce
    /// search over a synthetic low-difficulty job). Returns true on success and
    /// writes a human-readable report.
    bool selfTest(std::string& report);

private:
    void workerLoop(size_t workerIndex);
    void onJob(const MiningJob& job);
    void onDifficulty(double difficulty);
    void onSubmitResult(const stratum::ShareSubmission& share, bool accepted,
                        const std::string& reason);
    void onPoolState(PoolState state, const std::string& detail);

    /// Returns false when no job is available yet.
    bool snapshotJob(MiningJob& job, uint64_t& generation) const;

    void handleFoundNonce(const MiningWork& work, const MiningResult& result);

    mutable std::mutex configMutex_;
    core::Config config_;

    Statistics stats_;

    std::unique_ptr<stratum::StratumClient> pool_;
    cuda::CudaMiningBackend cudaBackend_;
    bool useCuda_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{true};

    std::vector<std::thread> workers_;

    mutable std::mutex jobMutex_;
    MiningJob currentJob_;
    bool hasJob_ = false;
    uint64_t jobGeneration_ = 0;

    std::atomic<uint64_t> extranonce2Counter_{0};
};

} // namespace azd::mining
