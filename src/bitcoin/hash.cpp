#include "bitcoin/hash.hpp"

#include <algorithm>

#include "bitcoin/sha256.hpp"
#include "util/hex.hpp"

namespace azd::bitcoin {

bool Hash256::isZero() const {
    for (uint8_t b : bytes) {
        if (b != 0) return false;
    }
    return true;
}

std::string Hash256::toRawHex() const { return util::toHex(bytes.data(), bytes.size()); }

bool Hash256::fromRawHex(const std::string& hex, Hash256& out) {
    Hash256 tmp;
    if (!util::fromHexFixed(hex, tmp.bytes.data(), 32)) return false;
    out = tmp;
    return true;
}

std::string Hash256::toDisplayHex() const {
    std::array<uint8_t, 32> reversed{};
    for (size_t i = 0; i < 32; ++i) reversed[i] = bytes[31 - i];
    return util::toHex(reversed.data(), reversed.size());
}

bool Hash256::fromDisplayHex(const std::string& hex, Hash256& out) {
    Hash256 tmp;
    if (!util::fromHexFixed(hex, tmp.bytes.data(), 32)) return false;
    std::reverse(tmp.bytes.begin(), tmp.bytes.end());
    out = tmp;
    return true;
}

Hash256 doubleSha256(const uint8_t* data, size_t len) {
    Hash256 out;
    sha256d(data, len, out.bytes.data());
    return out;
}

Hash256 doubleSha256(const std::vector<uint8_t>& data) {
    return doubleSha256(data.data(), data.size());
}

Hash256 doubleSha256Concat(const Hash256& a, const Hash256& b) {
    uint8_t buf[64];
    std::copy(a.bytes.begin(), a.bytes.end(), buf);
    std::copy(b.bytes.begin(), b.bytes.end(), buf + 32);
    return doubleSha256(buf, sizeof(buf));
}

Hash256 swapWordBytes(const Hash256& in) {
    Hash256 out;
    for (size_t word = 0; word < 8; ++word) {
        for (size_t i = 0; i < 4; ++i) {
            out.bytes[word * 4 + i] = in.bytes[word * 4 + (3 - i)];
        }
    }
    return out;
}

} // namespace azd::bitcoin
