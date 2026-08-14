// Local API routing, configuration handling and the CUDA stub's behaviour.
//
// Routing is tested directly through handleRequest(), so no socket is opened
// and no port is bound during ctest.
//
// The most important test in this file is the one asserting that a pool
// password never appears in any GET response.

#include <string>

#include "api/server.hpp"
#include "core/config.hpp"
#include "cuda/cuda_backend.hpp"
#include "mining/miner.hpp"
#include "test_framework.hpp"
#include "util/json.hpp"

using azd::api::ApiServer;
using azd::api::HttpRequest;
using azd::api::HttpResponse;
using azd::core::Config;

namespace {

const char* kSecretPassword = "SuperSecretPoolPassword12345";

Config configWithSecret() {
    Config config;
    config.miner.name = "AZD-TEST";
    config.pool.host = "pool.example.com";
    config.pool.port = 3333;
    config.pool.username = "wallet.worker";
    config.pool.password = kSecretPassword;
    return config;
}

HttpRequest get(const std::string& path) {
    HttpRequest request;
    request.method = "GET";
    request.path = path;
    return request;
}

HttpRequest makeRequest(const std::string& method, const std::string& path,
                        const std::string& body = std::string()) {
    HttpRequest request;
    request.method = method;
    request.path = path;
    request.body = body;
    return request;
}

} // namespace

// --- HTTP parsing ---------------------------------------------------------

TEST("parses a GET request") {
    const std::string raw =
        "GET /api/status?verbose=1 HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAccept: */*\r\n\r\n";
    HttpRequest request;
    REQUIRE(azd::api::parseHttpRequest(raw, request));
    CHECK_EQ(request.method, std::string("GET"));
    CHECK_EQ(request.path, std::string("/api/status"));
    CHECK_EQ(request.query, std::string("verbose=1"));
    CHECK_EQ(request.headers["host"], std::string("127.0.0.1:8080"));
}

TEST("parses a request with a body") {
    const std::string raw =
        "PUT /api/config HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 9\r\n\r\n"
        "{\"a\":123}";
    HttpRequest request;
    REQUIRE(azd::api::parseHttpRequest(raw, request));
    CHECK_EQ(request.method, std::string("PUT"));
    CHECK_EQ(request.body, std::string("{\"a\":123}"));
}

TEST("rejects an incomplete request") {
    HttpRequest request;
    CHECK(!azd::api::parseHttpRequest("GET /api/status HTTP/1.1\r\nHost: x", request));
    CHECK(!azd::api::parseHttpRequest("", request));
}

TEST("static path traversal is refused") {
    CHECK(azd::api::isSafeStaticPath("/"));
    CHECK(azd::api::isSafeStaticPath("/index.html"));
    CHECK(azd::api::isSafeStaticPath("/app.js"));

    CHECK(!azd::api::isSafeStaticPath("/../etc/passwd"));
    CHECK(!azd::api::isSafeStaticPath("/../../secret"));
    CHECK(!azd::api::isSafeStaticPath("/dir/../../x"));
    CHECK(!azd::api::isSafeStaticPath("\\windows\\system32"));
    CHECK(!azd::api::isSafeStaticPath("relative.html"));
    CHECK(!azd::api::isSafeStaticPath(""));
}

// --- Routing --------------------------------------------------------------

TEST("read-only endpoints return JSON") {
    azd::mining::MinerController miner(configWithSecret());
    ApiServer server(miner, "127.0.0.1", 0, "");

    for (const char* path : {"/api/status", "/api/gpu", "/api/pool", "/api/stats", "/api/config"}) {
        const HttpResponse response = server.handleRequest(get(path));
        CHECK_EQ(response.status, 200);
        CHECK_EQ(response.contentType, std::string("application/json"));

        azd::util::Json parsed;
        std::string error;
        CHECK(azd::util::Json::parse(response.body, parsed, &error));
    }
}

TEST("THE PASSWORD NEVER APPEARS IN ANY GET RESPONSE") {
    azd::mining::MinerController miner(configWithSecret());
    ApiServer server(miner, "127.0.0.1", 0, "");

    for (const char* path : {"/api/status", "/api/gpu", "/api/pool", "/api/stats", "/api/config"}) {
        const HttpResponse response = server.handleRequest(get(path));
        CHECK(response.body.find(kSecretPassword) == std::string::npos);
    }

    // The config serializer itself must also refuse to emit it.
    const Config config = configWithSecret();
    CHECK(config.toJson(false).dump().find(kSecretPassword) == std::string::npos);
    // ... but it must still say whether one is configured.
    CHECK(config.toJson(false)["pool"]["passwordSet"].asBool(false));

    // Only the explicit persist path includes it.
    CHECK(config.toJson(true).dump().find(kSecretPassword) != std::string::npos);
}

TEST("pool endpoint reports state without credentials") {
    azd::mining::MinerController miner(configWithSecret());
    ApiServer server(miner, "127.0.0.1", 0, "");

    const HttpResponse response = server.handleRequest(get("/api/pool"));
    azd::util::Json json;
    REQUIRE(azd::util::Json::parse(response.body, json));

    CHECK_EQ(json["host"].asString(), std::string("pool.example.com"));
    CHECK_EQ(json["username"].asString(), std::string("wallet.worker"));
    CHECK(json["passwordSet"].asBool(false));
    CHECK(!json.has("password"));
    CHECK_EQ(json["state"].asString(), std::string("disconnected"));
    // Nothing has happened yet, so every counter is zero.
    CHECK_EQ(json["acceptedShares"].asInt(-1), int64_t(0));
    CHECK_EQ(json["rejectedShares"].asInt(-1), int64_t(0));
}

TEST("wrong methods are refused") {
    azd::mining::MinerController miner(Config{});
    ApiServer server(miner, "127.0.0.1", 0, "");

    CHECK_EQ(server.handleRequest(makeRequest("POST", "/api/status")).status, 405);
    CHECK_EQ(server.handleRequest(makeRequest("GET", "/api/miner/start")).status, 405);
    CHECK_EQ(server.handleRequest(makeRequest("GET", "/api/miner/stop")).status, 405);
    CHECK_EQ(server.handleRequest(makeRequest("DELETE", "/api/config")).status, 405);
}

TEST("unknown API endpoints return 404") {
    azd::mining::MinerController miner(Config{});
    ApiServer server(miner, "127.0.0.1", 0, "");
    CHECK_EQ(server.handleRequest(get("/api/nope")).status, 404);
}

TEST("starting without a configured pool fails loudly and does not mine") {
    azd::mining::MinerController miner(Config{}); // no pool host
    ApiServer server(miner, "127.0.0.1", 0, "");

    const HttpResponse response = server.handleRequest(makeRequest("POST", "/api/miner/start"));
    CHECK_EQ(response.status, 409);
    CHECK(!miner.isRunning());

    azd::util::Json json;
    REQUIRE(azd::util::Json::parse(response.body, json));
    CHECK(json["error"].asString().find("no pool configured") != std::string::npos);

    // And the statistics must still be all zeros: a failed start hashes nothing.
    CHECK_EQ(miner.statistics().totalHashes(), uint64_t(0));
    CHECK_EQ(miner.statistics().acceptedShares(), uint64_t(0));
    CHECK_EQ(miner.statistics().hashrate10s(), 0.0);
}

TEST("stop is idempotent and safe when never started") {
    azd::mining::MinerController miner(Config{});
    ApiServer server(miner, "127.0.0.1", 0, "");

    CHECK_EQ(server.handleRequest(makeRequest("POST", "/api/miner/stop")).status, 200);
    CHECK_EQ(server.handleRequest(makeRequest("POST", "/api/miner/stop")).status, 200);
    CHECK(!miner.isRunning());
    CHECK_EQ(miner.statistics().totalHashes(), uint64_t(0));
}

TEST("PUT /api/config applies a patch") {
    azd::mining::MinerController miner(Config{});
    ApiServer server(miner, "127.0.0.1", 0, "");

    const HttpResponse response = server.handleRequest(makeRequest(
        "PUT", "/api/config",
        R"({"pool":{"host":"eu.pool.example","port":3334,"username":"bc1qexample.rig1",)"
        R"("password":"x"},"miner":{"name":"AZD-PC-01"}})"));
    CHECK_EQ(response.status, 200);

    const Config updated = miner.config();
    CHECK_EQ(updated.pool.host, std::string("eu.pool.example"));
    CHECK_EQ(updated.pool.port, uint16_t(3334));
    CHECK_EQ(updated.pool.username, std::string("bc1qexample.rig1"));
    CHECK_EQ(updated.miner.name, std::string("AZD-PC-01"));

    // The response echoes the config WITHOUT the password.
    CHECK(response.body.find("\"password\"") == std::string::npos);
}

TEST("PUT /api/config rejects malformed JSON") {
    azd::mining::MinerController miner(Config{});
    ApiServer server(miner, "127.0.0.1", 0, "");
    CHECK_EQ(server.handleRequest(makeRequest("PUT", "/api/config", "{not json")).status, 400);
}

TEST("static files are not served when no root is configured") {
    azd::mining::MinerController miner(Config{});
    ApiServer server(miner, "127.0.0.1", 0, "");
    CHECK_EQ(server.handleRequest(get("/")).status, 404);
}

// --- Configuration --------------------------------------------------------

TEST("config defaults are safe") {
    const Config config;
    CHECK_EQ(config.dashboard.host, std::string("127.0.0.1")); // localhost only
    CHECK_EQ(config.dashboard.port, uint16_t(8080));
    CHECK(config.pool.host.empty());     // no pool baked in
    CHECK(config.pool.username.empty()); // no developer wallet, ever
    CHECK(!config.poolConfigured());
    CHECK_EQ(config.gpu.temperatureStop, 85.0);
}

TEST("config loads from the example JSON shape") {
    const std::string text = R"({
      "miner":     {"name":"AZD-PC-01","device":0},
      "pool":      {"host":"","port":3333,"username":"","password":"x"},
      "gpu":       {"temperatureWarning":75,"temperatureStop":85},
      "dashboard": {"host":"127.0.0.1","port":8080}
    })";

    Config config;
    std::string error;
    REQUIRE(Config::loadFromString(text, config, &error));

    CHECK_EQ(config.miner.name, std::string("AZD-PC-01"));
    CHECK_EQ(config.pool.port, uint16_t(3333));
    CHECK_EQ(config.gpu.temperatureStop, 85.0);
    CHECK_EQ(config.dashboard.port, uint16_t(8080));
    // An example config has no credentials, so the miner must not consider
    // itself ready to mine.
    CHECK(!config.poolConfigured());
}

TEST("partial config keeps defaults for absent keys") {
    Config config;
    std::string error;
    REQUIRE(Config::loadFromString(R"({"pool":{"host":"a.example"}})", config, &error));
    CHECK_EQ(config.pool.host, std::string("a.example"));
    CHECK_EQ(config.pool.port, uint16_t(3333));   // default preserved
    CHECK_EQ(config.dashboard.host, std::string("127.0.0.1"));
}

TEST("invalid ports are ignored rather than accepted") {
    Config config;
    std::string error;
    REQUIRE(Config::loadFromString(R"({"pool":{"port":99999},"dashboard":{"port":-1}})", config,
                                   &error));
    CHECK_EQ(config.pool.port, uint16_t(3333));
    CHECK_EQ(config.dashboard.port, uint16_t(8080));
}

TEST("malformed config is an error, not a silent default") {
    Config config;
    std::string error;
    CHECK(!Config::loadFromString("{ not json", config, &error));
    CHECK(!error.empty());
    CHECK(!Config::loadFromString("[1,2,3]", config, &error));
}

// --- CUDA stub ------------------------------------------------------------

TEST("without CUDA the backend reports unavailable and simulates nothing") {
    azd::cuda::CudaMiningBackend backend;

    if (azd::cuda::CudaMiningBackend::compiledWithCuda()) {
        // On a CUDA build this test only checks internal consistency.
        CHECK(backend.available() == backend.unavailableReason().empty());
        return;
    }

    CHECK(!backend.available());
    CHECK_EQ(backend.unavailableReason(), std::string("CUDA backend not compiled"));
    CHECK_EQ(azd::cuda::CudaMiningBackend::deviceCount(), 0);
    CHECK(!backend.selectDevice(0));

    const azd::mining::DeviceInfo info = backend.deviceInfo();
    CHECK(!info.valid);
    CHECK(info.name.empty());

    const azd::mining::GpuTelemetry telemetry = backend.telemetry();
    CHECK(!telemetry.available);
    CHECK(!telemetry.hasTemperature);
    CHECK(!telemetry.hasPower);
    CHECK(!telemetry.hasUtilization);

    // mine() must return an error and claim ZERO hashes.
    azd::mining::MiningWork work;
    work.valid = true;
    const azd::mining::MiningResult result = backend.mine(work, 0, 1000000);
    CHECK(result.error);
    CHECK_EQ(result.errorMessage, std::string("CUDA backend not compiled"));
    CHECK(!result.found);
    CHECK_EQ(result.hashesPerformed, uint64_t(0));
    CHECK_EQ(result.hashesPerSecond(), 0.0);
}

TEST("the GPU endpoint reports unavailability honestly") {
    azd::mining::MinerController miner(configWithSecret());
    ApiServer server(miner, "127.0.0.1", 0, "");

    const HttpResponse response = server.handleRequest(get("/api/gpu"));
    azd::util::Json json;
    REQUIRE(azd::util::Json::parse(response.body, json));

    if (!azd::cuda::CudaMiningBackend::compiledWithCuda()) {
        CHECK(!json["available"].asBool(true));
        CHECK(!json["cudaCompiled"].asBool(true));
        CHECK_EQ(json["reason"].asString(), std::string("CUDA backend not compiled"));
        // Every telemetry field must be null -- not 0.
        CHECK(json["temperatureCelsius"].isNull());
        CHECK(json["powerWatts"].isNull());
        CHECK(json["utilizationPercent"].isNull());
        CHECK(!json["device"]["valid"].asBool(true));
    }
}

TEST("the self-test passes and reports measured values") {
    azd::mining::MinerController miner(Config{});
    std::string report;
    CHECK(miner.selfTest(report));
    CHECK(report.find("[PASS] genesis block hash") != std::string::npos);
    CHECK(report.find("[PASS] CPU nonce search") != std::string::npos);
    CHECK(report.find("hashes performed:") != std::string::npos);
}

AZD_TEST_MAIN("api")
