#include "bitcoin/block_header.hpp"

#include <cstring>

namespace azd::bitcoin {

void writeLE32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t readLE32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

uint32_t byteSwap32(uint32_t value) {
    return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

void setHeaderNonce(uint8_t* header80, uint32_t nonce) {
    writeLE32(header80 + BlockHeader::kNonceOffset, nonce);
}

void BlockHeader::serializeInto(uint8_t* out) const {
    writeLE32(out + 0, version);
    std::memcpy(out + 4, previousBlockHash.bytes.data(), 32);
    std::memcpy(out + 36, merkleRoot.bytes.data(), 32);
    writeLE32(out + 68, timestamp);
    writeLE32(out + 72, bits);
    writeLE32(out + 76, nonce);
}

std::array<uint8_t, BlockHeader::kSerializedSize> BlockHeader::serialize() const {
    std::array<uint8_t, kSerializedSize> out{};
    serializeInto(out.data());
    return out;
}

bool BlockHeader::deserialize(const uint8_t* data, size_t len, BlockHeader& out) {
    if (len != kSerializedSize) return false;
    BlockHeader h;
    h.version = readLE32(data + 0);
    std::memcpy(h.previousBlockHash.bytes.data(), data + 4, 32);
    std::memcpy(h.merkleRoot.bytes.data(), data + 36, 32);
    h.timestamp = readLE32(data + 68);
    h.bits = readLE32(data + 72);
    h.nonce = readLE32(data + 76);
    out = h;
    return true;
}

Hash256 BlockHeader::hash() const {
    const auto serialized = serialize();
    return doubleSha256(serialized.data(), serialized.size());
}

std::string BlockHeader::displayHash() const { return hash().toDisplayHex(); }

} // namespace azd::bitcoin
