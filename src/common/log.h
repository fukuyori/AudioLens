#pragma once

#include <format>
#include <string>
#include <string_view>

namespace audiolens {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

/// Writes to stderr, and to the log file if one has been opened, with a
/// timestamp. Not for use from the audio threads: it takes a lock and may
/// block. Audio threads accumulate atomic counters instead, which the control
/// thread logs.
void logMessage(LogLevel level, std::string_view message);

/// Starts appending to `path` as well as stderr. Returns false if the file
/// cannot be opened, in which case logging carries on to stderr alone.
///
/// The GUI needs this and the CLI tools do not: a windowed process has no
/// console, so its stderr goes nowhere and every diagnostic it emits is lost.
/// That is tolerable for a fault the user is watching happen and useless for
/// one that happens while the machine is asleep — which is exactly the class of
/// problem worth recording.
///
/// The date is included in each line here but not on stderr, because a file is
/// read days later and a console is read as it scrolls.
bool setLogFile(const std::string& path);

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
