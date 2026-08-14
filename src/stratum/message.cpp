#include "stratum/message.hpp"

#include <cstdio>

#include "util/hex.hpp"

namespace azd::stratum {

// --- LineBuffer -----------------------------------------------------------

void LineBuffer::append(const char* data, size_t len) {
    if (overflowed_) return;
    buffer_.append(data, len);
    if (buffer_.size() > kMaxLineLength && buffer_.find('\n') == std::string::npos) {
        overflowed_ = true;
        buffer_.clear();
    }
}

void LineBuffer::append(const std::string& data) { append(data.data(), data.size()); }

bool LineBuffer::nextLine(std::string& line) {
    const size_t newline = buffer_.find('\n');
    if (newline == std::string::npos) return false;

    line.assign(buffer_, 0, newline);
    buffer_.erase(0, newline + 1);

    // Tolerate CRLF from pools that send it.
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

// --- Message --------------------------------------------------------------

std::string Message::errorMessage() const {
    if (error.isNull()) return std::string();
    if (error.isString()) return error.asString();
    if (error.isArray()) {
        // Standard form: [code, message, traceback]
        const std::string text = error.at(1).asString();
        const int64_t code = error.at(0).asInt(0);
        if (!text.empty()) return text + " (code " + std::to_string(code) + ")";
        return "error code " + std::to_string(code);
    }
    return error.dump();
}

Message Message::parse(const std::string& line) {
    Message message;
    if (line.empty()) {
        message.parseError = "empty line";
        return message;
    }

    util::Json root;
    std::string error;
    if (!util::Json::parse(line, root, &error)) {
        message.parseError = "malformed JSON: " + error;
        return message;
    }
    if (!root.isObject()) {
        message.parseError = "message is not a JSON object";
        return message;
    }

    if (root.has("id") && !root["id"].isNull()) {
        message.hasId = true;
        message.id = root["id"].asInt(0);
    }
    message.method = root["method"].asString();
    message.params = root["params"];
    message.result = root["result"];
    message.error = root["error"];

    if (message.method.empty() && !message.hasId) {
        message.parseError = "message has neither method nor id";
        return message;
    }

    message.valid = true;
    return message;
}

// --- Request builders -----------------------------------------------------

namespace {

std::string wrapRequest(int64_t id, const std::string& method, const std::string& paramsJson) {
    std::string out = "{\"id\":";
    out += std::to_string(id);
    out += ",\"method\":";
    out += util::jsonEscape(method);
    out += ",\"params\":";
    out += paramsJson;
    out += "}\n";
    return out;
}

} // namespace

std::string buildSubscribe(int64_t id, const std::string& userAgent) {
    std::string params = "[";
    params += util::jsonEscape(userAgent);
    params += "]";
    return wrapRequest(id, "mining.subscribe", params);
}

std::string buildAuthorize(int64_t id, const std::string& username, const std::string& password) {
    // NOTE: the password is written to the socket because the protocol requires
    // it. It must never be logged, echoed into an API response, or stored
    // anywhere else. See util::redactSecret.
    std::string params = "[";
    params += util::jsonEscape(username);
    params += ",";
    params += util::jsonEscape(password);
    params += "]";
    return wrapRequest(id, "mining.authorize", params);
}

std::string buildSubmit(int64_t id, const std::string& username, const std::string& jobId,
                        const std::string& extranonce2Hex, const std::string& nTimeHex,
                        const std::string& nonceHex) {
    std::string params = "[";
    params += util::jsonEscape(username);
    params += ",";
    params += util::jsonEscape(jobId);
    params += ",";
    params += util::jsonEscape(extranonce2Hex);
    params += ",";
    params += util::jsonEscape(nTimeHex);
    params += ",";
    params += util::jsonEscape(nonceHex);
    params += "]";
    return wrapRequest(id, "mining.submit", params);
}

std::string buildSuggestDifficulty(int64_t id, double difficulty) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%.8g]", difficulty);
    return wrapRequest(id, "mining.suggest_difficulty", buf);
}

// --- Field decoding -------------------------------------------------------

bool parseBigEndianHex32(const std::string& hex, uint32_t& out) {
    if (hex.size() != 8) return false;
    std::vector<uint8_t> bytes;
    if (!util::fromHex(hex, bytes) || bytes.size() != 4) return false;
    out = (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
          (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
    return true;
}

std::string toBigEndianHex32(uint32_t value) {
    uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    return util::toHex(bytes, 4);
}

bool prevHashFromStratumHex(const std::string& hex, bitcoin::Hash256& out) {
    bitcoin::Hash256 asSent;
    if (!bitcoin::Hash256::fromRawHex(hex, asSent)) return false;
    out = bitcoin::swapWordBytes(asSent);
    return true;
}

std::string prevHashToStratumHex(const bitcoin::Hash256& headerOrder) {
    // The transform is its own inverse.
    return bitcoin::swapWordBytes(headerOrder).toRawHex();
}

SubscribeResult parseSubscribeResult(const util::Json& result) {
    SubscribeResult out;

    // Canonical shape: [[["mining.set_difficulty", id], ["mining.notify", id]],
    //                   extranonce1_hex, extranonce2_size]
    // Some pools flatten the first element; only elements 1 and 2 matter to us.
    if (!result.isArray() || result.size() < 3) {
        out.error = "mining.subscribe result is not a 3-element array";
        return out;
    }

    const std::string extranonce1Hex = result.at(1).asString();
    if (extranonce1Hex.empty()) {
        out.error = "mining.subscribe returned an empty extranonce1";
        return out;
    }
    if (!util::fromHex(extranonce1Hex, out.extranonce1)) {
        out.error = "extranonce1 is not valid hex";
        return out;
    }

    const int64_t size = result.at(2).asInt(-1);
    if (size <= 0 || size > 32) {
        out.error = "extranonce2_size out of range";
        return out;
    }
    out.extranonce2Size = static_cast<size_t>(size);

    // Best-effort: pull out the notify subscription id when present.
    const util::Json& subscriptions = result.at(0);
    if (subscriptions.isArray()) {
        for (size_t i = 0; i < subscriptions.size(); ++i) {
            const util::Json& entry = subscriptions.at(i);
            if (entry.isArray() && entry.size() >= 2 &&
                entry.at(0).asString() == "mining.notify") {
                out.subscriptionId = entry.at(1).asString();
            }
        }
    }

    out.ok = true;
    return out;
}

NotifyResult parseNotify(const util::Json& params) {
    NotifyResult out;

    if (!params.isArray() || params.size() < 9) {
        out.error = "mining.notify expects 9 parameters, got " + std::to_string(params.size());
        return out;
    }

    mining::MiningJob& job = out.job;

    job.jobId = params.at(0).asString();
    if (job.jobId.empty()) {
        out.error = "empty job_id";
        return out;
    }

    if (!prevHashFromStratumHex(params.at(1).asString(), job.previousBlockHash)) {
        out.error = "invalid prevhash";
        return out;
    }

    if (!util::fromHex(params.at(2).asString(), job.coinbase1)) {
        out.error = "invalid coinb1";
        return out;
    }
    if (!util::fromHex(params.at(3).asString(), job.coinbase2)) {
        out.error = "invalid coinb2";
        return out;
    }

    const util::Json& branches = params.at(4);
    if (!branches.isArray()) {
        out.error = "merkle_branch is not an array";
        return out;
    }
    for (size_t i = 0; i < branches.size(); ++i) {
        bitcoin::Hash256 branch;
        if (!bitcoin::Hash256::fromRawHex(branches.at(i).asString(), branch)) {
            out.error = "invalid merkle branch entry " + std::to_string(i);
            return out;
        }
        job.merkleBranches.push_back(branch);
    }

    if (!parseBigEndianHex32(params.at(5).asString(), job.version)) {
        out.error = "invalid version";
        return out;
    }
    if (!parseBigEndianHex32(params.at(6).asString(), job.nBits)) {
        out.error = "invalid nbits";
        return out;
    }
    if (!parseBigEndianHex32(params.at(7).asString(), job.nTime)) {
        out.error = "invalid ntime";
        return out;
    }

    job.cleanJobs = params.at(8).asBool(false);

    out.ok = true;
    return out;
}

} // namespace azd::stratum
