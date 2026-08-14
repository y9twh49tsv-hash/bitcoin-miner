// Statistics must start at zero and change ONLY because of real activity.
// These tests exist specifically to catch any future "helpful" placeholder.

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "bitcoin/uint256.hpp"
#include "mining/stats.hpp"
#include "test_framework.hpp"
#include "util/json.hpp"

using azd::mining::GpuTelemetry;
using azd::mining::MiningState;
using azd::mining::PoolState;
using azd::mining::Statistics;

TEST("a fresh Statistics object reports nothing but zeros") {
    Statistics stats;

    CHECK_EQ(stats.totalHashes(), uint64_t(0));
    CHECK_EQ(stats.acceptedShares(), uint64_t(0));
    CHECK_EQ(stats.rejectedShares(), uint64_t(0));
    CHECK_EQ(stats.staleShares(), uint64_t(0));
    CHECK_EQ(stats.blocksFound(), uint64_t(0));
    CHECK_EQ(stats.hashrate10s(), 0.0);
    CHECK_EQ(stats.hashrate60s(), 0.0);
    CHECK_EQ(stats.sessionAverageHashrate(), 0.0);
    CHECK(stats.bestShareDifficulty().isZero());
    CHECK_EQ(stats.sessionSeconds(), 0.0);
    CHECK(stats.miningState() == MiningState::Stopped);
    CHECK(stats.poolState() == PoolState::Disconnected);
}

TEST("hash counters only move when hashes are added") {
    Statistics stats;
    stats.beginSession();

    CHECK_EQ(stats.totalHashes(), uint64_t(0));
    stats.addHashes(1000);
    CHECK_EQ(stats.totalHashes(), uint64_t(1000));
    stats.addHashes(0); // a no-op must stay a no-op
    CHECK_EQ(stats.totalHashes(), uint64_t(1000));
    stats.addHashes(500);
    CHECK_EQ(stats.totalHashes(), uint64_t(1500));
}

TEST("hashrate is zero until real hashes are recorded") {
    Statistics stats;
    stats.beginSession();
    CHECK_EQ(stats.hashrate10s(), 0.0);

    stats.addHashes(100000);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    stats.addHashes(100000);

    // A positive rate now exists, and it is derived from measured samples.
    CHECK(stats.hashrate10s() > 0.0);
    CHECK(stats.hashrate60s() > 0.0);
}

TEST("share counters are independent and never inferred from each other") {
    Statistics stats;
    stats.beginSession();

    stats.recordAcceptedShare(azd::bitcoin::UInt256(42), 42.0);
    CHECK_EQ(stats.acceptedShares(), uint64_t(1));
    CHECK_EQ(stats.rejectedShares(), uint64_t(0));

    stats.recordRejectedShare("Low difficulty share");
    CHECK_EQ(stats.acceptedShares(), uint64_t(1));
    CHECK_EQ(stats.rejectedShares(), uint64_t(1));
    CHECK_EQ(stats.lastRejectReason(), std::string("Low difficulty share"));

    stats.recordStaleShare();
    CHECK_EQ(stats.staleShares(), uint64_t(1));
    CHECK_EQ(stats.acceptedShares(), uint64_t(1)); // a stale share is NOT an accepted one
}

TEST("best difficulty tracks the maximum exactly") {
    Statistics stats;
    stats.beginSession();

    stats.recordAcceptedShare(azd::bitcoin::UInt256(10), 10.0);
    CHECK(stats.bestShareDifficulty() == azd::bitcoin::UInt256(10));

    stats.recordAcceptedShare(azd::bitcoin::UInt256(5), 5.0);
    CHECK(stats.bestShareDifficulty() == azd::bitcoin::UInt256(10)); // not replaced by a worse one

    stats.recordAcceptedShare(azd::bitcoin::UInt256(1000), 1000.0);
    CHECK(stats.bestShareDifficulty() == azd::bitcoin::UInt256(1000));
    CHECK_EQ(stats.bestShareDifficultyApprox(), 1000.0);
}

TEST("beginSession resets session state but keeps lifetime totals") {
    Statistics stats;
    stats.beginSession();
    stats.addHashes(1000);
    stats.recordAcceptedShare(azd::bitcoin::UInt256(99), 99.0);

    stats.beginSession();
    CHECK_EQ(stats.totalHashes(), uint64_t(1000));       // lifetime counter survives
    CHECK(stats.bestShareDifficulty().isZero());          // session best is reset
    CHECK_EQ(stats.hashrate10s(), 0.0);                   // samples cleared
}

TEST("session time only runs while a session is active") {
    Statistics stats;
    CHECK_EQ(stats.sessionSeconds(), 0.0);

    stats.beginSession();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(stats.sessionSeconds() > 0.0);

    stats.endSession();
    CHECK_EQ(stats.sessionSeconds(), 0.0);
}

TEST("uptime is always measured from process start") {
    Statistics stats;
    CHECK(stats.uptimeSeconds() >= 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(stats.uptimeSeconds() > 0.0);
}

TEST("JSON output reports unknown values as null, never as zero") {
    Statistics stats;
    const azd::util::Json json = stats.toJson();

    CHECK_EQ(json["miningState"].asString(), std::string("stopped"));
    CHECK_EQ(json["poolState"].asString(), std::string("disconnected"));
    CHECK_EQ(json["totalHashes"].asInt(-1), int64_t(0));
    CHECK_EQ(json["acceptedShares"].asInt(-1), int64_t(0));
    CHECK_EQ(json["rejectedShares"].asInt(-1), int64_t(0));

    // No share has been found, so there is no best difficulty to report.
    CHECK(json["bestShareDifficulty"].isNull());
    CHECK(json["lastRejectReason"].isNull());
    CHECK(json["currentJobId"].isNull());
    CHECK(json["poolDifficulty"].isNull());
    CHECK(json["pool"].isNull());
}

TEST("state transitions are reported verbatim") {
    Statistics stats;

    stats.setMiningState(MiningState::Running);
    CHECK_EQ(stats.toJson()["miningState"].asString(), std::string("running"));

    stats.setPoolState(PoolState::Authorized);
    CHECK_EQ(stats.toJson()["poolState"].asString(), std::string("authorized"));

    stats.setMiningState(MiningState::Error);
    CHECK_EQ(stats.toJson()["miningState"].asString(), std::string("error"));
}

TEST("concurrent hash accounting stays exact") {
    Statistics stats;
    stats.beginSession();

    constexpr int kThreads = 4;
    constexpr int kIterations = 2000;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&stats] {
            for (int j = 0; j < kIterations; ++j) stats.addHashes(10);
        });
    }
    for (std::thread& thread : threads) thread.join();

    CHECK_EQ(stats.totalHashes(), uint64_t(kThreads) * kIterations * 10);
}

// --- GPU telemetry --------------------------------------------------------

TEST("GPU telemetry defaults to unavailable with no numbers at all") {
    GpuTelemetry telemetry;
    CHECK(!telemetry.available);
    CHECK(!telemetry.hasTemperature);
    CHECK(!telemetry.hasUtilization);
    CHECK(!telemetry.hasPower);

    const azd::util::Json json = telemetry.toJson();
    CHECK(!json["available"].asBool(true));
    CHECK(json["temperatureCelsius"].isNull());
    CHECK(json["utilizationPercent"].isNull());
    CHECK(json["powerWatts"].isNull());
    CHECK(json["fanSpeedPercent"].isNull());
    CHECK(json["name"].isNull());
    CHECK_EQ(json["reason"].asString(), std::string("CUDA backend not compiled"));
}

TEST("GPU telemetry only reports values that were really measured") {
    GpuTelemetry telemetry;
    telemetry.available = true;
    telemetry.name = "Test Device";
    telemetry.hasTemperature = true;
    telemetry.temperatureCelsius = 61.0;
    // Power was NOT read.

    const azd::util::Json json = telemetry.toJson();
    CHECK_EQ(json["temperatureCelsius"].asNumber(-1.0), 61.0);
    CHECK(json["powerWatts"].isNull());
    CHECK(json["utilizationPercent"].isNull());
    CHECK_EQ(json["name"].asString(), std::string("Test Device"));
}

AZD_TEST_MAIN("stats")
