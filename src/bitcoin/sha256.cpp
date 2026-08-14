#include "bitcoin/sha256.hpp"

#include <cstring>

namespace azd::bitcoin {
namespace {

constexpr uint32_t kRoundConstants[64] = {
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

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t bigSigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t bigSigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t smallSigma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t smallSigma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

inline uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void writeBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void writeBE64(uint8_t* p, uint64_t v) {
    writeBE32(p, static_cast<uint32_t>(v >> 32));
    writeBE32(p + 4, static_cast<uint32_t>(v));
}

} // namespace

SHA256::SHA256() { reset(); }

void SHA256::reset() {
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;
    bitLength_ = 0;
    bufferLen_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void SHA256::transform(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = readBE32(block + i * 4);
    for (int i = 16; i < 64; ++i) {
        w[i] = smallSigma1(w[i - 2]) + w[i - 7] + smallSigma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + bigSigma1(e) + ch(e, f, g) + kRoundConstants[i] + w[i];
        const uint32_t t2 = bigSigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void SHA256::update(const uint8_t* data, size_t len) {
    bitLength_ += static_cast<uint64_t>(len) * 8;

    if (bufferLen_ > 0) {
        const size_t need = kBlockSize - bufferLen_;
        const size_t take = len < need ? len : need;
        std::memcpy(buffer_ + bufferLen_, data, take);
        bufferLen_ += take;
        data += take;
        len -= take;
        if (bufferLen_ < kBlockSize) return;
        transform(buffer_);
        bufferLen_ = 0;
    }

    while (len >= kBlockSize) {
        transform(data);
        data += kBlockSize;
        len -= kBlockSize;
    }

    if (len > 0) {
        std::memcpy(buffer_, data, len);
        bufferLen_ = len;
    }
}

void SHA256::update(const std::vector<uint8_t>& data) {
    if (!data.empty()) update(data.data(), data.size());
}

void SHA256::finalize(uint8_t out[kDigestSize]) {
    const uint64_t bitLength = bitLength_;

    // Append 0x80, pad with zeros, then the 64-bit big-endian length.
    buffer_[bufferLen_++] = 0x80;
    if (bufferLen_ > kBlockSize - 8) {
        std::memset(buffer_ + bufferLen_, 0, kBlockSize - bufferLen_);
        transform(buffer_);
        bufferLen_ = 0;
    }
    std::memset(buffer_ + bufferLen_, 0, kBlockSize - 8 - bufferLen_);
    writeBE64(buffer_ + kBlockSize - 8, bitLength);
    transform(buffer_);

    for (int i = 0; i < 8; ++i) writeBE32(out + i * 4, state_[i]);
}

void sha256(const uint8_t* data, size_t len, uint8_t out[SHA256::kDigestSize]) {
    SHA256 ctx;
    ctx.update(data, len);
    ctx.finalize(out);
}

std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data) {
    std::array<uint8_t, 32> out{};
    sha256(data.data(), data.size(), out.data());
    return out;
}

void sha256d(const uint8_t* data, size_t len, uint8_t out[SHA256::kDigestSize]) {
    uint8_t first[SHA256::kDigestSize];
    sha256(data, len, first);
    sha256(first, SHA256::kDigestSize, out);
}

std::array<uint8_t, 32> sha256d(const std::vector<uint8_t>& data) {
    std::array<uint8_t, 32> out{};
    sha256d(data.data(), data.size(), out.data());
    return out;
}

} // namespace azd::bitcoin
