#include "util/logging.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <mutex>

namespace azd::util {
namespace {

std::mutex& logMutex() {
    static std::mutex m;
    return m;
}

LogLevel& currentLevel() {
    static LogLevel level = LogLevel::Info;
    return level;
}

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace

void setLogLevel(LogLevel level) { currentLevel() = level; }
LogLevel logLevel() { return currentLevel(); }

void logMessage(LogLevel level, const std::string& component, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(currentLevel())) return;
    std::lock_guard<std::mutex> lock(logMutex());
    std::cout << timestamp() << " [" << levelName(level) << "] [" << component << "] " << message
              << std::endl;
}

void logDebug(const std::string& c, const std::string& m) { logMessage(LogLevel::Debug, c, m); }
void logInfo(const std::string& c, const std::string& m) { logMessage(LogLevel::Info, c, m); }
void logWarn(const std::string& c, const std::string& m) { logMessage(LogLevel::Warn, c, m); }
void logError(const std::string& c, const std::string& m) { logMessage(LogLevel::Error, c, m); }

std::string redactSecret(const std::string& secret) {
    if (secret.empty()) return "<empty>";
    return "<redacted>";
}

} // namespace azd::util
