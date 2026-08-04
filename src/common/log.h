#pragma once

#include <format>
#include <string>
#include <string_view>

namespace audiolens {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

/// Writes to stderr with a timestamp. Not for use from the audio threads:
/// it takes a lock and may block. Audio threads accumulate atomic counters
/// instead, which the control thread logs.
void logMessage(LogLevel level, std::string_view message);

void setLogLevel(LogLevel level);
LogLevel logLevel();

template <typename... Args>
void logf(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (level < logLevel()) {
        return;
    }
    logMessage(level, std::format(fmt, std::forward<Args>(args)...));
}

#define AL_TRACE(...) ::audiolens::logf(::audiolens::LogLevel::Trace, __VA_ARGS__)
#define AL_DEBUG(...) ::audiolens::logf(::audiolens::LogLevel::Debug, __VA_ARGS__)
#define AL_INFO(...) ::audiolens::logf(::audiolens::LogLevel::Info, __VA_ARGS__)
#define AL_WARN(...) ::audiolens::logf(::audiolens::LogLevel::Warn, __VA_ARGS__)
#define AL_ERROR(...) ::audiolens::logf(::audiolens::LogLevel::Error, __VA_ARGS__)

}  // namespace audiolens
