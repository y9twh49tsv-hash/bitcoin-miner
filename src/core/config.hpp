#pragma once

#include <cstdint>
#include <string>

#include "util/json.hpp"

namespace azd::core {

struct MinerConfig {
    std::string name = "AZD-MINER";
    int device = 0;
    /// CPU reference-search threads. The CPU path exists for correctness
    /// checking, not for production hashing; 0 means "no CPU workers".
    int cpuThreads = 1;
};

struct PoolConfig {
    std::string host;   // empty => no pool configured, miner cannot start
    uint16_t port = 3333;
    std::string username; // wallet.worker
    std::string password = "x";
};

struct GpuConfig {
    double temperatureWarning = 75.0;
    double temperatureStop = 85.0;
};

struct DashboardConfig {
    std::string host = "127.0.0.1"; // localhost only by default, on purpose
    uint16_t port = 8080;
    /// Directory containing index.html. Empty => resolve relative to the
    /// executable / working directory at startup.
    std::string root;
};

struct Config {
    MinerConfig miner;
    PoolConfig pool;
    GpuConfig gpu;
    DashboardConfig dashboard;

    /// Load from a JSON file. Missing keys keep their defaults. Returns false
    /// (with `error` set) only when the file cannot be read or is not valid
    /// JSON -- a partial config is not an error.
    static bool loadFromFile(const std::string& path, Config& out, std::string* error);
    static bool loadFromString(const std::string& text, Config& out, std::string* error);

    /// Apply a partial JSON object on top of the current values. Used by
    /// PUT /api/config. Unknown keys are ignored.
    void applyPartialJson(const util::Json& patch);

    /// Serialize.
    ///
    /// `includeSecrets` MUST stay false for anything that leaves the process
    /// (API responses, logs). The pool password is only ever written back when
    /// explicitly persisting the config to disk.
    util::Json toJson(bool includeSecrets) const;

    /// True when there is enough information to attempt a real pool connection.
    bool poolConfigured() const { return !pool.host.empty() && pool.port != 0 && !pool.username.empty(); }
};

} // namespace azd::core
