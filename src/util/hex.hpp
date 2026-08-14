#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace azd::util {

/// Encode raw bytes as lowercase hex. No prefix, no separators.
std::string toHex(const uint8_t* data, size_t len);
std::string toHex(const std::vector<uint8_t>& data);

/// Decode a hex string into raw bytes.
/// Returns false if the input has odd length or contains non-hex characters.
/// `out` is left untouched on failure.
bool fromHex(const std::string& hex, std::vector<uint8_t>& out);

/// Convenience wrapper. Returns an empty vector on malformed input, so only
/// use it where the input is known-good (tests, literals).
std::vector<uint8_t> fromHexOrEmpty(const std::string& hex);

/// Decode exactly `len` bytes of hex into `out`. Returns false on any mismatch.
bool fromHexFixed(const std::string& hex, uint8_t* out, size_t len);

} // namespace azd::util
