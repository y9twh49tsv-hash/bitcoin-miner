#pragma once

#include <cstdint>
#include <string>

#include "bitcoin/hash.hpp"
#include "mining/work.hpp"

namespace azd::mining {

/// Description of a compute device. `valid == false` means no device was
/// interrogated -- the fields are then meaningless and must not be displayed.
struct DeviceInfo {
    bool valid = false;
    std::string name;
    std::string backend = "none"; // "cpu-reference" | "cuda"
    int index = -1;

    // Only meaningful when `valid` is true and the backend really reported it.
    bool hasComputeCapability = false;
    int computeCapabilityMajor = 0;
    int computeCapabilityMinor = 0;
    bool hasMemory = false;
    uint64_t totalMemoryBytes = 0;
    bool hasMultiprocessorCount = false;
    int multiprocessorCount = 0;
};

/// Outcome of scanning a nonce range.
///
/// `hashesPerformed` and `elapsedSeconds` are measured, never estimated: the
/// backend counts the nonces it actually hashed and times the actual work.
struct MiningResult {
    bool found = false;         // a nonce meeting the share target was found
    uint32_t nonce = 0;
    bitcoin::Hash256 hash;

    bool meetsNetworkTarget = false; // a real block, not just a share

    uint64_t hashesPerformed = 0;
    double elapsedSeconds = 0.0;

    bool error = false;
    std::string errorMessage;

    /// Hashes per second for this call. Returns 0 when nothing was measured --
    /// never a projected or nominal figure.
    double hashesPerSecond() const {
        if (elapsedSeconds <= 0.0 || hashesPerformed == 0) return 0.0;
        return static_cast<double>(hashesPerformed) / elapsedSeconds;
    }

    static MiningResult failure(const std::string& message) {
        MiningResult result;
        result.error = true;
        result.errorMessage = message;
        return result;
    }
};

} // namespace azd::mining
