#pragma once

#include <string>

namespace azd::util {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void setLogLevel(LogLevel level);
LogLevel logLevel();

/// Thread-safe line logging to stdout.
///
/// SECURITY: never pass a pool password, wallet key or any other credential to
/// these functions. `redactSecret()` exists for the cases where a value has to
/// appear in a message at all.
void logMessage(LogLevel level, const std::string& component, const std::string& message);

void logDebug(const std::string& component, const std::string& message);
void logInfo(const std::string& component, const std::string& message);
void logWarn(const std::string& component, const std::string& message);
void logError(const std::string& component, const std::string& message);

/// Turns any secret into a fixed placeholder. Never reveals length or prefix.
std::string redactSecret(const std::string& secret);

} // namespace azd::util
