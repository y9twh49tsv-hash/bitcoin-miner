#include "bitcoin/difficulty.hpp"

#include <cmath>

namespace azd::bitcoin {

UInt256 difficultyOneTarget() {
    // 0x00000000FFFF0000000000000000000000000000000000000000000000000000
    UInt256 t;
    t.setLimb(3, 0x00000000FFFF0000ull);
    return t;
}

bool decodeCompactBits(uint32_t nBits, UInt256& target, bool* negative, bool* overflow) {
    const uint32_t exponent = nBits >> 24;
    uint32_t mantissa = nBits & 0x007FFFFFu;
    const bool isNegative = (nBits & 0x00800000u) != 0 && mantissa != 0;

    if (negative) *negative = isNegative;
    if (overflow) *overflow = false;

    UInt256 value;
    bool isOverflow = false;

    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);
        value = UInt256(mantissa);
    } else {
        value = UInt256(mantissa);
        const uint32_t shiftBytes = exponent - 3;
        if (shiftBytes > 32) {
            isOverflow = mantissa != 0;
        } else {
            // Would the mantissa be pushed past bit 255?
            const unsigned bits = value.bitLength();
            if (bits != 0 && bits + shiftBytes * 8 > 256) {
                isOverflow = true;
            } else {
                value = value << (shiftBytes * 8);
            }
        }
    }

    if (overflow) *overflow = isOverflow;

    if (isNegative || isOverflow) {
        target = UInt256::zero();
        return false;
    }

    target = value;
    return true;
}

UInt256 targetFromCompactBits(uint32_t nBits) {
    UInt256 target;
    if (!decodeCompactBits(nBits, target)) return UInt256::zero();
    return target;
}

uint32_t encodeCompactBits(const UInt256& target) {
    if (target.isZero()) return 0;

    // Number of significant bytes.
    unsigned size = (target.bitLength() + 7) / 8;
    uint32_t compact;
    if (size <= 3) {
        // Left-align the value into the 3 mantissa bytes.
        const uint64_t low = target.limb(0);
        compact = static_cast<uint32_t>(low << (8 * (3 - size)));
    } else {
        const UInt256 shifted = target >> (8 * (size - 3));
        compact = static_cast<uint32_t>(shifted.limb(0) & 0x00FFFFFFull);
    }

    // The sign bit doubles as part of the mantissa, so a value whose top bit
    // is set has to be renormalized one byte up.
    if (compact & 0x00800000u) {
        compact >>= 8;
        size += 1;
    }

    return compact | (size << 24);
}

bool hashMeetsTarget(const Hash256& hash, const UInt256& target) {
    if (target.isZero()) return false;
    return UInt256::fromHashLittleEndian(hash) <= target;
}

UInt256 targetFromShareDifficulty(double difficulty) {
    if (!(difficulty > 0.0) || !std::isfinite(difficulty)) return UInt256::zero();

    // Fixed-point: target = difficulty1 * 2^32 / round(difficulty * 2^32).
    // Pools send difficulties like 0.001 or 16384; scaling by 2^32 keeps small
    // fractional values usable while the arithmetic itself stays integer.
    constexpr double kScale = 4294967296.0; // 2^32
    const double scaledDouble = difficulty * kScale;
    if (scaledDouble < 1.0) {
        // Difficulty so small the scaled denominator underflows; clamp to the
        // easiest representable target rather than dividing by zero.
        return UInt256::max();
    }

    UInt256 denominator;
    if (scaledDouble >= 18446744073709551616.0) {
        // Difficulty >= 2^32: scale the numerator down instead so we stay in
        // range, at the cost of the fractional part (irrelevant that high).
        denominator = UInt256(static_cast<uint64_t>(difficulty));
        if (denominator.isZero()) return UInt256::zero();
        return UInt256::divide(difficultyOneTarget(), denominator);
    }

    denominator = UInt256(static_cast<uint64_t>(scaledDouble));
    const UInt256 numerator = difficultyOneTarget() << 32;
    return UInt256::divide(numerator, denominator);
}

UInt256 shareDifficultyOfHash(const Hash256& hash) {
    const UInt256 value = UInt256::fromHashLittleEndian(hash);
    if (value.isZero()) return UInt256::max();
    return UInt256::divide(difficultyOneTarget(), value);
}

UInt256 shareDifficultyOfHashFixed32(const Hash256& hash) {
    const UInt256 value = UInt256::fromHashLittleEndian(hash);
    if (value.isZero()) return UInt256::max();
    // difficulty1 occupies 224 bits, so shifting it left by 32 cannot overflow.
    return UInt256::divide(difficultyOneTarget() << 32, value);
}

double shareDifficultyOfHashApproximate(const Hash256& hash) {
    const UInt256 value = UInt256::fromHashLittleEndian(hash);
    if (value.isZero()) return 0.0;
    const double v = value.toDoubleApproximate();
    if (v <= 0.0) return 0.0;
    return difficultyOneTarget().toDoubleApproximate() / v;
}

double networkDifficultyApproximate(uint32_t nBits) {
    UInt256 target;
    if (!decodeCompactBits(nBits, target) || target.isZero()) return 0.0;
    return difficultyOneTarget().toDoubleApproximate() / target.toDoubleApproximate();
}

} // namespace azd::bitcoin
