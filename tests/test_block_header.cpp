// Block header serialization and the Bitcoin genesis block.
//
// The genesis test is the project's gate: 80-byte layout, endianness, SHA-256d
// and display-order conversion all have to be simultaneously correct to
// reproduce the hash everyone knows.

#include <array>
#include <string>

#include "bitcoin/block_header.hpp"
#include "bitcoin/difficulty.hpp"
#include "bitcoin/hash.hpp"
#include "test_framework.hpp"
#include "util/hex.hpp"

using azd::bitcoin::BlockHeader;
using azd::bitcoin::Hash256;

namespace {

// Bitcoin genesis block (block 0), mined 2009-01-03.
constexpr uint32_t kGenesisVersion = 1;
constexpr const char* kGenesisPrevHashDisplay =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kGenesisMerkleRootDisplay =
    "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b";
constexpr uint32_t kGenesisTimestamp = 1231006505; // 2009-01-03 18:15:05 UTC
constexpr uint32_t kGenesisBits = 0x1d00ffff;
constexpr uint32_t kGenesisNonce = 2083236893;

constexpr const char* kGenesisHashDisplay =
    "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";

// The canonical serialized genesis header, as it appears at the start of
// blk00000.dat.
constexpr const char* kGenesisHeaderHex =
    "01000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
    "29ab5f49"
    "ffff001d"
    "1dac2b7c";

BlockHeader genesisHeader() {
    BlockHeader header;
    header.version = kGenesisVersion;
    REQUIRE(Hash256::fromDisplayHex(kGenesisPrevHashDisplay, header.previousBlockHash));
    REQUIRE(Hash256::fromDisplayHex(kGenesisMerkleRootDisplay, header.merkleRoot));
    header.timestamp = kGenesisTimestamp;
    header.bits = kGenesisBits;
    header.nonce = kGenesisNonce;
    return header;
}

} // namespace

TEST("header serializes to exactly 80 bytes") {
    const auto serialized = genesisHeader().serialize();
    CHECK_EQ(serialized.size(), size_t(80));
}

TEST("genesis header serialization matches the canonical bytes") {
    const auto serialized = genesisHeader().serialize();
    CHECK_EQ(azd::util::toHex(serialized.data(), serialized.size()),
             std::string(kGenesisHeaderHex));
}

TEST("GENESIS BLOCK: SHA256d of the header reproduces the known block hash") {
    const BlockHeader header = genesisHeader();
    CHECK_EQ(header.displayHash(), std::string(kGenesisHashDisplay));
}

TEST("genesis hash satisfies the genesis target") {
    const BlockHeader header = genesisHeader();
    const auto target = azd::bitcoin::targetFromCompactBits(header.bits);
    CHECK(azd::bitcoin::hashMeetsTarget(header.hash(), target));
}

TEST("changing the nonce changes the hash") {
    BlockHeader header = genesisHeader();
    const Hash256 original = header.hash();
    header.nonce = kGenesisNonce + 1;
    CHECK(header.hash() != original);
    CHECK(!azd::bitcoin::hashMeetsTarget(header.hash(),
                                         azd::bitcoin::targetFromCompactBits(header.bits)));
}

TEST("field offsets are little-endian at the documented positions") {
    const auto serialized = genesisHeader().serialize();
    CHECK_EQ(azd::bitcoin::readLE32(serialized.data() + 0), kGenesisVersion);
    CHECK_EQ(azd::bitcoin::readLE32(serialized.data() + 68), kGenesisTimestamp);
    CHECK_EQ(azd::bitcoin::readLE32(serialized.data() + 72), kGenesisBits);
    CHECK_EQ(azd::bitcoin::readLE32(serialized.data() + 76), kGenesisNonce);
}

TEST("previous hash and merkle root are stored in internal byte order") {
    const auto serialized = genesisHeader().serialize();
    // The merkle root at offset 36 is the byte-reverse of the displayed value.
    const std::string rawMerkle = azd::util::toHex(serialized.data() + 36, 32);
    CHECK_EQ(rawMerkle,
             std::string("3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"));

    Hash256 merkle;
    REQUIRE(Hash256::fromDisplayHex(kGenesisMerkleRootDisplay, merkle));
    CHECK_EQ(merkle.toRawHex(), rawMerkle);
    CHECK_EQ(merkle.toDisplayHex(), std::string(kGenesisMerkleRootDisplay));
}

TEST("deserialize round-trips the genesis header") {
    const auto serialized = genesisHeader().serialize();
    BlockHeader parsed;
    REQUIRE(BlockHeader::deserialize(serialized.data(), serialized.size(), parsed));

    CHECK_EQ(parsed.version, kGenesisVersion);
    CHECK_EQ(parsed.timestamp, kGenesisTimestamp);
    CHECK_EQ(parsed.bits, kGenesisBits);
    CHECK_EQ(parsed.nonce, kGenesisNonce);
    CHECK_EQ(parsed.displayHash(), std::string(kGenesisHashDisplay));
}

TEST("deserialize rejects wrong lengths") {
    const auto serialized = genesisHeader().serialize();
    BlockHeader parsed;
    CHECK(!BlockHeader::deserialize(serialized.data(), 79, parsed));
    CHECK(!BlockHeader::deserialize(serialized.data(), 81, parsed));
    CHECK(!BlockHeader::deserialize(serialized.data(), 0, parsed));
}

TEST("setHeaderNonce patches the serialized buffer in place") {
    auto serialized = genesisHeader().serialize();
    azd::bitcoin::setHeaderNonce(serialized.data(), 0xDEADBEEFu);
    CHECK_EQ(azd::bitcoin::readLE32(serialized.data() + 76), 0xDEADBEEFu);

    // Restoring the real nonce must restore the real hash.
    azd::bitcoin::setHeaderNonce(serialized.data(), kGenesisNonce);
    const Hash256 hash = azd::bitcoin::doubleSha256(serialized.data(), serialized.size());
    CHECK_EQ(hash.toDisplayHex(), std::string(kGenesisHashDisplay));
}

TEST("byteSwap32 handles stratum-style big-endian fields") {
    CHECK_EQ(azd::bitcoin::byteSwap32(0x01000000u), 0x00000001u);
    CHECK_EQ(azd::bitcoin::byteSwap32(0x1d00ffffu), 0xffff001du);
    CHECK_EQ(azd::bitcoin::byteSwap32(azd::bitcoin::byteSwap32(0x12345678u)), 0x12345678u);
}

// A second real block, to prove the genesis result was not a lucky constant.
// Block 1 of the Bitcoin main chain.
TEST("block 1 header reproduces its known hash") {
    BlockHeader header;
    header.version = 1;
    REQUIRE(Hash256::fromDisplayHex(kGenesisHashDisplay, header.previousBlockHash));
    REQUIRE(Hash256::fromDisplayHex(
        "0e3e2357e806b6cdb1f70b54c3a3a17b6714ee1f0e68bebb44a74b1efd512098", header.merkleRoot));
    header.timestamp = 1231469665;
    header.bits = 0x1d00ffff;
    header.nonce = 2573394689u;

    CHECK_EQ(header.displayHash(),
             std::string("00000000839a8e6886ab5951d76f411475428afc90947ee320161bbf18eb6048"));
}

AZD_TEST_MAIN("block_header")
