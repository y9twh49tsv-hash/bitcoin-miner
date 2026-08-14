#include "bitcoin/uint256.hpp"

#include <cctype>

namespace azd::bitcoin {

UInt256 UInt256::max() {
    UInt256 v;
    for (size_t i = 0; i < 4; ++i) v.limbs_[i] = ~uint64_t(0);
    return v;
}

UInt256 UInt256::fromHashLittleEndian(const Hash256& hash) {
    UInt256 v;
    for (size_t limb = 0; limb < 4; ++limb) {
        uint64_t acc = 0;
        for (size_t byte = 0; byte < 8; ++byte) {
            acc |= static_cast<uint64_t>(hash.bytes[limb * 8 + byte]) << (byte * 8);
        }
        v.limbs_[limb] = acc;
    }
    return v;
}

Hash256 UInt256::toHashLittleEndian() const {
    Hash256 h;
    for (size_t limb = 0; limb < 4; ++limb) {
        for (size_t byte = 0; byte < 8; ++byte) {
            h.bytes[limb * 8 + byte] = static_cast<uint8_t>(limbs_[limb] >> (byte * 8));
        }
    }
    return h;
}

bool UInt256::fromHexBigEndian(const std::string& hex, UInt256& out) {
    if (hex.empty() || hex.size() > 64) return false;
    UInt256 v;
    for (char c : hex) {
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        v = (v << 4) | UInt256(static_cast<uint64_t>(digit));
    }
    out = v;
    return true;
}

std::string UInt256::toHexBigEndian() const {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 64; ++i) {
        const size_t nibbleFromLow = 63 - i;
        const uint64_t limbValue = limbs_[nibbleFromLow / 16];
        const unsigned shift = static_cast<unsigned>((nibbleFromLow % 16) * 4);
        out[i] = kDigits[(limbValue >> shift) & 0xF];
    }
    return out;
}

bool UInt256::isZero() const {
    for (uint64_t l : limbs_) {
        if (l != 0) return false;
    }
    return true;
}

unsigned UInt256::bitLength() const {
    for (int limb = 3; limb >= 0; --limb) {
        if (limbs_[static_cast<size_t>(limb)] == 0) continue;
        uint64_t v = limbs_[static_cast<size_t>(limb)];
        unsigned bits = 0;
        while (v) {
            v >>= 1;
            ++bits;
        }
        return static_cast<unsigned>(limb) * 64 + bits;
    }
    return 0;
}

int UInt256::compare(const UInt256& other) const {
    for (int i = 3; i >= 0; --i) {
        const size_t idx = static_cast<size_t>(i);
        if (limbs_[idx] < other.limbs_[idx]) return -1;
        if (limbs_[idx] > other.limbs_[idx]) return 1;
    }
    return 0;
}

UInt256 UInt256::operator<<(unsigned shift) const {
    UInt256 out;
    if (shift >= 256) return out;
    const unsigned limbShift = shift / 64;
    const unsigned bitShift = shift % 64;
    for (int i = 3; i >= 0; --i) {
        const size_t dst = static_cast<size_t>(i);
        if (dst < limbShift) continue;
        const size_t src = dst - limbShift;
        uint64_t value = limbs_[src] << bitShift;
        if (bitShift != 0 && src > 0) {
            value |= limbs_[src - 1] >> (64 - bitShift);
        }
        out.limbs_[dst] = value;
    }
    return out;
}

UInt256 UInt256::operator>>(unsigned shift) const {
    UInt256 out;
    if (shift >= 256) return out;
    const unsigned limbShift = shift / 64;
    const unsigned bitShift = shift % 64;
    for (size_t dst = 0; dst < 4; ++dst) {
        const size_t src = dst + limbShift;
        if (src > 3) break;
        uint64_t value = limbs_[src] >> bitShift;
        if (bitShift != 0 && src + 1 <= 3) {
            value |= limbs_[src + 1] << (64 - bitShift);
        }
        out.limbs_[dst] = value;
    }
    return out;
}

UInt256 UInt256::operator+(const UInt256& other) const {
    UInt256 out;
    uint64_t carry = 0;
    for (size_t i = 0; i < 4; ++i) {
        const uint64_t sum = limbs_[i] + other.limbs_[i];
        uint64_t newCarry = (sum < limbs_[i]) ? 1u : 0u;
        const uint64_t total = sum + carry;
        if (total < sum) newCarry = 1;
        out.limbs_[i] = total;
        carry = newCarry;
    }
    return out;
}

UInt256 UInt256::operator-(const UInt256& other) const {
    UInt256 out;
    uint64_t borrow = 0;
    for (size_t i = 0; i < 4; ++i) {
        const uint64_t a = limbs_[i];
        const uint64_t b = other.limbs_[i];
        uint64_t diff = a - b;
        uint64_t newBorrow = (a < b) ? 1u : 0u;
        if (borrow) {
            if (diff == 0) newBorrow = 1;
            diff -= 1;
        }
        out.limbs_[i] = diff;
        borrow = newBorrow;
    }
    return out;
}

UInt256 UInt256::operator|(const UInt256& other) const {
    UInt256 out;
    for (size_t i = 0; i < 4; ++i) out.limbs_[i] = limbs_[i] | other.limbs_[i];
    return out;
}

UInt256 UInt256::divide(const UInt256& numerator, const UInt256& denominator, UInt256* remainder) {
    UInt256 quotient;
    UInt256 rem;

    if (denominator.isZero()) {
        if (remainder) *remainder = rem;
        return quotient;
    }

    // Classic restoring shift-subtract long division, MSB first.
    for (int bit = 255; bit >= 0; --bit) {
        rem = rem << 1;
        const size_t limb = static_cast<size_t>(bit) / 64;
        const unsigned shift = static_cast<unsigned>(bit) % 64;
        if ((numerator.limbs_[limb] >> shift) & 1u) {
            rem = rem | UInt256(1);
        }
        if (rem >= denominator) {
            rem = rem - denominator;
            quotient.limbs_[limb] |= (uint64_t(1) << shift);
        }
    }

    if (remainder) *remainder = rem;
    return quotient;
}

double UInt256::toDoubleApproximate() const {
    double result = 0.0;
    double scale = 1.0;
    for (size_t i = 0; i < 4; ++i) {
        result += scale * static_cast<double>(limbs_[i]);
        scale *= 18446744073709551616.0; // 2^64
    }
    return result;
}

} // namespace azd::bitcoin
