#pragma once

#include <cstdint>
#include <string>

#include "bitcoin/hash.hpp"
#include "bitcoin/uint256.hpp"

namespace azd::bitcoin {

/// Bitcoin's difficulty-1 target: 0x00000000FFFF0000...0000 (the target
/// encoded by nBits 0x1d00ffff). Pool share difficulty is defined relative to
/// this value.
UInt256 difficultyOneTarget();

/// The pool/stratum difficulty-1 target used for *share* difficulty. Bitcoin
/// and every Stratum V1 pool use the same constant here.
inline UInt256 shareDifficultyOneTarget() { return difficultyOneTarget(); }

/// Decode a compact "nBits" value into a 256-bit target.
///
///   nBits = 0xEEMMMMMM  ->  target = 0x00MMMMMM * 256^(EE - 3)
///
/// Returns false for encodings Bitcoin rejects: a negative mantissa (sign bit
/// set) or an overflowing exponent. A `true` result with a zero target means
/// the mantissa itself was zero, which is valid-but-unmineable.
bool decodeCompactBits(uint32_t nBits, UInt256& target, bool* negative = nullptr,
                       bool* overflow = nullptr);

/// Convenience form: returns a zero target when the encoding is invalid.
UInt256 targetFromCompactBits(uint32_t nBits);

/// Encode a 256-bit target back into compact nBits form. Round-trips every
/// canonically-encoded target.
uint32_t encodeCompactBits(const UInt256& target);

/// True when `hash` satisfies `target`, i.e. hash <= target when both are read
/// as 256-bit little-endian numbers. Exact integer comparison, no doubles.
bool hashMeetsTarget(const Hash256& hash, const UInt256& target);

/// Target for a given pool share difficulty: target = difficulty1 / difficulty.
/// Difficulties are supplied by the pool as a decimal (often fractional, e.g.
/// 0.001), so the conversion is done in fixed point to keep it exact enough
/// while never touching the final comparison, which stays integer.
UInt256 targetFromShareDifficulty(double difficulty);

/// The difficulty a specific hash actually achieved: difficulty1 / hash.
/// Returned as an exact TRUNCATING integer quotient, so any share easier than
/// difficulty 1 yields 0. Use shareDifficultyOfHashFixed32() when shares below
/// difficulty 1 have to be compared -- which is the normal case on a pool.
/// A zero hash (impossible in practice) yields the maximum value.
UInt256 shareDifficultyOfHash(const Hash256& hash);

/// The same value in 2^-32 fixed point: (difficulty1 << 32) / hash.
///
/// This is what "best difficulty" is ranked by. Pools routinely hand out
/// difficulties well below 1, and the plain integer quotient truncates every
/// such share to 0, which would make them all compare equal. The fixed-point
/// form keeps the ordering exact without introducing floating point.
UInt256 shareDifficultyOfHashFixed32(const Hash256& hash);

/// Same value as a double, for display only (dashboard, logs). Never use this
/// to decide whether a share is valid.
double shareDifficultyOfHashApproximate(const Hash256& hash);

/// Network difficulty implied by an nBits value, for display only.
double networkDifficultyApproximate(uint32_t nBits);

} // namespace azd::bitcoin
