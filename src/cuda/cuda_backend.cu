// CUDA build of CudaMiningBackend.
//
// Compiled ONLY when AZD_ENABLE_CUDA=ON. The cloud container has no nvcc, so
// this file is never touched by the default build.
//
// Status: the kernel below is a real, complete SHA-256d nonce scanner. It is
// intentionally the straightforward version -- correctness first. It has NOT
// yet been executed on a GPU in this repository, so no performance figure for
// it appears anywhere in this project. Do not add one until it has been
// measured on real hardware, and do not trust a single share it finds until
// verifyResultOnCpu() has confirmed it.
//
// Planned optimizations, in the order they should be attempted (each one must
// be re-validated against the CPU reference before it is kept):
//   1. Host-side midstate: bytes 0..63 of the header never change while the
//      nonce varies, so the first compression can be done once on the CPU.
//   2. Constant-memory / register residency for the tail block and the target.
//   3. Skip the second hash early: if the final SHA-256 word cannot produce
//      enough leading zeros, reject before finishing.
//   4. Multiple nonces per thread and tuned block/grid occupancy.

#include "cuda/cuda_backend.hpp"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(AZD_ENABLE_NVML)
#include <nvml.h>
#endif

#include "bitcoin/block_header.hpp"
#include "bitcoin/difficulty.hpp"
#include "bitcoin/hash.hpp"
#include "mining/work.hpp"

namespace azd::cuda {
namespace {

// ---------------------------------------------------------------------------
// Device-side SHA-256
// ---------------------------------------------------------------------------

__constant__ uint32_t d_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

__device__ __forceinline__ uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

__device__ __forceinline__ uint32_t chDev(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

__device__ __forceinline__ uint32_t majDev(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

__device__ __forceinline__ uint32_t bigSigma0Dev(uint32_t x) {
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}
__device__ __forceinline__ uint32_t bigSigma1Dev(uint32_t x) {
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}
__device__ __forceinline__ uint32_t smallSigma0Dev(uint32_t x) {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}
__device__ __forceinline__ uint32_t smallSigma1Dev(uint32_t x) {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

/// One SHA-256 compression over a 16-word big-endian message block.
__device__ void sha256Compress(uint32_t state[8], const uint32_t block[16]) {
    uint32_t w[64];
#pragma unroll
    for (int i = 0; i < 16; ++i) w[i] = block[i];
#pragma unroll
    for (int i = 16; i < 64; ++i) {
        w[i] = smallSigma1Dev(w[i - 2]) + w[i - 7] + smallSigma0Dev(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

#pragma unroll
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + bigSigma1Dev(e) + chDev(e, f, g) + d_k[i] + w[i];
        const uint32_t t2 = bigSigma0Dev(a) + majDev(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

__device__ __forceinline__ void initState(uint32_t state[8]) {
    state[0] = 0x6a09e667u; state[1] = 0xbb67ae85u;
    state[2] = 0x3c6ef372u; state[3] = 0xa54ff53au;
    state[4] = 0x510e527fu; state[5] = 0x9b05688cu;
    state[6] = 0x1f83d9abu; state[7] = 0x5be0cd19u;
}

__device__ __forceinline__ uint32_t loadBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

/// SHA256d over the 80-byte header, producing 32 bytes in internal order.
__device__ void sha256dHeader(const uint8_t header[80], uint8_t out[32]) {
    uint32_t state[8];
    uint32_t block[16];

    // --- First hash, block 0: header bytes 0..63 ---
    initState(state);
#pragma unroll
    for (int i = 0; i < 16; ++i) block[i] = loadBE32(header + i * 4);
    sha256Compress(state, block);

    // --- First hash, block 1: header bytes 64..79 + padding + length(640) ---
#pragma unroll
    for (int i = 0; i < 4; ++i) block[i] = loadBE32(header + 64 + i * 4);
    block[4] = 0x80000000u;
#pragma unroll
    for (int i = 5; i < 15; ++i) block[i] = 0u;
    block[15] = 640u; // 80 bytes * 8 bits
    sha256Compress(state, block);

    // --- Second hash over the 32-byte digest ---
    uint32_t inner[8];
#pragma unroll
    for (int i = 0; i < 8; ++i) inner[i] = state[i];

    initState(state);
#pragma unroll
    for (int i = 0; i < 8; ++i) block[i] = inner[i];
    block[8] = 0x80000000u;
#pragma unroll
    for (int i = 9; i < 15; ++i) block[i] = 0u;
    block[15] = 256u; // 32 bytes * 8 bits
    sha256Compress(state, block);

#pragma unroll
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = uint8_t(state[i] >> 24);
        out[i * 4 + 1] = uint8_t(state[i] >> 16);
        out[i * 4 + 2] = uint8_t(state[i] >> 8);
        out[i * 4 + 3] = uint8_t(state[i]);
    }
}

/// hash <= target, both 32 bytes in internal (little-endian) byte order.
/// Compares from the most significant byte (index 31) downwards.
__device__ bool hashLessOrEqual(const uint8_t hash[32], const uint8_t target[32]) {
#pragma unroll
    for (int i = 31; i >= 0; --i) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true; // exactly equal still meets the target
}

struct KernelFound {
    uint32_t found;  // 0 or 1, written via atomicCAS
    uint32_t nonce;
};

/// One thread = one nonce. `headerTemplate` holds the 80 header bytes with the
/// nonce field ignored; each thread patches bytes 76..79 with its own nonce.
__global__ void mineKernel(const uint8_t* __restrict__ headerTemplate,
                           const uint8_t* __restrict__ shareTarget, uint32_t startNonce,
                           uint32_t nonceCount, KernelFound* __restrict__ result) {
    const uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= nonceCount) return;

    // Stop early once a peer thread has already found a share.
    if (result->found != 0u) return;

    const uint32_t nonce = startNonce + index;

    uint8_t header[80];
#pragma unroll
    for (int i = 0; i < 80; ++i) header[i] = headerTemplate[i];

    // Nonce is little-endian at offset 76.
    header[76] = uint8_t(nonce);
    header[77] = uint8_t(nonce >> 8);
    header[78] = uint8_t(nonce >> 16);
    header[79] = uint8_t(nonce >> 24);

    uint8_t hash[32];
    sha256dHeader(header, hash);

    if (hashLessOrEqual(hash, shareTarget)) {
        // Keep the lowest winning nonce so results are deterministic.
        if (atomicCAS(&result->found, 0u, 1u) == 0u) {
            result->nonce = nonce;
        } else {
            atomicMin(&result->nonce, nonce);
        }
    }
}

std::string cudaErrorText(cudaError_t err, const char* what) {
    return std::string(what) + ": " + cudaGetErrorString(err);
}

} // namespace

// ---------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------

struct CudaMiningBackend::Impl {
    int deviceIndex = 0;
    bool deviceReady = false;
    std::string reason = "not initialized";
    cudaDeviceProp props{};

    uint8_t* d_header = nullptr;
    uint8_t* d_target = nullptr;
    KernelFound* d_result = nullptr;

    bool nvmlReady = false;

    bool ensureDevice() {
        if (deviceReady) return true;

        int count = 0;
        cudaError_t err = cudaGetDeviceCount(&count);
        if (err != cudaSuccess) {
            reason = cudaErrorText(err, "cudaGetDeviceCount failed");
            return false;
        }
        if (count == 0) {
            reason = "no CUDA device found";
            return false;
        }
        if (deviceIndex < 0 || deviceIndex >= count) {
            reason = "CUDA device index " + std::to_string(deviceIndex) + " out of range (" +
                     std::to_string(count) + " device(s) present)";
            return false;
        }
        err = cudaSetDevice(deviceIndex);
        if (err != cudaSuccess) {
            reason = cudaErrorText(err, "cudaSetDevice failed");
            return false;
        }
        err = cudaGetDeviceProperties(&props, deviceIndex);
        if (err != cudaSuccess) {
            reason = cudaErrorText(err, "cudaGetDeviceProperties failed");
            return false;
        }

        if (!allocate()) return false;

        deviceReady = true;
        reason.clear();
        return true;
    }

    bool allocate() {
        cudaError_t err = cudaMalloc(&d_header, 80);
        if (err != cudaSuccess) {
            reason = cudaErrorText(err, "cudaMalloc(header) failed");
            return false;
        }
        err = cudaMalloc(&d_target, 32);
        if (err != cudaSuccess) {
            reason = cudaErrorText(err, "cudaMalloc(target) failed");
            return false;
        }
        err = cudaMalloc(&d_result, sizeof(KernelFound));
        if (err != cudaSuccess) {
            reason = cudaErrorText(err, "cudaMalloc(result) failed");
            return false;
        }
        return true;
    }

    void release() {
        if (d_header) cudaFree(d_header);
        if (d_target) cudaFree(d_target);
        if (d_result) cudaFree(d_result);
        d_header = nullptr;
        d_target = nullptr;
        d_result = nullptr;
#if defined(AZD_ENABLE_NVML)
        if (nvmlReady) nvmlShutdown();
        nvmlReady = false;
#endif
    }
};

CudaMiningBackend::CudaMiningBackend() : impl_(new Impl()) {}

CudaMiningBackend::~CudaMiningBackend() {
    if (impl_) {
        impl_->release();
        delete impl_;
        impl_ = nullptr;
    }
}

bool CudaMiningBackend::available() const { return impl_ && impl_->ensureDevice(); }

std::string CudaMiningBackend::unavailableReason() const {
    if (!impl_) return "backend not constructed";
    if (impl_->ensureDevice()) return std::string();
    return impl_->reason;
}

bool CudaMiningBackend::selectDevice(int deviceIndex) {
    if (!impl_) return false;
    if (deviceIndex < 0) return false;
    if (impl_->deviceIndex != deviceIndex) {
        impl_->release();
        impl_->deviceReady = false;
        impl_->deviceIndex = deviceIndex;
    }
    return impl_->ensureDevice();
}

DeviceInfoResult CudaMiningBackend::deviceInfoResult() const {
    DeviceInfoResult result;
    if (!impl_ || !impl_->ensureDevice()) {
        result.info.valid = false;
        result.info.backend = "cuda";
        result.message = impl_ ? impl_->reason : "backend not constructed";
        return result;
    }

    result.info.valid = true;
    result.info.backend = "cuda";
    result.info.index = impl_->deviceIndex;
    result.info.name = impl_->props.name;
    result.info.hasComputeCapability = true;
    result.info.computeCapabilityMajor = impl_->props.major;
    result.info.computeCapabilityMinor = impl_->props.minor;
    result.info.hasMemory = true;
    result.info.totalMemoryBytes = static_cast<uint64_t>(impl_->props.totalGlobalMem);
    result.info.hasMultiprocessorCount = true;
    result.info.multiprocessorCount = impl_->props.multiProcessorCount;
    return result;
}

mining::DeviceInfo CudaMiningBackend::deviceInfo() const { return deviceInfoResult().info; }

mining::GpuTelemetry CudaMiningBackend::telemetry() const {
    mining::GpuTelemetry telemetry;
    if (!impl_ || !impl_->ensureDevice()) {
        telemetry.available = false;
        telemetry.unavailableReason = impl_ ? impl_->reason : "backend not constructed";
        return telemetry;
    }

    telemetry.available = true;
    telemetry.unavailableReason.clear();
    telemetry.name = impl_->props.name;
    telemetry.hasMemory = true;
    telemetry.memoryTotalBytes = static_cast<uint64_t>(impl_->props.totalGlobalMem);

#if defined(AZD_ENABLE_NVML)
    // Temperature / power / utilization come from NVML and ONLY from NVML.
    // If any query fails, the corresponding field stays "unknown" and the
    // dashboard shows N/A. Never substitute a placeholder number.
    if (!impl_->nvmlReady && nvmlInit_v2() == NVML_SUCCESS) {
        impl_->nvmlReady = true;
    }
    if (impl_->nvmlReady) {
        nvmlDevice_t device{};
        if (nvmlDeviceGetHandleByIndex_v2(static_cast<unsigned>(impl_->deviceIndex), &device) ==
            NVML_SUCCESS) {
            unsigned temperature = 0;
            if (nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temperature) ==
                NVML_SUCCESS) {
                telemetry.hasTemperature = true;
                telemetry.temperatureCelsius = static_cast<double>(temperature);
            }
            nvmlUtilization_t utilization{};
            if (nvmlDeviceGetUtilizationRates(device, &utilization) == NVML_SUCCESS) {
                telemetry.hasUtilization = true;
                telemetry.utilizationPercent = static_cast<double>(utilization.gpu);
            }
            unsigned milliwatts = 0;
            if (nvmlDeviceGetPowerUsage(device, &milliwatts) == NVML_SUCCESS) {
                telemetry.hasPower = true;
                telemetry.powerWatts = static_cast<double>(milliwatts) / 1000.0;
            }
            unsigned fanPercent = 0;
            if (nvmlDeviceGetFanSpeed(device, &fanPercent) == NVML_SUCCESS) {
                telemetry.hasFanSpeed = true;
                telemetry.fanSpeedPercent = static_cast<double>(fanPercent);
            }
        }
    }
#endif
    return telemetry;
}

mining::MiningResult CudaMiningBackend::mine(const mining::MiningWork& work, uint32_t startNonce,
                                             uint64_t nonceCount,
                                             const std::atomic<bool>* stopFlag) {
    mining::MiningResult result;

    if (!work.valid) return mining::MiningResult::failure("invalid work");
    if (!impl_ || !impl_->ensureDevice()) {
        return mining::MiningResult::failure(impl_ ? impl_->reason : "backend not constructed");
    }
    if (nonceCount == 0) return result;

    const bitcoin::Hash256 targetBytes = work.shareTarget.toHashLittleEndian();

    cudaError_t err = cudaMemcpy(impl_->d_header, work.headerBytes.data(), 80,
                                 cudaMemcpyHostToDevice);
    if (err != cudaSuccess) return mining::MiningResult::failure(cudaErrorText(err, "H2D header"));
    err = cudaMemcpy(impl_->d_target, targetBytes.data(), 32, cudaMemcpyHostToDevice);
    if (err != cudaSuccess) return mining::MiningResult::failure(cudaErrorText(err, "H2D target"));

    const auto started = std::chrono::steady_clock::now();

    // Chunk the range so the stop flag is honoured promptly and so a single
    // launch never runs long enough to trip the display watchdog.
    constexpr uint64_t kChunkSize = 1u << 22; // 4M nonces per launch
    constexpr int kThreadsPerBlock = 256;

    uint64_t remaining = nonceCount;
    uint64_t offset = 0;
    uint64_t hashesDone = 0;

    while (remaining > 0) {
        if (stopFlag && stopFlag->load()) break;

        const uint64_t chunk = remaining < kChunkSize ? remaining : kChunkSize;

        KernelFound host{0u, 0u};
        err = cudaMemcpy(impl_->d_result, &host, sizeof(host), cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
            return mining::MiningResult::failure(cudaErrorText(err, "H2D result"));
        }

        const int blocks =
            static_cast<int>((chunk + kThreadsPerBlock - 1) / kThreadsPerBlock);
        mineKernel<<<blocks, kThreadsPerBlock>>>(
            impl_->d_header, impl_->d_target,
            static_cast<uint32_t>(startNonce + offset), static_cast<uint32_t>(chunk),
            impl_->d_result);

        err = cudaGetLastError();
        if (err != cudaSuccess) {
            return mining::MiningResult::failure(cudaErrorText(err, "kernel launch"));
        }
        err = cudaDeviceSynchronize();
        if (err != cudaSuccess) {
            return mining::MiningResult::failure(cudaErrorText(err, "kernel execution"));
        }

        err = cudaMemcpy(&host, impl_->d_result, sizeof(host), cudaMemcpyDeviceToHost);
        if (err != cudaSuccess) {
            return mining::MiningResult::failure(cudaErrorText(err, "D2H result"));
        }

        hashesDone += chunk;

        if (host.found != 0u) {
            result.found = true;
            result.nonce = host.nonce;
            // Recompute on the CPU. A GPU result is never trusted directly --
            // see CLAUDE.md rule 6.
            result.hash = mining::hashWorkAtNonce(work, host.nonce);
            result.meetsNetworkTarget =
                bitcoin::hashMeetsTarget(result.hash, work.networkTarget);
            break;
        }

        offset += chunk;
        remaining -= chunk;
    }

    result.hashesPerformed = hashesDone;
    result.elapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

int CudaMiningBackend::deviceCount() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess) return 0;
    return count;
}

bool CudaMiningBackend::compiledWithCuda() { return true; }

} // namespace azd::cuda
