#include "util/hex.hpp"

namespace azd::util {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string toHex(const uint8_t* data, size_t len) {
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHexDigits[data[i] >> 4];
        out[i * 2 + 1] = kHexDigits[data[i] & 0x0F];
    }
    return out;
}

std::string toHex(const std::vector<uint8_t>& data) {
    return toHex(data.data(), data.size());
}

bool fromHex(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    std::vector<uint8_t> tmp;
    tmp.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexValue(hex[i]);
        const int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        tmp.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    out.swap(tmp);
    return true;
}

std::vector<uint8_t> fromHexOrEmpty(const std::string& hex) {
    std::vector<uint8_t> out;
    if (!fromHex(hex, out)) out.clear();
    return out;
}

bool fromHexFixed(const std::string& hex, uint8_t* out, size_t len) {
    if (hex.size() != len * 2) return false;
    std::vector<uint8_t> tmp;
    if (!fromHex(hex, tmp)) return false;
    for (size_t i = 0; i < len; ++i) out[i] = tmp[i];
    return true;
}

} // namespace azd::util
