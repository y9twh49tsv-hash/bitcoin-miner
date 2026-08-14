#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "mining/backend.hpp"
#include "mining/stats.hpp"
#include "mining/work.hpp"

namespace azd::cuda {

/// deviceInfo() paired with an explanation, so callers can report *why* there
/// is no device rather than showing an empty card.
struct DeviceInfoResult {
    mining::DeviceInfo info;
    std::string message;
};

/// GPU mining backend.
///
/// Two implementations are compiled, selected by the AZD_ENABLE_CUDA CMake
/// option:
///
///   AZD_ENABLE_CUDA=OFF  -> cuda_backend_stub.cpp
///        available() is false, deviceInfo().valid is false, and mine()
///        returns an error result saying "CUDA backend not compiled".
///        NOTHING is simulated: no hashrate, no temperature, no device.
///
///   AZD_ENABLE_CUDA=ON   -> cuda_backend.cu
///        Real device queries and a real SHA-256d kernel.
///
/// Callers must treat `available() == false` as "no GPU data exists" and
/// surface it as unavailable, never as zero-valued telemetry.
class CudaMiningBackend {
public:
    CudaMiningBackend();
    ~CudaMiningBackend();

    CudaMiningBackend(const CudaMiningBackend&) = delete;
    CudaMiningBackend& operator=(const CudaMiningBackend&) = delete;

    /// True only when the project was compiled with CUDA support AND a usable
    /// device was actually found at runtime.
    bool available() const;

    /// Human-readable reason why the backend is unavailable. Empty when it is
    /// available.
    std::string unavailableReason() const;

    /// Selects the device to use. Returns false if CUDA is not compiled in or
    /// the index is out of range.
    bool selectDevice(int deviceIndex);

    /// Queries the device. Safe to call when unavailable (returns a DeviceInfo
    /// with `valid == false` plus the reason).
    DeviceInfoResult deviceInfoResult() const;
    mining::DeviceInfo deviceInfo() const;

    /// Live telemetry (temperature, utilization, power). Without CUDA -- and
    /// without NVML at runtime -- every field stays "unknown" so the dashboard
    /// shows N/A.
    mining::GpuTelemetry telemetry() const;

    /// Scan `nonceCount` nonces starting at `startNonce`.
    ///
    /// Returns as soon as a nonce meeting work.shareTarget is found, or when
    /// the range is exhausted. `stopFlag`, when non-null and set to true,
    /// aborts the scan at the next opportunity (used for job switching and for
    /// the STOP control).
    mining::MiningResult mine(const mining::MiningWork& work, uint32_t startNonce,
                              uint64_t nonceCount,
                              const std::atomic<bool>* stopFlag = nullptr);

    /// Number of CUDA devices visible. Always 0 without CUDA.
    static int deviceCount();

    /// True when the binary was compiled with AZD_ENABLE_CUDA=ON, regardless of
    /// whether a device is present.
    static bool compiledWithCuda();

private:
    struct Impl;
    Impl* impl_;
};

/// Fixed message used everywhere the backend is missing, so the CLI, the API
/// and the dashboard all say the same thing.
inline const char* kCudaNotCompiledMessage() { return "CUDA backend not compiled"; }

} // namespace azd::cuda
