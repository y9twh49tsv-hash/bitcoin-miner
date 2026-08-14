#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mining/work.hpp"
#include "util/json.hpp"

namespace azd::stratum {

/// Stratum V1 is newline-delimited JSON-RPC 2.0-ish over a raw TCP socket.
/// TCP gives no message framing at all, so a single read() can deliver half a
/// message, three messages, or three-and-a-half. LineBuffer is the only place
/// that reassembly happens.
class LineBuffer {
public:
    /// Feed raw bytes straight from the socket.
    void append(const char* data, size_t len);
    void append(const std::string& data);

    /// Pop one complete line (without its terminator). Returns false when only
    /// a partial line is buffered.
    bool nextLine(std::string& line);

    /// Bytes currently held as an incomplete line.
    size_t pending() const { return buffer_.size(); }

    void clear() { buffer_.clear(); }

    /// A hostile or broken pool could stream unbounded data with no newline.
    /// Once the buffer exceeds this, the connection should be dropped.
    static constexpr size_t kMaxLineLength = 1 << 20; // 1 MiB
    bool overflowed() const { return overflowed_; }

private:
    std::string buffer_;
    bool overflowed_ = false;
};

/// A parsed inbound stratum message: either a response to one of our requests
/// (has `id`), or a server-initiated notification (has `method`).
struct Message {
    bool valid = false;
    std::string parseError;

    bool hasId = false;
    int64_t id = 0;

    std::string method;      // set for notifications / requests
    util::Json params;       // array
    util::Json result;       // set for responses
    util::Json error;        // set for responses; null when successful

    bool isNotification() const { return !method.empty(); }
    bool isResponse() const { return method.empty() && hasId; }
    bool isError() const { return !error.isNull(); }

    /// Human-readable error text from the standard [code, message, data] form.
    std::string errorMessage() const;

    static Message parse(const std::string& line);
};

// --- Outbound request builders -------------------------------------------
// Each returns a single line INCLUDING the trailing '\n'.

std::string buildSubscribe(int64_t id, const std::string& userAgent);
std::string buildAuthorize(int64_t id, const std::string& username, const std::string& password);
std::string buildSubmit(int64_t id, const std::string& username, const std::string& jobId,
                        const std::string& extranonce2Hex, const std::string& nTimeHex,
                        const std::string& nonceHex);
std::string buildSuggestDifficulty(int64_t id, double difficulty);

// --- Inbound payload decoding ---------------------------------------------

/// Result of parsing a `mining.subscribe` response.
struct SubscribeResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> extranonce1;
    size_t extranonce2Size = 0;
    std::string subscriptionId;
};

SubscribeResult parseSubscribeResult(const util::Json& result);

/// Decode a `mining.notify` params array into a MiningJob.
///
/// Params order (Stratum V1):
///   [0] job_id            string
///   [1] prevhash          hex, 32 bytes, 4-byte-word swapped (see below)
///   [2] coinb1            hex
///   [3] coinb2            hex
///   [4] merkle_branch     array of hex, internal byte order
///   [5] version           hex, big-endian
///   [6] nbits             hex, big-endian
///   [7] ntime             hex, big-endian
///   [8] clean_jobs        bool
struct NotifyResult {
    bool ok = false;
    std::string error;
    mining::MiningJob job;
};

NotifyResult parseNotify(const util::Json& params);

/// Convert stratum's prevhash hex into header serialization order.
///
/// Pools transmit the previous block hash as eight 32-bit words, each written
/// big-endian, in the SAME word order as the internal little-endian hash. So
/// reversing the bytes *within* each 4-byte word -- and only that -- produces
/// the bytes that belong at offset 4 of the header.
///
/// This is the single most error-prone conversion in Stratum V1. It is
/// isolated here, covered by round-trip tests, and MUST be re-verified against
/// a live pool's first `mining.notify` (compare the decoded value against the
/// current chain tip from a block explorer) before this miner is trusted in
/// production. See README "Known validation gaps".
bool prevHashFromStratumHex(const std::string& hex, bitcoin::Hash256& out);

/// Inverse of the above -- used by tests and by diagnostics.
std::string prevHashToStratumHex(const bitcoin::Hash256& headerOrder);

/// Decode a big-endian hex field ("1d00ffff") into the host uint32 whose
/// little-endian serialization belongs in the header.
bool parseBigEndianHex32(const std::string& hex, uint32_t& out);

/// Encode a uint32 as 8 big-endian hex characters (what mining.submit expects
/// for ntime and nonce).
std::string toBigEndianHex32(uint32_t value);

} // namespace azd::stratum
