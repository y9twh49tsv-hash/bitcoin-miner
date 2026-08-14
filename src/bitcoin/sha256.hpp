#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace azd::bitcoin {

/// Streaming SHA-256 (FIPS 180-4). Portable, no intrinsics.
///
/// This is the correctness reference for the whole project: every GPU result
/// must ultimately agree with what this class produces.
class SHA256 {
public:
    static constexpr size_t kDigestSize = 32;
    static constexpr size_t kBlockSize = 64;

    SHA256();

    void reset();
    void update(const uint8_t* data, size_t len);
    void update(const std::vector<uint8_t>& data);

    /// Writes the digest and leaves the object in a finalized state.
    /// Call reset() before reusing.
    void finalize(uint8_t out[kDigestSize]);

private:
    void transform(const uint8_t* block);

    uint32_t state_[8];
    uint64_t bitLength_;
    uint8_t buffer_[kBlockSize];
    size_t bufferLen_;
};

/// SHA256(data)
void sha256(const uint8_t* data, size_t len, uint8_t out[SHA256::kDigestSize]);
std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data);

/// SHA256d(data) = SHA256(SHA256(data)) -- the hash Bitcoin mining uses.
void sha256d(const uint8_t* data, size_t len, uint8_t out[SHA256::kDigestSize]);
std::array<uint8_t, 32> sha256d(const std::vector<uint8_t>& data);

} // namespace azd::bitcoin
