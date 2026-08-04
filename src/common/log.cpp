#include "common/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace audiolens {
namespace {

std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

void setLogLevel(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }

LogLevel logLevel() { return g_level.load(std::memory_order_relaxed); }

void logMessage(LogLevel level, std::string_view message) {
    if (level < logLevel()) {
        return;
    }
    const auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    const std::string stamp = std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::milliseconds>(now));

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[%s] %s %.*s\n", stamp.c_str(), levelName(level),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

}  // namespace audiolens
