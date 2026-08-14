// Compact nBits decoding, 256-bit targets and exact hash-vs-target comparison.
//
// Every comparison here is integer. If any of this drifts into floating point,
// shares one unit above the target start being submitted (and rejected).

#include <string>

#include "bitcoin/difficulty.hpp"
#include "bitcoin/hash.hpp"
#include "bitcoin/uint256.hpp"
#include "test_framework.hpp"

using azd::bitcoin::decodeCompactBits;
using azd::bitcoin::difficultyOneTarget;
using azd::bitcoin::encodeCompactBits;
using azd::bitcoin::Hash256;
using azd::bitcoin::hashMeetsTarget;
using azd::bitcoin::targetFromCompactBits;
using azd::bitcoin::UInt256;

namespace {

UInt256 fromHex(const std::string& hex) {
    UInt256 value;
    REQUIRE(UInt256::fromHexBigEndian(hex, value));
    return value;
}

Hash256 hashFromDisplay(const std::string& hex) {
    Hash256 hash;
    REQUIRE(Hash256::fromDisplayHex(hex, hash));
    return hash;
}

} // namespace

// --- UInt256 basics -------------------------------------------------------

TEST("uint256 hex round-trip") {
    const std::string hex =
        "00000000ffff0000000000000000000000000000000000000000000000000000";
    CHECK_EQ(fromHex(hex).toHexBigEndian(), hex);
}

TEST("uint256 comparison across limb boundaries") {
    CHECK(fromHex("1") < fromHex("2"));
    CHECK(fromHex("ffffffffffffffff") < fromHex("10000000000000000"));
    CHECK(fromHex("10000000000000000") > fromHex("ffffffffffffffff"));
    CHECK(fromHex("ff") == fromHex("00ff"));
    CHECK(UInt256::zero() < UInt256(1));
    CHECK(UInt256::max() > fromHex("ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
}

TEST("uint256 shifts") {
    CHECK_EQ((UInt256(1) << 64).toHexBigEndian(), fromHex("10000000000000000").toHexBigEndian());
    CHECK_EQ((UInt256(1) << 255).toHexBigEndian(),
             fromHex("8000000000000000000000000000000000000000000000000000000000000000")
                 .toHexBigEndian());
    CHECK(((UInt256(1) << 255) >> 255) == UInt256(1));
    CHECK((UInt256(1) << 256) == UInt256::zero());
    CHECK_EQ(((fromHex("ff00") >> 8)).toHexBigEndian(), fromHex("ff").toHexBigEndian());
}

TEST("uint256 addition and subtraction carry correctly") {
    const UInt256 a = fromHex("ffffffffffffffff");
    CHECK((a + UInt256(1)) == fromHex("10000000000000000"));
    CHECK((fromHex("10000000000000000") - UInt256(1)) == a);

    // Borrow across all four limbs.
    const UInt256 big = fromHex("100000000000000000000000000000000000000000000000");
    CHECK((big - UInt256(1)) == fromHex("0fffffffffffffffffffffffffffffffffffffffffffffff"));
}

TEST("uint256 division") {
    UInt256 remainder;
    const UInt256 quotient = UInt256::divide(UInt256(100), UInt256(7), &remainder);
    CHECK(quotient == UInt256(14));
    CHECK(remainder == UInt256(2));

    // Exact division of the difficulty-1 target by 1 is the identity.
    CHECK(UInt256::divide(difficultyOneTarget(), UInt256(1)) == difficultyOneTarget());

    // Dividing by a value larger than the numerator gives zero.
    CHECK(UInt256::divide(UInt256(5), UInt256(10)) == UInt256::zero());

    // Division by zero is defined to return zero rather than trapping.
    CHECK(UInt256::divide(UInt256(5), UInt256::zero()) == UInt256::zero());
}

TEST("uint256 bit length") {
    CHECK_EQ(UInt256::zero().bitLength(), 0u);
    CHECK_EQ(UInt256(1).bitLength(), 1u);
    CHECK_EQ(UInt256(255).bitLength(), 8u);
    CHECK_EQ(UInt256(256).bitLength(), 9u);
    CHECK_EQ((UInt256(1) << 255).bitLength(), 256u);
}

// --- Compact bits ---------------------------------------------------------

TEST("nBits 0x1d00ffff decodes to the difficulty-1 target") {
    const UInt256 target = targetFromCompactBits(0x1d00ffff);
    CHECK_EQ(target.toHexBigEndian(),
             std::string("00000000ffff0000000000000000000000000000000000000000000000000000"));
    CHECK(target == difficultyOneTarget());
}

TEST("compact bits decoding matches Bitcoin Core's documented vectors") {
    struct Vector {
        uint32_t bits;
        const char* expected;
    };
    // Values from Bitcoin Core's arith_uint256 tests / the nBits documentation.
    const Vector vectors[] = {
        {0x01003456, "0"},
        {0x01123456, "12"},
        {0x02008000, "80"},
        {0x05009234, "92340000"},
        {0x04123456, "12345600"},
        {0x0400ffff, "ffff00"},
        {0x1b0404cb, "404cb000000000000000000000000000000000000000000000000"},
    };

    for (const Vector& vector : vectors) {
        UInt256 target;
        CHECK(decodeCompactBits(vector.bits, target));
        UInt256 expected;
        REQUIRE(UInt256::fromHexBigEndian(vector.expected, expected));
        CHECK_EQ(target.toHexBigEndian(), expected.toHexBigEndian());
    }
}

TEST("compact bits rejects negative mantissas") {
    UInt256 target;
    bool negative = false;
    bool overflow = false;
    CHECK(!decodeCompactBits(0x01803456, target, &negative, &overflow));
    CHECK(negative);
    CHECK(!overflow);
    CHECK(target.isZero());
}

TEST("compact bits rejects overflow") {
    UInt256 target;
    bool negative = false;
    bool overflow = false;
    CHECK(!decodeCompactBits(0xff123456, target, &negative, &overflow));
    CHECK(overflow);
    CHECK(target.isZero());
}

TEST("compact bits encode round-trips for canonical encodings") {
    const uint32_t values[] = {0x1d00ffff, 0x1b0404cb, 0x05009234, 0x04123456,
                               0x0400ffff, 0x02008000, 0x1a05db8b};
    for (uint32_t bits : values) {
        UInt256 target;
        REQUIRE(decodeCompactBits(bits, target));
        CHECK_EQ(encodeCompactBits(target), bits);
    }
}

TEST("non-canonical encodings normalize instead of round-tripping") {
    // When the exponent is <= 3 the low mantissa bytes are shifted out, so
    // several compact values decode to the same target. Re-encoding must
    // produce the canonical form. 0x01123456 decodes to 18, whose canonical
    // encoding is 0x01120000.
    UInt256 target;
    REQUIRE(decodeCompactBits(0x01123456, target));
    CHECK(target == UInt256(18));
    CHECK_EQ(encodeCompactBits(target), 0x01120000u);

    // Both encodings must still decode to the same target.
    UInt256 canonical;
    REQUIRE(decodeCompactBits(0x01120000, canonical));
    CHECK(canonical == target);
}

TEST("encoding renormalizes when the high bit of the mantissa is set") {
    // 0x00800000 would look negative in compact form, so it must be shifted.
    UInt256 value;
    REQUIRE(UInt256::fromHexBigEndian("800000", value));
    const uint32_t bits = encodeCompactBits(value);
    UInt256 decoded;
    REQUIRE(decodeCompactBits(bits, decoded));
    CHECK(decoded == value);
    CHECK((bits & 0x00800000u) == 0u);
}

// --- Hash vs target -------------------------------------------------------

TEST("the genesis hash meets the genesis target") {
    const Hash256 genesis =
        hashFromDisplay("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    CHECK(hashMeetsTarget(genesis, targetFromCompactBits(0x1d00ffff)));
}

TEST("a hash above the target is rejected") {
    // One unit above the difficulty-1 target: the low-order byte decides.
    const Hash256 justOver =
        hashFromDisplay("00000000ffff0000000000000000000000000000000000000000000000000001");
    CHECK(!hashMeetsTarget(justOver, difficultyOneTarget()));

    const Hash256 exactly =
        hashFromDisplay("00000000ffff0000000000000000000000000000000000000000000000000000");
    CHECK(hashMeetsTarget(exactly, difficultyOneTarget()));

    const Hash256 justUnder =
        hashFromDisplay("00000000fffeffffffffffffffffffffffffffffffffffffffffffffffffffff");
    CHECK(hashMeetsTarget(justUnder, difficultyOneTarget()));
}

TEST("a zero target never accepts anything") {
    const Hash256 zeroHash;
    CHECK(!hashMeetsTarget(zeroHash, UInt256::zero()));
}

TEST("hash to number conversion respects internal byte order") {
    // Display hash 0x00..01 means the number 1.
    const Hash256 one =
        hashFromDisplay("0000000000000000000000000000000000000000000000000000000000000001");
    CHECK(UInt256::fromHashLittleEndian(one) == UInt256(1));

    // And the conversion round-trips.
    CHECK(UInt256(1).toHashLittleEndian() == one);
}

// --- Share difficulty -----------------------------------------------------

TEST("share difficulty 1 gives the difficulty-1 target") {
    const UInt256 target = azd::bitcoin::targetFromShareDifficulty(1.0);
    CHECK_EQ(target.toHexBigEndian(), difficultyOneTarget().toHexBigEndian());
}

TEST("higher share difficulty gives a smaller target") {
    const UInt256 d1 = azd::bitcoin::targetFromShareDifficulty(1.0);
    const UInt256 d16 = azd::bitcoin::targetFromShareDifficulty(16.0);
    const UInt256 d1024 = azd::bitcoin::targetFromShareDifficulty(1024.0);

    CHECK(d16 < d1);
    CHECK(d1024 < d16);

    // difficulty 16 target should be exactly difficulty1 / 16.
    CHECK(d16 == UInt256::divide(difficultyOneTarget(), UInt256(16)));
}

TEST("fractional share difficulty is supported") {
    const UInt256 easy = azd::bitcoin::targetFromShareDifficulty(0.001);
    CHECK(easy > difficultyOneTarget());
    CHECK(!easy.isZero());
}

TEST("invalid share difficulties yield a zero target, never a lucky one") {
    CHECK(azd::bitcoin::targetFromShareDifficulty(0.0).isZero());
    CHECK(azd::bitcoin::targetFromShareDifficulty(-5.0).isZero());
}

TEST("share difficulty of a known hash") {
    // A hash exactly equal to the difficulty-1 target has difficulty 1.
    const Hash256 atTarget =
        hashFromDisplay("00000000ffff0000000000000000000000000000000000000000000000000000");
    CHECK(azd::bitcoin::shareDifficultyOfHash(atTarget) == UInt256(1));

    // The genesis block was mined at network difficulty 1, but its hash has
    // several more leading zeros than required, so the difficulty it actually
    // achieved is far higher. The exact integer quotient is 2536.
    const Hash256 genesis =
        hashFromDisplay("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    CHECK(azd::bitcoin::shareDifficultyOfHash(genesis) == UInt256(2536));

    const double approximate = azd::bitcoin::shareDifficultyOfHashApproximate(genesis);
    CHECK(approximate > 2536.0 && approximate < 2537.0);
}

TEST("fixed-point share difficulty ranks shares easier than difficulty 1") {
    // A pool typically hands out difficulties far below 1. The plain integer
    // quotient truncates all of those to 0, so "best difficulty" would never
    // move. The 2^-32 fixed point form must keep them ordered.
    // Both hashes are real shares captured from a local mock-pool session.
    // The smaller number is the better share.
    const Hash256 betterShare =
        hashFromDisplay("0000005fc99b9ae3079f7e7b56cc497d450986d94119a8ac011792563e3987a6");
    const Hash256 weakerShare =
        hashFromDisplay("00000084f3f335e22702977ebb3bf239931054944db1ed0a5b231fd3b02b0a36");

    // Both are below difficulty 1, so the truncating form cannot tell them apart.
    CHECK(azd::bitcoin::shareDifficultyOfHash(betterShare).isZero());
    CHECK(azd::bitcoin::shareDifficultyOfHash(weakerShare).isZero());

    const UInt256 betterFixed = azd::bitcoin::shareDifficultyOfHashFixed32(betterShare);
    const UInt256 weakerFixed = azd::bitcoin::shareDifficultyOfHashFixed32(weakerShare);

    CHECK(!betterFixed.isZero());
    CHECK(!weakerFixed.isZero());
    CHECK(betterFixed > weakerFixed); // the smaller hash is the better share

    // Exact expected value, computed independently: (difficulty1 << 32) / hash.
    CHECK(betterFixed == UInt256(44837796ull));

    // And difficulty 1 itself scales to exactly 2^32.
    const Hash256 atTarget =
        hashFromDisplay("00000000ffff0000000000000000000000000000000000000000000000000000");
    CHECK(azd::bitcoin::shareDifficultyOfHashFixed32(atTarget) == (UInt256(1) << 32));
}

TEST("share difficulty and target are consistent") {
    // If a hash meets the target for difficulty D, its own difficulty is >= D.
    const Hash256 hash =
        hashFromDisplay("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    const UInt256 achieved = azd::bitcoin::shareDifficultyOfHash(hash);

    for (double difficulty : {0.5, 1.0, 2.0}) {
        const UInt256 target = azd::bitcoin::targetFromShareDifficulty(difficulty);
        if (hashMeetsTarget(hash, target)) {
            CHECK(achieved.toDoubleApproximate() >= difficulty * 0.999);
        }
    }
}

TEST("network difficulty of nBits 0x1d00ffff is 1") {
    const double difficulty = azd::bitcoin::networkDifficultyApproximate(0x1d00ffff);
    CHECK(difficulty > 0.999 && difficulty < 1.001);
}

AZD_TEST_MAIN("difficulty")
