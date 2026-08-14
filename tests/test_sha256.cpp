// SHA-256 / SHA-256d correctness against published vectors.
//
// Vectors come from FIPS 180-2 Appendix B and the standard NESSIE/NIST test
// set. If any of these fail, nothing else in the project can be trusted.

#include <string>
#include <vector>

#include "bitcoin/sha256.hpp"
#include "test_framework.hpp"
#include "util/hex.hpp"

using azd::bitcoin::sha256;
using azd::bitcoin::sha256d;
using azd::bitcoin::SHA256;

namespace {

std::vector<uint8_t> bytesOf(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string sha256Hex(const std::string& input) {
    uint8_t digest[32];
    sha256(reinterpret_cast<const uint8_t*>(input.data()), input.size(), digest);
    return azd::util::toHex(digest, 32);
}

std::string sha256dHex(const std::string& input) {
    uint8_t digest[32];
    sha256d(reinterpret_cast<const uint8_t*>(input.data()), input.size(), digest);
    return azd::util::toHex(digest, 32);
}

} // namespace

TEST("sha256 of empty input") {
    CHECK_EQ(sha256Hex(""),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST("sha256 of \"abc\"") {
    CHECK_EQ(sha256Hex("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST("sha256 of 448-bit message") {
    CHECK_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST("sha256 of 896-bit message spanning multiple blocks") {
    CHECK_EQ(sha256Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                       "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
             std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

TEST("sha256 of one million 'a' characters") {
    SHA256 ctx;
    const std::vector<uint8_t> chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) ctx.update(chunk.data(), chunk.size());
    uint8_t digest[32];
    ctx.finalize(digest);
    CHECK_EQ(azd::util::toHex(digest, 32),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST("streaming update matches one-shot for every split point") {
    const std::string message =
        "The quick brown fox jumps over the lazy dog; Bitcoin uses SHA-256d.";
    const std::string expected = sha256Hex(message);

    for (size_t split = 0; split <= message.size(); ++split) {
        SHA256 ctx;
        ctx.update(reinterpret_cast<const uint8_t*>(message.data()), split);
        ctx.update(reinterpret_cast<const uint8_t*>(message.data()) + split,
                   message.size() - split);
        uint8_t digest[32];
        ctx.finalize(digest);
        CHECK_EQ(azd::util::toHex(digest, 32), expected);
    }
}

TEST("update across exact block boundaries") {
    // 64, 55, 56 and 119 bytes exercise the padding edge cases.
    for (size_t len : {size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(119),
                       size_t(128)}) {
        const std::string message(len, 'x');
        SHA256 ctx;
        ctx.update(reinterpret_cast<const uint8_t*>(message.data()), message.size());
        uint8_t streamed[32];
        ctx.finalize(streamed);

        uint8_t oneShot[32];
        sha256(reinterpret_cast<const uint8_t*>(message.data()), message.size(), oneShot);
        CHECK_EQ(azd::util::toHex(streamed, 32), azd::util::toHex(oneShot, 32));
    }
}

TEST("reset makes the context reusable") {
    SHA256 ctx;
    const auto abc = bytesOf("abc");
    ctx.update(abc);
    uint8_t first[32];
    ctx.finalize(first);

    ctx.reset();
    ctx.update(abc);
    uint8_t second[32];
    ctx.finalize(second);

    CHECK_EQ(azd::util::toHex(first, 32), azd::util::toHex(second, 32));
}

TEST("sha256d is sha256 applied twice") {
    // Independently computed: SHA256(SHA256("abc")).
    CHECK_EQ(sha256dHex("abc"),
             std::string("4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358"));

    // And structurally: hashing the digest of the digest must agree.
    uint8_t once[32];
    sha256(reinterpret_cast<const uint8_t*>("abc"), 3, once);
    uint8_t twice[32];
    sha256(once, 32, twice);
    CHECK_EQ(sha256dHex("abc"), azd::util::toHex(twice, 32));
}

TEST("sha256d of empty input") {
    CHECK_EQ(sha256dHex(""),
             std::string("5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456"));
}

AZD_TEST_MAIN("sha256")
