#include "mining/work.hpp"

#include "bitcoin/difficulty.hpp"
#include "bitcoin/merkle.hpp"
#include "bitcoin/sha256.hpp"

namespace azd::mining {

std::vector<uint8_t> buildCoinbase(const std::vector<uint8_t>& coinbase1,
                                   const std::vector<uint8_t>& extranonce1,
                                   const std::vector<uint8_t>& extranonce2,
                                   const std::vector<uint8_t>& coinbase2) {
    std::vector<uint8_t> coinbase;
    coinbase.reserve(coinbase1.size() + extranonce1.size() + extranonce2.size() +
                     coinbase2.size());
    coinbase.insert(coinbase.end(), coinbase1.begin(), coinbase1.end());
    coinbase.insert(coinbase.end(), extranonce1.begin(), extranonce1.end());
    coinbase.insert(coinbase.end(), extranonce2.begin(), extranonce2.end());
    coinbase.insert(coinbase.end(), coinbase2.begin(), coinbase2.end());
    return coinbase;
}

std::vector<uint8_t> encodeExtranonce2(uint64_t counter, size_t size) {
    std::vector<uint8_t> out(size, 0);
    for (size_t i = 0; i < size; ++i) {
        // Big-endian: least significant byte last.
        const size_t shift = 8 * i;
        if (shift >= 64) break;
        out[size - 1 - i] = static_cast<uint8_t>((counter >> shift) & 0xFFu);
    }
    return out;
}

MiningWork buildWorkWithExtranonce2(const MiningJob& job,
                                    const std::vector<uint8_t>& extranonce2) {
    MiningWork work;
    work.jobId = job.jobId;
    work.extranonce2 = extranonce2;

    if (!job.valid() || extranonce2.size() != job.extranonce2Size) {
        work.valid = false;
        return work;
    }

    // 1. Assemble the coinbase transaction the pool asked us to use.
    const std::vector<uint8_t> coinbase =
        buildCoinbase(job.coinbase1, job.extranonce1, extranonce2, job.coinbase2);

    // 2. SHA256d(coinbase) -- this is the leftmost Merkle leaf.
    work.coinbaseHash = bitcoin::doubleSha256(coinbase);

    // 3. Fold the pool's Merkle branch to get the root.
    work.merkleRoot = bitcoin::merkleRootFromBranch(work.coinbaseHash, job.merkleBranches);

    // 4. Build the 80-byte header.
    work.header.version = job.version;
    work.header.previousBlockHash = job.previousBlockHash;
    work.header.merkleRoot = work.merkleRoot;
    work.header.timestamp = job.nTime;
    work.header.bits = job.nBits;
    work.header.nonce = 0;
    work.header.serializeInto(work.headerBytes.data());

    // 5. Targets. The share target comes from the pool difficulty; the network
    //    target comes from nBits. Both are exact 256-bit integers.
    work.shareDifficulty = job.difficulty;
    work.shareTarget = bitcoin::targetFromShareDifficulty(job.difficulty);
    work.networkTarget = bitcoin::targetFromCompactBits(job.nBits);

    // A pool that has not sent mining.set_difficulty yet leaves us without a
    // share target. Fall back to the network target so nothing is ever
    // submitted that would not also be a block -- never invent a difficulty.
    if (work.shareTarget.isZero()) {
        work.shareTarget = work.networkTarget;
    }

    work.valid = !work.networkTarget.isZero();
    return work;
}

MiningWork buildWork(const MiningJob& job, uint64_t extranonce2Counter) {
    return buildWorkWithExtranonce2(job,
                                    encodeExtranonce2(extranonce2Counter, job.extranonce2Size));
}

bitcoin::Hash256 hashWorkAtNonce(const MiningWork& work, uint32_t nonce) {
    std::array<uint8_t, bitcoin::BlockHeader::kSerializedSize> buffer = work.headerBytes;
    bitcoin::setHeaderNonce(buffer.data(), nonce);
    return bitcoin::doubleSha256(buffer.data(), buffer.size());
}

} // namespace azd::mining
