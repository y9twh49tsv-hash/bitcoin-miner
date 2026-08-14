#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bitcoin/block_header.hpp"
#include "bitcoin/hash.hpp"
#include "bitcoin/uint256.hpp"

namespace azd::mining {

/// A job exactly as a pool describes it in `mining.notify`, already decoded
/// into binary and normalized into header byte order.
///
/// Nothing in here is invented locally: every field comes from the pool.
struct MiningJob {
    std::string jobId;

    /// Previous block hash in *header serialization order*, i.e. ready to be
    /// copied straight into bytes 4..36 of the header. The stratum decoding
    /// (including the 4-byte word swap) happens in stratum/message.cpp.
    bitcoin::Hash256 previousBlockHash;

    /// Coinbase transaction, split by the pool at the extranonce insertion
    /// point: coinbase = coinb1 || extranonce1 || extranonce2 || coinb2
    std::vector<uint8_t> coinbase1;
    std::vector<uint8_t> coinbase2;

    /// Merkle branch for the coinbase leaf, internal byte order.
    std::vector<bitcoin::Hash256> merkleBranches;

    uint32_t version = 0;
    uint32_t nBits = 0;
    uint32_t nTime = 0;

    bool cleanJobs = false;

    /// Extranonce state supplied by mining.subscribe / mining.set_extranonce.
    std::vector<uint8_t> extranonce1;
    size_t extranonce2Size = 0;

    /// Share difficulty in force when this job was received (mining.set_difficulty).
    double difficulty = 0.0;

    bool valid() const {
        return !jobId.empty() && !coinbase1.empty() && extranonce2Size > 0;
    }
};

/// A concrete, hashable unit of work: one job plus one chosen extranonce2.
///
/// The 80-byte header is fully built here; a nonce search only rewrites bytes
/// 76..80. This is the exact object a CUDA kernel will later receive.
struct MiningWork {
    std::string jobId;
    std::vector<uint8_t> extranonce2;

    bitcoin::BlockHeader header;
    std::array<uint8_t, bitcoin::BlockHeader::kSerializedSize> headerBytes{};

    /// Coinbase transaction actually used, and its SHA256d.
    bitcoin::Hash256 coinbaseHash;
    bitcoin::Hash256 merkleRoot;

    /// Share target derived from the pool difficulty. A hash <= this target is
    /// a share worth submitting.
    bitcoin::UInt256 shareTarget;
    double shareDifficulty = 0.0;

    /// Network target derived from nBits. A hash <= this is an actual block.
    bitcoin::UInt256 networkTarget;

    bool valid = false;

    uint32_t nTime() const { return header.timestamp; }
    uint32_t version() const { return header.version; }
    uint32_t nBits() const { return header.bits; }
};

/// coinbase = coinb1 || extranonce1 || extranonce2 || coinb2
std::vector<uint8_t> buildCoinbase(const std::vector<uint8_t>& coinbase1,
                                   const std::vector<uint8_t>& extranonce1,
                                   const std::vector<uint8_t>& extranonce2,
                                   const std::vector<uint8_t>& coinbase2);

/// Encode a counter into `size` bytes, big-endian.
///
/// Extranonce2 is an opaque byte string as far as the pool is concerned -- the
/// only hard requirement is that the bytes we hash are the bytes we submit.
/// Big-endian is used so that consecutive counters differ in the last byte,
/// which makes debugging pool traffic much easier to read.
std::vector<uint8_t> encodeExtranonce2(uint64_t counter, size_t size);

/// Build a full work item from a job and an extranonce2 counter.
/// Returns a MiningWork with `valid == false` if the job is incomplete.
MiningWork buildWork(const MiningJob& job, uint64_t extranonce2Counter);

/// Same, with explicit extranonce2 bytes (used by tests and by resubmission).
MiningWork buildWorkWithExtranonce2(const MiningJob& job,
                                    const std::vector<uint8_t>& extranonce2);

/// Hash a work item at a specific nonce. Correctness reference used by both
/// the CPU search and (later) the CUDA verification path.
bitcoin::Hash256 hashWorkAtNonce(const MiningWork& work, uint32_t nonce);

} // namespace azd::mining
