#include "core/config.hpp"

#include <fstream>
#include <sstream>

namespace azd::core {
namespace {

uint16_t toPort(const util::Json& value, uint16_t fallback) {
    if (value.isNull()) return fallback;
    const int64_t v = value.asInt(static_cast<int64_t>(fallback));
    if (v <= 0 || v > 65535) return fallback;
    return static_cast<uint16_t>(v);
}

} // namespace

bool Config::loadFromString(const std::string& text, Config& out, std::string* error) {
    util::Json root;
    if (!util::Json::parse(text, root, error)) return false;
    if (!root.isObject()) {
        if (error) *error = "config root must be a JSON object";
        return false;
    }
    Config config;
    config.applyPartialJson(root);
    out = config;
    return true;
}

bool Config::loadFromFile(const std::string& path, Config& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open config file: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return loadFromString(buffer.str(), out, error);
}

void Config::applyPartialJson(const util::Json& patch) {
    if (!patch.isObject()) return;

    const util::Json& m = patch["miner"];
    if (m.isObject()) {
        if (m.has("name")) miner.name = m["name"].asString(miner.name);
        if (m.has("device")) miner.device = static_cast<int>(m["device"].asInt(miner.device));
        if (m.has("cpuThreads")) {
            const int64_t threads = m["cpuThreads"].asInt(miner.cpuThreads);
            miner.cpuThreads = threads < 0 ? 0 : static_cast<int>(threads);
        }
    }

    const util::Json& p = patch["pool"];
    if (p.isObject()) {
        if (p.has("host")) pool.host = p["host"].asString(pool.host);
        if (p.has("port")) pool.port = toPort(p["port"], pool.port);
        if (p.has("username")) pool.username = p["username"].asString(pool.username);
        if (p.has("password")) pool.password = p["password"].asString(pool.password);
    }

    const util::Json& g = patch["gpu"];
    if (g.isObject()) {
        if (g.has("temperatureWarning")) {
            gpu.temperatureWarning = g["temperatureWarning"].asNumber(gpu.temperatureWarning);
        }
        if (g.has("temperatureStop")) {
            gpu.temperatureStop = g["temperatureStop"].asNumber(gpu.temperatureStop);
        }
    }

    const util::Json& d = patch["dashboard"];
    if (d.isObject()) {
        if (d.has("host")) dashboard.host = d["host"].asString(dashboard.host);
        if (d.has("port")) dashboard.port = toPort(d["port"], dashboard.port);
        if (d.has("root")) dashboard.root = d["root"].asString(dashboard.root);
    }
}

util::Json Config::toJson(bool includeSecrets) const {
    util::Json minerJson = util::Json::object();
    minerJson.set("name", miner.name);
    minerJson.set("device", static_cast<int64_t>(miner.device));
    minerJson.set("cpuThreads", static_cast<int64_t>(miner.cpuThreads));

    util::Json poolJson = util::Json::object();
    poolJson.set("host", pool.host);
    poolJson.set("port", static_cast<int64_t>(pool.port));
    poolJson.set("username", pool.username);
    if (includeSecrets) {
        poolJson.set("password", pool.password);
    } else {
        // The dashboard needs to know whether a password is set, never what it
        // is. This flag is the only thing that ever leaves the process.
        poolJson.set("passwordSet", !pool.password.empty());
    }

    util::Json gpuJson = util::Json::object();
    gpuJson.set("temperatureWarning", gpu.temperatureWarning);
    gpuJson.set("temperatureStop", gpu.temperatureStop);

    util::Json dashJson = util::Json::object();
    dashJson.set("host", dashboard.host);
    dashJson.set("port", static_cast<int64_t>(dashboard.port));

    util::Json root = util::Json::object();
    root.set("miner", minerJson);
    root.set("pool", poolJson);
    root.set("gpu", gpuJson);
    root.set("dashboard", dashJson);
    return root;
}

} // namespace azd::core
