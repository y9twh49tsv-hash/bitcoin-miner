#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bitcoin/hash.hpp"

namespace azd::bitcoin {

/// The 80-byte Bitcoin block header.
///
/// Serialization layout (exactly 80 bytes, all integers little-endian):
///
///   offset  size  field
///   0       4     version        (uint32 LE)
///   4       32    previous hash  (internal byte order, written as-is)
///   36      32    merkle root    (internal byte order, written as-is)
///   68      4     timestamp      (uint32 LE)
///   72      4     bits           (uint32 LE, compact target)
///   76      4     nonce          (uint32 LE)
///
/// The block hash is SHA256d over these 80 bytes, and the result is in
/// internal byte order -- reverse it to get the hash people quote.
struct BlockHeader {
    static constexpr size_t kSerializedSize = 80;
    static constexpr size_t kNonceOffset = 76;

    uint32_t version = 0;
    Hash256 previousBlockHash;
    Hash256 merkleRoot;
    uint32_t timestamp = 0;
    uint32_t bits = 0;
    uint32_t nonce = 0;

    /// Serialize into exactly 80 bytes.
    std::array<uint8_t, kSerializedSize> serialize() const;
    void serializeInto(uint8_t* out) const;

    /// Parse exactly 80 bytes. Returns false if `len != 80`.
    static bool deserialize(const uint8_t* data, size_t len, BlockHeader& out);

    /// SHA256d of the serialized header, in internal byte order.
    Hash256 hash() const;

    /// The hash as normally displayed (byte-reversed hex).
    std::string displayHash() const;
};

/// Little-endian 32-bit read/write helpers. Used by the header and by the
/// coinbase/extranonce construction, so they live here rather than being
/// re-implemented per call site.
void writeLE32(uint8_t* out, uint32_t value);
uint32_t readLE32(const uint8_t* in);

/// Byte-swap a 32-bit value (e.g. converting a big-endian hex field such as
/// stratum's `version` / `nbits` / `ntime` into header order).
uint32_t byteSwap32(uint32_t value);

/// Overwrite only the nonce inside an already-serialized 80-byte header.
/// This is the hot path of any nonce search: the other 76 bytes never change.
void setHeaderNonce(uint8_t* header80, uint32_t nonce);

} // namespace azd::bitcoin
