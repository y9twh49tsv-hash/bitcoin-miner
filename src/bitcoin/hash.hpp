#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace azd::bitcoin {

/// A 256-bit Bitcoin hash held in *internal byte order* -- exactly the bytes
/// SHA256d produces, and exactly the bytes that go into a serialized block
/// header.
///
/// Bitcoin displays hashes (block explorers, RPC, the genesis hash everyone
/// quotes) in the REVERSED byte order. Every conversion in this project is
/// explicit about which of the two it means:
///
///   toRawHex()      / fromRawHex()      -> internal order, wire/serialization
///   toDisplayHex()  / fromDisplayHex()  -> reversed, human/RPC order
///
/// There is no implicit conversion between the two. Byte-order bugs are the
/// single most common source of "my miner finds nothing" and they are silent,
/// so the naming is deliberately noisy.
struct Hash256 {
    std::array<uint8_t, 32> bytes{};

    Hash256() = default;

    static Hash256 zero() { return Hash256{}; }

    bool isZero() const;

    /// Hex in internal (serialization) byte order.
    std::string toRawHex() const;
    static bool fromRawHex(const std::string& hex, Hash256& out);

    /// Hex in display order (what a block explorer shows). This is the
    /// byte-reverse of the internal order.
    std::string toDisplayHex() const;
    static bool fromDisplayHex(const std::string& hex, Hash256& out);

    bool operator==(const Hash256& other) const { return bytes == other.bytes; }
    bool operator!=(const Hash256& other) const { return bytes != other.bytes; }

    const uint8_t* data() const { return bytes.data(); }
    uint8_t* data() { return bytes.data(); }
    static constexpr size_t size() { return 32; }
};

/// SHA256d over a byte range, returned in internal byte order.
Hash256 doubleSha256(const uint8_t* data, size_t len);
Hash256 doubleSha256(const std::vector<uint8_t>& data);

/// SHA256d(a || b), the operation every Merkle step performs.
Hash256 doubleSha256Concat(const Hash256& a, const Hash256& b);

/// Reverse the order of each 4-byte word within the 32 bytes, keeping word
/// positions. Stratum's `prevhash` field needs this; see stratum/message.hpp.
Hash256 swapWordBytes(const Hash256& in);

} // namespace azd::bitcoin
