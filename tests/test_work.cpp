// Mining work construction (coinbase -> merkle root -> 80-byte header) and the
// CPU reference nonce search.

#include <string>
#include <vector>

#include "bitcoin/block_header.hpp"
#include "bitcoin/difficulty.hpp"
#include "bitcoin/hash.hpp"
#include "bitcoin/merkle.hpp"
#include "mining/miner.hpp"
#include "mining/work.hpp"
#include "test_framework.hpp"
#include "util/hex.hpp"

using azd::bitcoin::Hash256;
using azd::mining::buildCoinbase;
using azd::mining::buildWork;
using azd::mining::CpuReferenceMiner;
using azd::mining::encodeExtranonce2;
using azd::mining::MiningJob;
using azd::mining::MiningWork;

namespace {

std::vector<uint8_t> bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    REQUIRE(azd::util::fromHex(hex, out));
    return out;
}

/// A complete, self-consistent job. The values are arbitrary but structurally
/// exactly what a pool sends.
MiningJob sampleJob() {
    MiningJob job;
    job.jobId = "testjob";
    REQUIRE(Hash256::fromDisplayHex(
        "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",
        job.previousBlockHash));
    job.coinbase1 = bytes("01000000010000000000000000000000000000000000000000000000000000"
                          "000000000000ffffffff20020862");
    job.coinbase2 = bytes("072f736c7573682f000000000100f2052a010000001976a914d23fcdf86f7e"
                          "756a64a7a9688ef9903327048ed988ac00000000");
    job.extranonce1 = bytes("08000002");
    job.extranonce2Size = 4;
    job.version = 2;
    job.nBits = 0x1d00ffff;
    job.nTime = 0x504e86b9;
    job.difficulty = 1.0;

    Hash256 branch;
    REQUIRE(Hash256::fromRawHex(
        "58bd1a9d4d5b2ca2a1a4e4c04e3a0a2b6b9f7d0a3e4b5c6d7e8f90a1b2c3d4e5", branch));
    job.merkleBranches.push_back(branch);
    return job;
}

} // namespace

// --- Coinbase -------------------------------------------------------------

TEST("coinbase is coinb1 + extranonce1 + extranonce2 + coinb2, in that order") {
    const std::vector<uint8_t> coinbase =
        buildCoinbase(bytes("aabb"), bytes("1111"), bytes("2222"), bytes("ccdd"));
    CHECK_EQ(azd::util::toHex(coinbase), std::string("aabb11112222ccdd"));
}

TEST("coinbase length is the sum of its parts") {
    const MiningJob job = sampleJob();
    const std::vector<uint8_t> extranonce2 = encodeExtranonce2(1, job.extranonce2Size);
    const std::vector<uint8_t> coinbase =
        buildCoinbase(job.coinbase1, job.extranonce1, extranonce2, job.coinbase2);
    CHECK_EQ(coinbase.size(), job.coinbase1.size() + job.extranonce1.size() +
                                  extranonce2.size() + job.coinbase2.size());
}

TEST("extranonce2 encoding has the requested width") {
    CHECK_EQ(azd::util::toHex(encodeExtranonce2(0, 4)), std::string("00000000"));
    CHECK_EQ(azd::util::toHex(encodeExtranonce2(1, 4)), std::string("00000001"));
    CHECK_EQ(azd::util::toHex(encodeExtranonce2(258, 4)), std::string("00000102"));
    CHECK_EQ(azd::util::toHex(encodeExtranonce2(1, 8)), std::string("0000000000000001"));
    CHECK_EQ(azd::util::toHex(encodeExtranonce2(255, 2)), std::string("00ff"));
    CHECK_EQ(encodeExtranonce2(5, 3).size(), size_t(3));
}

TEST("different extranonce2 values produce different coinbases and roots") {
    const MiningJob job = sampleJob();
    const MiningWork first = buildWork(job, 0);
    const MiningWork second = buildWork(job, 1);

    REQUIRE(first.valid);
    REQUIRE(second.valid);
    CHECK(first.coinbaseHash != second.coinbaseHash);
    CHECK(first.merkleRoot != second.merkleRoot);
    CHECK(first.headerBytes != second.headerBytes);
}

// --- Work construction ----------------------------------------------------

TEST("work construction follows the documented pipeline") {
    const MiningJob job = sampleJob();
    const MiningWork work = buildWork(job, 7);
    REQUIRE(work.valid);

    // Recompute every step independently.
    const std::vector<uint8_t> extranonce2 = encodeExtranonce2(7, job.extranonce2Size);
    const std::vector<uint8_t> coinbase =
        buildCoinbase(job.coinbase1, job.extranonce1, extranonce2, job.coinbase2);
    const Hash256 coinbaseHash = azd::bitcoin::doubleSha256(coinbase);
    const Hash256 merkleRoot =
        azd::bitcoin::merkleRootFromBranch(coinbaseHash, job.merkleBranches);

    CHECK_EQ(work.coinbaseHash.toDisplayHex(), coinbaseHash.toDisplayHex());
    CHECK_EQ(work.merkleRoot.toDisplayHex(), merkleRoot.toDisplayHex());
    CHECK_EQ(work.extranonce2, extranonce2);
}

TEST("the header carries the job's fields at the right offsets") {
    const MiningJob job = sampleJob();
    const MiningWork work = buildWork(job, 0);
    REQUIRE(work.valid);

    CHECK_EQ(work.headerBytes.size(), size_t(80));
    CHECK_EQ(azd::bitcoin::readLE32(work.headerBytes.data() + 0), job.version);
    CHECK_EQ(azd::bitcoin::readLE32(work.headerBytes.data() + 68), job.nTime);
    CHECK_EQ(azd::bitcoin::readLE32(work.headerBytes.data() + 72), job.nBits);
    CHECK_EQ(azd::bitcoin::readLE32(work.headerBytes.data() + 76), 0u); // nonce starts at 0

    // prev hash and merkle root sit in internal byte order.
    CHECK_EQ(azd::util::toHex(work.headerBytes.data() + 4, 32), job.previousBlockHash.toRawHex());
    CHECK_EQ(azd::util::toHex(work.headerBytes.data() + 36, 32), work.merkleRoot.toRawHex());
}

TEST("targets are derived from the pool difficulty and from nBits") {
    MiningJob job = sampleJob();
    job.difficulty = 16.0;
    const MiningWork work = buildWork(job, 0);
    REQUIRE(work.valid);

    CHECK(work.networkTarget == azd::bitcoin::targetFromCompactBits(job.nBits));
    CHECK(work.shareTarget == azd::bitcoin::targetFromShareDifficulty(16.0));
    CHECK(work.shareTarget < work.networkTarget); // difficulty 16 is harder than difficulty 1
}

TEST("a job without a difficulty falls back to the network target, never to a guess") {
    MiningJob job = sampleJob();
    job.difficulty = 0.0;
    const MiningWork work = buildWork(job, 0);
    REQUIRE(work.valid);
    CHECK(work.shareTarget == work.networkTarget);
    CHECK_EQ(work.shareDifficulty, 0.0);
}

TEST("incomplete jobs produce invalid work rather than something mineable") {
    MiningJob job = sampleJob();
    job.jobId.clear();
    CHECK(!buildWork(job, 0).valid);

    job = sampleJob();
    job.coinbase1.clear();
    CHECK(!buildWork(job, 0).valid);

    job = sampleJob();
    job.extranonce2Size = 0;
    CHECK(!buildWork(job, 0).valid);

    // extranonce2 of the wrong width must be refused.
    job = sampleJob();
    CHECK(!azd::mining::buildWorkWithExtranonce2(job, bytes("0001")).valid);
}

TEST("hashing at a nonce only changes the nonce bytes") {
    const MiningJob job = sampleJob();
    const MiningWork work = buildWork(job, 0);
    REQUIRE(work.valid);

    const Hash256 a = azd::mining::hashWorkAtNonce(work, 0);
    const Hash256 b = azd::mining::hashWorkAtNonce(work, 1);
    CHECK(a != b);

    // Recompute manually.
    auto header = work.headerBytes;
    azd::bitcoin::setHeaderNonce(header.data(), 12345);
    CHECK_EQ(azd::mining::hashWorkAtNonce(work, 12345).toDisplayHex(),
             azd::bitcoin::doubleSha256(header.data(), header.size()).toDisplayHex());

    // The work object itself must not have been mutated.
    CHECK_EQ(azd::bitcoin::readLE32(work.headerBytes.data() + 76), 0u);
}

// --- CPU reference miner --------------------------------------------------

TEST("CPU miner finds a nonce that genuinely meets an easy target") {
    MiningJob job = sampleJob();
    // A very easy share target so the search terminates quickly, while every
    // hash along the way is real.
    job.difficulty = 0.00001;
    const MiningWork work = buildWork(job, 0);
    REQUIRE(work.valid);

    CpuReferenceMiner miner;
    const auto result = miner.mine(work, 0, 5000000);

    REQUIRE(result.found);
    CHECK(!result.error);

    // Independently verify the reported nonce.
    const Hash256 hash = azd::mining::hashWorkAtNonce(work, result.nonce);
    CHECK_EQ(hash.toDisplayHex(), result.hash.toDisplayHex());
    CHECK(azd::bitcoin::hashMeetsTarget(hash, work.shareTarget));
}

TEST("CPU miner reports real hash counts and real elapsed time") {
    MiningJob job = sampleJob();
    job.difficulty = 1e12; // effectively unfindable in this range
    const MiningWork work = buildWork(job, 0);
    REQUIRE(work.valid);

    CpuReferenceMiner miner;
    const auto result = miner.mine(work, 0, 20000);

    CHECK(!result.found);
    CHECK_EQ(result.hashesPerformed, uint64_t(20000)); // exactly what was asked for
    CHECK(result.elapsedSeconds > 0.0);
    CHECK(result.hashesPerSecond() > 0.0);
}

TEST("an empty range performs no hashes and claims none") {
    const MiningWork work = buildWork(sampleJob(), 0);
    CpuReferenceMiner miner;
    const auto result = miner.mine(work, 0, 0);
    CHECK_EQ(result.hashesPerformed, uint64_t(0));
    CHECK(!result.found);
    CHECK_EQ(result.hashesPerSecond(), 0.0);
}

TEST("invalid work is refused rather than hashed") {
    MiningWork work;
    work.valid = false;
    CpuReferenceMiner miner;
    const auto result = miner.mine(work, 0, 1000);
    CHECK(result.error);
    CHECK_EQ(result.hashesPerformed, uint64_t(0));
}

TEST("the stop flag halts the search") {
    MiningJob job = sampleJob();
    job.difficulty = 1e12;
    const MiningWork work = buildWork(job, 0);

    std::atomic<bool> stop{true};
    CpuReferenceMiner miner;
    const auto result = miner.mine(work, 0, 100000000, &stop);

    CHECK(!result.found);
    CHECK(result.hashesPerformed < 100000000ull);
}

TEST("verify() is the oracle: it confirms good nonces and rejects bad ones") {
    MiningJob job = sampleJob();
    job.difficulty = 0.00001;
    const MiningWork work = buildWork(job, 0);

    CpuReferenceMiner miner;
    const auto result = miner.mine(work, 0, 5000000);
    REQUIRE(result.found);

    Hash256 hash;
    bool meetsShare = false;
    bool meetsNetwork = false;
    CHECK(CpuReferenceMiner::verify(work, result.nonce, hash, meetsShare, meetsNetwork));
    CHECK(meetsShare);

    // The nonce immediately before the winner is (overwhelmingly likely) not a
    // share; verify must say so rather than rubber-stamping it.
    if (result.nonce > 0) {
        Hash256 otherHash;
        bool otherShare = true;
        bool otherNetwork = true;
        CpuReferenceMiner::verify(work, result.nonce - 1, otherHash, otherShare, otherNetwork);
        CHECK(!otherShare);
    }
}

TEST("the genesis nonce is recoverable by the reference search") {
    // The strongest possible end-to-end check: search a real header for a real
    // nonce at the real network target.
    MiningWork work;
    work.jobId = "genesis";
    work.header.version = 1;
    REQUIRE(Hash256::fromDisplayHex(
        "0000000000000000000000000000000000000000000000000000000000000000",
        work.header.previousBlockHash));
    REQUIRE(Hash256::fromDisplayHex(
        "4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b", work.header.merkleRoot));
    work.header.timestamp = 1231006505;
    work.header.bits = 0x1d00ffff;
    work.header.nonce = 0;
    work.header.serializeInto(work.headerBytes.data());
    work.networkTarget = azd::bitcoin::targetFromCompactBits(0x1d00ffff);
    work.shareTarget = work.networkTarget;
    work.valid = true;

    const uint32_t knownNonce = 2083236893;
    CpuReferenceMiner miner;
    const auto result = miner.mine(work, knownNonce - 20000, 40000);

    REQUIRE(result.found);
    CHECK_EQ(result.nonce, knownNonce);
    CHECK(result.meetsNetworkTarget);
    CHECK_EQ(result.hash.toDisplayHex(),
             std::string("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f"));
}

AZD_TEST_MAIN("work")
