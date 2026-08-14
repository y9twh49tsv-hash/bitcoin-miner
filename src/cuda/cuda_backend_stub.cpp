// Non-CUDA build of CudaMiningBackend.
//
// Compiled when AZD_ENABLE_CUDA=OFF (the default, and what the cloud CI uses).
//
// This file deliberately contains NO simulation. It does not fake a device, a
// hashrate, a temperature or a result. Every entry point reports honestly that
// the GPU backend is not present, and mine() returns an error rather than
// pretending to have hashed anything.

#include "cuda/cuda_backend.hpp"

namespace azd::cuda {

struct CudaMiningBackend::Impl {
    // Nothing to hold: there is no device and no state without CUDA.
};

CudaMiningBackend::CudaMiningBackend() : impl_(nullptr) {}
CudaMiningBackend::~CudaMiningBackend() = default;

bool CudaMiningBackend::available() const { return false; }

std::string CudaMiningBackend::unavailableReason() const { return kCudaNotCompiledMessage(); }

bool CudaMiningBackend::selectDevice(int) { return false; }

DeviceInfoResult CudaMiningBackend::deviceInfoResult() const {
    DeviceInfoResult result;
    result.info.valid = false;
    result.info.backend = "none";
    result.message = kCudaNotCompiledMessage();
    return result;
}

mining::DeviceInfo CudaMiningBackend::deviceInfo() const { return deviceInfoResult().info; }

mining::GpuTelemetry CudaMiningBackend::telemetry() const {
    mining::GpuTelemetry telemetry;
    telemetry.available = false;
    telemetry.unavailableReason = kCudaNotCompiledMessage();
    // hasTemperature / hasUtilization / hasPower stay false: the API will
    // serialize them as null and the dashboard will show "N/A".
    return telemetry;
}

mining::MiningResult CudaMiningBackend::mine(const mining::MiningWork&, uint32_t, uint64_t,
                                             const std::atomic<bool>*) {
    return mining::MiningResult::failure(kCudaNotCompiledMessage());
}

int CudaMiningBackend::deviceCount() { return 0; }

bool CudaMiningBackend::compiledWithCuda() { return false; }

} // namespace azd::cuda
