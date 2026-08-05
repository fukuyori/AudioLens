#include "common/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace audiolens {
namespace {

std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;

/// Guarded by g_mutex. Never closed: the process exiting closes it, and a
/// close here would race every other thread that is still logging.
std::FILE* g_file = nullptr;

/// Rotate at a megabyte. Large enough to hold weeks of an app that logs only
/// when something happens, small enough to open in an editor.
constexpr long kMaxLogBytes = 1024 * 1024;

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

bool setLogFile(const std::string& path) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        return true;  // Already logging somewhere; one destination is enough.
    }

    // Rotate before opening rather than while writing, so the check happens
    // once per run instead of once per line, and so a long-running session
    // never has the file renamed out from under it.
    if (std::FILE* existing = std::fopen(path.c_str(), "rb")) {
        std::fseek(existing, 0, SEEK_END);
        const long size = std::ftell(existing);
        std::fclose(existing);
        if (size > kMaxLogBytes) {
            const std::string previous = path + ".1";
            std::remove(previous.c_str());
            std::rename(path.c_str(), previous.c_str());
        }
    }

    g_file = std::fopen(path.c_str(), "a");
    return g_file != nullptr;
}

void logMessage(LogLevel level, std::string_view message) {
    if (level < logLevel()) {
        return;
    }
    const auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    const auto floored = std::chrono::floor<std::chrono::milliseconds>(now);
    const std::string stamp = std::format("{:%H:%M:%S}", floored);

    const std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[%s] %s %.*s\n", stamp.c_str(), levelName(level),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);

    if (g_file != nullptr) {
        // Dated, unlike the console line: a file is read days after the fact,
        // and "17:22:16" alone cannot say which night it belongs to.
        const std::string dated = std::format("{:%Y-%m-%d %H:%M:%S}", floored);
        std::fprintf(g_file, "[%s] %s %.*s\n", dated.c_str(), levelName(level),
                     static_cast<int>(message.size()), message.data());
        // Flushed every line on purpose. The events worth having are the ones
        // written just before the process is killed or the machine sleeps, and
        // those are precisely the ones a buffer loses.
        std::fflush(g_file);
    }
}

}  // namespace audiolens
