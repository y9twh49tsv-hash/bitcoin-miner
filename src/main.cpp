// AZD Bitcoin Miner -- entry point.
//
// Honest-by-construction: this program never reports mining activity it did
// not perform. Without a configured pool it will not mine at all, and without
// CUDA it says so plainly instead of pretending to have a GPU.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "api/server.hpp"
#include "core/config.hpp"
#include "cuda/cuda_backend.hpp"
#include "mining/miner.hpp"
#include "util/logging.hpp"

namespace {

std::atomic<bool> g_shutdownRequested{false};

void handleSignal(int) { g_shutdownRequested.store(true); }

void printUsage() {
    std::cout << R"(AZD Bitcoin Miner

Usage: azd-miner [options]

Options:
  --config <path>      Load configuration from a JSON file.
                       (Copy config/config.example.json and edit it. Real
                        config files are gitignored -- never commit credentials.)
  --selftest           Run the built-in correctness checks and exit.
  --devices            List compute backends and exit.
  --no-dashboard       Do not start the local HTTP dashboard.
  --dashboard-root <p> Directory containing index.html (default: ./dashboard).
  --port <n>           Dashboard port (default: 8080).
  --start              Begin mining immediately (requires a configured pool).
  --verbose            Enable debug logging.
  --help               Show this message.

The dashboard binds to 127.0.0.1 and has no authentication. Do not expose it.
)";
}

std::string findDashboardRoot(const std::string& explicitRoot, const char* argv0) {
    namespace fs = std::filesystem;

    if (!explicitRoot.empty()) return explicitRoot;

    std::error_code ec;
    // 1. ./dashboard relative to the working directory.
    if (fs::exists("dashboard/index.html", ec)) return "dashboard";

    // 2. Next to the executable, and one level up (typical build layout).
    if (argv0 != nullptr) {
        const fs::path exePath = fs::absolute(fs::path(argv0), ec);
        if (!ec) {
            const fs::path exeDir = exePath.parent_path();
            for (const fs::path candidate :
                 {exeDir / "dashboard", exeDir.parent_path() / "dashboard",
                  exeDir.parent_path().parent_path() / "dashboard"}) {
                if (fs::exists(candidate / "index.html", ec)) return candidate.string();
            }
        }
    }
    return std::string();
}

} // namespace

int main(int argc, char** argv) {
    std::string configPath;
    std::string dashboardRoot;
    bool runSelfTest = false;
    bool listDevices = false;
    bool noDashboard = false;
    bool startMining = false;
    bool verbose = false;
    int portOverride = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--dashboard-root" && i + 1 < argc) {
            dashboardRoot = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            portOverride = std::atoi(argv[++i]);
        } else if (arg == "--selftest") {
            runSelfTest = true;
        } else if (arg == "--devices") {
            listDevices = true;
        } else if (arg == "--no-dashboard") {
            noDashboard = true;
        } else if (arg == "--start") {
            startMining = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n\n";
            printUsage();
            return 2;
        }
    }

    if (verbose) azd::util::setLogLevel(azd::util::LogLevel::Debug);

    std::cout << "AZD BITCOIN MINER 0.1.0\n";
    std::cout << "SHA-256d Bitcoin miner (foundation build)\n";
    std::cout << "CUDA support: "
              << (azd::cuda::CudaMiningBackend::compiledWithCuda()
                      ? "compiled"
                      : std::string(azd::cuda::kCudaNotCompiledMessage()))
              << "\n\n";

    azd::core::Config config;
    if (!configPath.empty()) {
        std::string error;
        if (!azd::core::Config::loadFromFile(configPath, config, &error)) {
            std::cerr << "Failed to load config: " << error << "\n";
            return 1;
        }
        azd::util::logInfo("main", "loaded config from " + configPath);
    }

    if (portOverride > 0 && portOverride <= 65535) {
        config.dashboard.port = static_cast<uint16_t>(portOverride);
    }

    if (listDevices) {
        std::cout << "Compute backends\n";
        std::cout << "  cpu-reference : available (correctness oracle; not a production miner)\n";
        std::cout << "  cuda          : ";
        if (!azd::cuda::CudaMiningBackend::compiledWithCuda()) {
            std::cout << "unavailable -- " << azd::cuda::kCudaNotCompiledMessage() << "\n";
            std::cout << "                  Rebuild with -DAZD_ENABLE_CUDA=ON on a machine with "
                         "the CUDA Toolkit.\n";
        } else {
            azd::cuda::CudaMiningBackend backend;
            if (backend.available()) {
                const azd::mining::DeviceInfo info = backend.deviceInfo();
                std::cout << "available -- " << info.name << " (compute "
                          << info.computeCapabilityMajor << "." << info.computeCapabilityMinor
                          << ", " << (info.totalMemoryBytes >> 20) << " MiB)\n";
            } else {
                std::cout << "unavailable -- " << backend.unavailableReason() << "\n";
            }
        }
        return 0;
    }

    azd::mining::MinerController miner(config);

    if (runSelfTest) {
        std::string report;
        const bool ok = miner.selfTest(report);
        std::cout << "Self-test\n" << report;
        std::cout << (ok ? "\nSELF-TEST PASSED\n" : "\nSELF-TEST FAILED\n");
        return ok ? 0 : 1;
    }

    std::unique_ptr<azd::api::ApiServer> server;
    if (!noDashboard) {
        const std::string root =
            findDashboardRoot(dashboardRoot.empty() ? config.dashboard.root : dashboardRoot,
                              argc > 0 ? argv[0] : nullptr);
        if (root.empty()) {
            azd::util::logWarn("main", "dashboard files not found; serving API only");
        }
        server = std::make_unique<azd::api::ApiServer>(miner, config.dashboard.host,
                                                       config.dashboard.port, root);
        std::string error;
        if (!server->start(error)) {
            std::cerr << "Failed to start dashboard: " << error << "\n";
            return 1;
        }
        std::cout << "Dashboard: http://" << config.dashboard.host << ":" << server->boundPort()
                  << "\n";
    }

    if (startMining) {
        std::string error;
        if (!miner.start(error)) {
            std::cerr << "Cannot start mining: " << error << "\n";
            if (noDashboard) return 1;
            std::cerr << "Configure the pool in the dashboard, then press START MINING.\n";
        }
    } else {
        std::cout << "Miner is STOPPED. Configure a pool and press START MINING in the "
                     "dashboard (or pass --start).\n";
    }

    std::signal(SIGINT, handleSignal);
#if !defined(_WIN32)
    std::signal(SIGTERM, handleSignal);
#endif

    std::cout << "\nPress Ctrl+C to exit.\n" << std::endl;

    while (!g_shutdownRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutting down...\n";
    miner.stop();
    if (server) server->stop();
    std::cout << "Stopped.\n";
    return 0;
}
