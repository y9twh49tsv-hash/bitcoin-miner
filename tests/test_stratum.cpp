// Stratum V1 message handling.
//
// These tests NEVER open a socket. Everything runs against local sample
// messages, so `ctest` never touches a real pool and no share, connection or
// acceptance is ever fabricated.

#include <string>
#include <vector>

#include "bitcoin/block_header.hpp"
#include "stratum/message.hpp"
#include "test_framework.hpp"
#include "util/hex.hpp"

using azd::bitcoin::Hash256;
using azd::stratum::LineBuffer;
using azd::stratum::Message;

namespace {

/// Sample messages in the exact shape a pool sends them.
const char* kSubscribeResponse =
    R"({"id":1,"result":[[["mining.set_difficulty","b4b6693b72a50c7116db18d6497cac52"],)"
    R"(["mining.notify","ae6812eb4cd7735a302a8a9dd95cf71f"]],"08000002",4],"error":null})";

const char* kNotify =
    R"({"params":["bf","4d16b6f85af6e2198f44ae2a6de67f78487ae5611b77c6c0440b921e00000000",)"
    R"("01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff20020862",)"
    R"("072f736c7573682f000000000100f2052a010000001976a914d23fcdf86f7e756a64a7a9688ef9903327048ed988ac00000000",)"
    R"(["58bd1a9d4d5b2ca2a1a4e4c04e3a0a2b6b9f7d0a3e4b5c6d7e8f90a1b2c3d4e5"],)"
    R"("00000002","1c2ac4af","504e86b9",false],"id":null,"method":"mining.notify"})";

const char* kSetDifficulty = R"({"id":null,"method":"mining.set_difficulty","params":[512]})";

const char* kSubmitAccepted = R"({"error":null,"id":4,"result":true})";
const char* kSubmitRejected =
    R"({"error":[23,"Low difficulty share",null],"id":5,"result":false})";

/// The genesis block hash, i.e. the previous-block hash for block 1.
const char* kGenesisDisplay =
    "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";

/// Encode a display-order hash the way a Stratum pool transmits it.
///
/// Derivation: the header field is the byte-reverse of the display form, and a
/// pool sends that field with the bytes of each 4-byte word reversed. Composing
/// the two is equivalent to keeping each 8-hex-character word intact while
/// reversing the ORDER of the eight words. Computed here independently of the
/// production code so the test actually checks the conversion.
std::string displayHashToStratumHex(const std::string& displayHex) {
    REQUIRE(displayHex.size() == 64);
    std::string out;
    for (int word = 7; word >= 0; --word) {
        out += displayHex.substr(static_cast<size_t>(word) * 8, 8);
    }
    return out;
}

} // namespace

// --- LineBuffer: TCP framing ----------------------------------------------

TEST("line buffer splits newline-delimited messages") {
    LineBuffer buffer;
    buffer.append("{\"a\":1}\n{\"b\":2}\n");

    std::string line;
    REQUIRE(buffer.nextLine(line));
    CHECK_EQ(line, std::string("{\"a\":1}"));
    REQUIRE(buffer.nextLine(line));
    CHECK_EQ(line, std::string("{\"b\":2}"));
    CHECK(!buffer.nextLine(line));
}

TEST("line buffer holds a partial message until the rest arrives") {
    LineBuffer buffer;
    std::string line;

    buffer.append("{\"id\":1,\"resu");
    CHECK(!buffer.nextLine(line));   // incomplete: must not be delivered
    CHECK(buffer.pending() > 0);

    buffer.append("lt\":true}");
    CHECK(!buffer.nextLine(line));   // still no terminator

    buffer.append("\n");
    REQUIRE(buffer.nextLine(line));
    CHECK_EQ(line, std::string("{\"id\":1,\"result\":true}"));
    CHECK_EQ(buffer.pending(), size_t(0));
}

TEST("line buffer handles a single read containing several messages plus a fragment") {
    LineBuffer buffer;
    buffer.append(std::string(kSetDifficulty) + "\n" + kSubmitAccepted + "\n{\"partial\":");

    std::string line;
    REQUIRE(buffer.nextLine(line));
    CHECK_EQ(line, std::string(kSetDifficulty));
    REQUIRE(buffer.nextLine(line));
    CHECK_EQ(line, std::string(kSubmitAccepted));
    CHECK(!buffer.nextLine(line));
    CHECK(buffer.pending() > 0);
}

TEST("line buffer tolerates CRLF terminators") {
    LineBuffer buffer;
    buffer.append("{\"a\":1}\r\n");
    std::string line;
    REQUIRE(buffer.nextLine(line));
    CHECK_EQ(line, std::string("{\"a\":1}"));
}

TEST("line buffer feeds byte by byte without losing data") {
    const std::string message = std::string(kSubmitAccepted) + "\n";
    LineBuffer buffer;
    std::string line;

    for (size_t i = 0; i + 1 < message.size(); ++i) {
        buffer.append(message.data() + i, 1);
        CHECK(!buffer.nextLine(line));
    }
    buffer.append(message.data() + message.size() - 1, 1);
    REQUIRE(buffer.nextLine(line));
    CHECK_EQ(line, std::string(kSubmitAccepted));
}

TEST("line buffer refuses an unbounded line") {
    LineBuffer buffer;
    const std::string chunk(64 * 1024, 'x');
    for (int i = 0; i < 20 && !buffer.overflowed(); ++i) buffer.append(chunk);
    CHECK(buffer.overflowed());
}

// --- Message parsing ------------------------------------------------------

TEST("parses a notification") {
    const Message message = Message::parse(kSetDifficulty);
    REQUIRE(message.valid);
    CHECK(message.isNotification());
    CHECK(!message.isResponse());
    CHECK_EQ(message.method, std::string("mining.set_difficulty"));
    CHECK_EQ(message.params.at(0).asNumber(0.0), 512.0);
}

TEST("parses a successful response") {
    const Message message = Message::parse(kSubmitAccepted);
    REQUIRE(message.valid);
    CHECK(message.isResponse());
    CHECK(!message.isError());
    CHECK_EQ(message.id, int64_t(4));
    CHECK(message.result.asBool(false));
}

TEST("parses an error response and its message") {
    const Message message = Message::parse(kSubmitRejected);
    REQUIRE(message.valid);
    CHECK(message.isError());
    CHECK(!message.result.asBool(false));
    CHECK_EQ(message.errorMessage(), std::string("Low difficulty share (code 23)"));
}

TEST("rejects malformed messages instead of throwing") {
    for (const char* bad : {"", "not json", "{\"unterminated\":", "[]", "{}", "12345",
                            "{\"params\":[1,2,3]}"}) {
        const Message message = Message::parse(bad);
        CHECK(!message.valid);
        CHECK(!message.parseError.empty());
    }
}

// --- Request building -----------------------------------------------------

TEST("subscribe request is well formed and newline terminated") {
    const std::string line = azd::stratum::buildSubscribe(1, "azd-bitcoin-miner/0.1");
    CHECK(!line.empty());
    CHECK_EQ(line.back(), '\n');

    const Message message = Message::parse(line.substr(0, line.size() - 1));
    REQUIRE(message.valid);
    CHECK_EQ(message.method, std::string("mining.subscribe"));
    CHECK_EQ(message.id, int64_t(1));
    CHECK_EQ(message.params.at(0).asString(), std::string("azd-bitcoin-miner/0.1"));
}

TEST("authorize request carries username and password in order") {
    const std::string line = azd::stratum::buildAuthorize(2, "wallet.worker1", "s3cret");
    const Message message = Message::parse(line.substr(0, line.size() - 1));
    REQUIRE(message.valid);
    CHECK_EQ(message.method, std::string("mining.authorize"));
    CHECK_EQ(message.params.at(0).asString(), std::string("wallet.worker1"));
    CHECK_EQ(message.params.at(1).asString(), std::string("s3cret"));
}

TEST("submit request has the five stratum parameters") {
    const std::string line =
        azd::stratum::buildSubmit(7, "wallet.worker1", "bf", "00000001", "504e86b9", "b2957c02");
    const Message message = Message::parse(line.substr(0, line.size() - 1));
    REQUIRE(message.valid);
    CHECK_EQ(message.method, std::string("mining.submit"));
    CHECK_EQ(message.params.size(), size_t(5));
    CHECK_EQ(message.params.at(0).asString(), std::string("wallet.worker1"));
    CHECK_EQ(message.params.at(1).asString(), std::string("bf"));
    CHECK_EQ(message.params.at(2).asString(), std::string("00000001"));
    CHECK_EQ(message.params.at(3).asString(), std::string("504e86b9"));
    CHECK_EQ(message.params.at(4).asString(), std::string("b2957c02"));
}

TEST("strings with quotes are escaped rather than breaking the frame") {
    const std::string line = azd::stratum::buildAuthorize(3, "we\"ird\\name", "p\"ass");
    const Message message = Message::parse(line.substr(0, line.size() - 1));
    REQUIRE(message.valid);
    CHECK_EQ(message.params.at(0).asString(), std::string("we\"ird\\name"));
    CHECK_EQ(message.params.at(1).asString(), std::string("p\"ass"));
}

// --- Field conversions ----------------------------------------------------

TEST("big-endian hex fields decode to host integers") {
    uint32_t value = 0;
    REQUIRE(azd::stratum::parseBigEndianHex32("1d00ffff", value));
    CHECK_EQ(value, 0x1d00ffffu);

    REQUIRE(azd::stratum::parseBigEndianHex32("00000001", value));
    CHECK_EQ(value, 1u);

    REQUIRE(azd::stratum::parseBigEndianHex32("4966bc61", value));
    CHECK_EQ(value, 1231469665u); // block 1's timestamp

    CHECK(!azd::stratum::parseBigEndianHex32("1d00ff", value));   // too short
    CHECK(!azd::stratum::parseBigEndianHex32("1d00ffffff", value)); // too long
    CHECK(!azd::stratum::parseBigEndianHex32("zzzzzzzz", value));  // not hex
}

TEST("uint32 to big-endian hex round-trips") {
    CHECK_EQ(azd::stratum::toBigEndianHex32(0x1d00ffffu), std::string("1d00ffff"));
    CHECK_EQ(azd::stratum::toBigEndianHex32(1u), std::string("00000001"));
    CHECK_EQ(azd::stratum::toBigEndianHex32(2083236893u), std::string("7c2bac1d"));
}

TEST("stratum prevhash decodes to the real previous block hash") {
    // A pool mining on top of the genesis block would send its hash in the
    // word-swapped form. Decoding it must produce the header field whose
    // display form is the genesis hash.
    const std::string stratumHex = displayHashToStratumHex(kGenesisDisplay);

    Hash256 headerField;
    REQUIRE(azd::stratum::prevHashFromStratumHex(stratumHex, headerField));

    CHECK_EQ(headerField.toDisplayHex(), std::string(kGenesisDisplay));

    // And the encoding is its own inverse.
    CHECK_EQ(azd::stratum::prevHashToStratumHex(headerField), stratumHex);
}

TEST("stratum prevhash is NOT the display form") {
    // A common bug is to use the pool's hex directly. Guard against it.
    const std::string stratumHex = displayHashToStratumHex(kGenesisDisplay);
    CHECK(stratumHex != std::string(kGenesisDisplay));

    Hash256 wrong;
    REQUIRE(Hash256::fromRawHex(stratumHex, wrong));
    Hash256 right;
    REQUIRE(azd::stratum::prevHashFromStratumHex(stratumHex, right));
    CHECK(wrong != right);
}

TEST("real-world prevhash shape: leading zeros land at the end") {
    // Every real block hash starts with zeros in display form, so a pool's
    // prevhash string ends with "00000000". This is the quickest sanity check
    // on the encoding when reading live pool traffic.
    const std::string stratumHex = displayHashToStratumHex(kGenesisDisplay);
    CHECK_EQ(stratumHex.substr(stratumHex.size() - 8), std::string("00000000"));
}

// --- Payload decoding -----------------------------------------------------

TEST("subscribe result yields extranonce1 and extranonce2_size") {
    const Message message = Message::parse(kSubscribeResponse);
    REQUIRE(message.valid);

    const auto result = azd::stratum::parseSubscribeResult(message.result);
    REQUIRE(result.ok);
    CHECK_EQ(azd::util::toHex(result.extranonce1), std::string("08000002"));
    CHECK_EQ(result.extranonce2Size, size_t(4));
    CHECK_EQ(result.subscriptionId, std::string("ae6812eb4cd7735a302a8a9dd95cf71f"));
}

TEST("subscribe result rejects malformed payloads") {
    azd::util::Json bad;
    REQUIRE(azd::util::Json::parse(R"([[],"nothex",4])", bad));
    CHECK(!azd::stratum::parseSubscribeResult(bad).ok);

    REQUIRE(azd::util::Json::parse(R"([[],"08000002",0])", bad));
    CHECK(!azd::stratum::parseSubscribeResult(bad).ok);

    REQUIRE(azd::util::Json::parse(R"([[],"08000002"])", bad));
    CHECK(!azd::stratum::parseSubscribeResult(bad).ok);
}

TEST("mining.notify decodes into a job") {
    const Message message = Message::parse(kNotify);
    REQUIRE(message.valid);
    CHECK_EQ(message.method, std::string("mining.notify"));

    const auto notify = azd::stratum::parseNotify(message.params);
    REQUIRE(notify.ok);

    const azd::mining::MiningJob& job = notify.job;
    CHECK_EQ(job.jobId, std::string("bf"));
    CHECK_EQ(job.version, 2u);
    CHECK_EQ(job.nBits, 0x1c2ac4afu);
    CHECK_EQ(job.nTime, 0x504e86b9u);
    CHECK(!job.cleanJobs);
    CHECK_EQ(job.merkleBranches.size(), size_t(1));
    CHECK(!job.coinbase1.empty());
    CHECK(!job.coinbase2.empty());

    // The prevhash must have been word-swapped, not copied verbatim.
    Hash256 verbatim;
    REQUIRE(Hash256::fromRawHex("4d16b6f85af6e2198f44ae2a6de67f78487ae5611b77c6c0440b921e00000000",
                                verbatim));
    CHECK(job.previousBlockHash != verbatim);
    CHECK(job.previousBlockHash == azd::bitcoin::swapWordBytes(verbatim));
}

TEST("mining.notify rejects bad payloads instead of mining garbage") {
    azd::util::Json params;

    // Too few parameters.
    REQUIRE(azd::util::Json::parse(R"(["job","00",""])", params));
    CHECK(!azd::stratum::parseNotify(params).ok);

    // Empty job id.
    REQUIRE(azd::util::Json::parse(
        R"(["","4d16b6f85af6e2198f44ae2a6de67f78487ae5611b77c6c0440b921e00000000",)"
        R"("00","00",[],"00000002","1c2ac4af","504e86b9",true])",
        params));
    CHECK(!azd::stratum::parseNotify(params).ok);

    // Short prevhash.
    REQUIRE(azd::util::Json::parse(
        R"(["job","4d16b6f8","00","00",[],"00000002","1c2ac4af","504e86b9",true])", params));
    CHECK(!azd::stratum::parseNotify(params).ok);

    // Non-hex coinbase.
    REQUIRE(azd::util::Json::parse(
        R"(["job","4d16b6f85af6e2198f44ae2a6de67f78487ae5611b77c6c0440b921e00000000",)"
        R"("zz","00",[],"00000002","1c2ac4af","504e86b9",true])",
        params));
    CHECK(!azd::stratum::parseNotify(params).ok);

    // Bad merkle branch entry.
    REQUIRE(azd::util::Json::parse(
        R"(["job","4d16b6f85af6e2198f44ae2a6de67f78487ae5611b77c6c0440b921e00000000",)"
        R"("00","00",["abcd"],"00000002","1c2ac4af","504e86b9",true])",
        params));
    CHECK(!azd::stratum::parseNotify(params).ok);
}

TEST("clean_jobs is carried through") {
    azd::util::Json params;
    REQUIRE(azd::util::Json::parse(
        R"(["job","4d16b6f85af6e2198f44ae2a6de67f78487ae5611b77c6c0440b921e00000000",)"
        R"("00","00",[],"00000002","1c2ac4af","504e86b9",true])",
        params));
    const auto notify = azd::stratum::parseNotify(params);
    REQUIRE(notify.ok);
    CHECK(notify.job.cleanJobs);
}

AZD_TEST_MAIN("stratum")
