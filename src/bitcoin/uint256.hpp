#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "bitcoin/hash.hpp"

namespace azd::bitcoin {

/// Unsigned 256-bit integer, stored as four 64-bit limbs, limb[0] least
/// significant. Everything here is exact integer arithmetic -- no doubles.
///
/// Bitcoin's target comparison MUST be exact: a hash that is one unit above
/// the target is a rejected share, and floating point cannot represent 256-bit
/// values without losing exactly the low-order bits that decide the outcome.
class UInt256 {
public:
    UInt256() : limbs_{0, 0, 0, 0} {}
    explicit UInt256(uint64_t v) : limbs_{v, 0, 0, 0} {}

    static UInt256 zero() { return UInt256(); }
    static UInt256 max();

    /// Interpret a 256-bit hash (internal byte order = little-endian) as a
    /// number. This is how Bitcoin compares a block hash against a target.
    static UInt256 fromHashLittleEndian(const Hash256& hash);

    /// Big-endian hex, i.e. the order a target/difficulty is normally written
    /// ("00000000ffff0000...."). Accepts 1..64 hex characters.
    static bool fromHexBigEndian(const std::string& hex, UInt256& out);

    /// 64 lowercase hex characters, most significant nibble first.
    std::string toHexBigEndian() const;

    /// Convert back to a hash in internal (little-endian) byte order.
    Hash256 toHashLittleEndian() const;

    bool isZero() const;

    /// Index of the highest set bit + 1 (0 for zero).
    unsigned bitLength() const;

    uint64_t limb(size_t i) const { return limbs_[i]; }
    void setLimb(size_t i, uint64_t v) { limbs_[i] = v; }

    UInt256 operator<<(unsigned shift) const;
    UInt256 operator>>(unsigned shift) const;
    UInt256 operator+(const UInt256& other) const;
    UInt256 operator-(const UInt256& other) const;
    UInt256 operator|(const UInt256& other) const;

    bool operator==(const UInt256& o) const { return limbs_ == o.limbs_; }
    bool operator!=(const UInt256& o) const { return limbs_ != o.limbs_; }
    bool operator<(const UInt256& o) const { return compare(o) < 0; }
    bool operator<=(const UInt256& o) const { return compare(o) <= 0; }
    bool operator>(const UInt256& o) const { return compare(o) > 0; }
    bool operator>=(const UInt256& o) const { return compare(o) >= 0; }

    /// -1 / 0 / +1
    int compare(const UInt256& other) const;

    /// Exact truncating division. Division by zero yields zero (callers must
    /// not rely on that -- check first).
    static UInt256 divide(const UInt256& numerator, const UInt256& denominator,
                          UInt256* remainder = nullptr);

    /// Lossy: only for display/logging (e.g. printing a share difficulty).
    /// Never use for share/block validation.
    double toDoubleApproximate() const;

private:
    std::array<uint64_t, 4> limbs_;
};

} // namespace azd::bitcoin
