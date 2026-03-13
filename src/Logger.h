#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdarg>
#include <cstdio>

// Lang.h included by Logger.cpp — forward declaration not needed here

// ============================================================================
//  Log levels
// ============================================================================
enum class LogLevel { Debug, Info, Warn, Error, Fatal };

// ============================================================================
//  Single log entry (stored in ring buffer for in-app display)
// ============================================================================
struct LogEntry {
    LogLevel    level;
    std::string timestamp;
    std::string message;
};

// ============================================================================
//  Logger – singleton
//
//  Usage:
//    LOG_INFO("Initialized WASAPI, found %d devices", count);
//    LOG_WARN("Buffer underrun on node %d", nodeId);
//    LOG_ERROR("CoCreateInstance failed: HRESULT 0x%08X", hr);
// ============================================================================
class Logger {
public:
    static Logger& get() {
        static Logger instance;
        return instance;
    }

    // Call once at startup with path to log file
    void open(const std::string& path);
    void close();

    void log(LogLevel level, const char* fmt, ...);

    // ── In-app log window ────────────────────────────────────────────────────
    // Call inside an ImGui frame to draw the floating log window
    void drawWindow(bool* p_open);

    // Access to entries (for custom rendering)
    const std::vector<LogEntry>& entries() const { return m_entries; }
    void clear() { std::lock_guard<std::mutex> lk(m_mutex); m_entries.clear(); }

    // Auto-scroll toggle
    bool autoScroll { true };

private:
    Logger() = default;
    ~Logger() { close(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::string makeTimestamp();
    const char* levelStr(LogLevel l);

    std::ofstream            m_file;
    std::vector<LogEntry>    m_entries;   // ring: last 2000 lines
    mutable std::mutex       m_mutex;
    static constexpr size_t  kMaxEntries = 2000;
};

// ── Convenience macros ────────────────────────────────────────────────────────
#define LOG_DEBUG(...)  Logger::get().log(LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)   Logger::get().log(LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)   Logger::get().log(LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...)  Logger::get().log(LogLevel::Error, __VA_ARGS__)
#define LOG_FATAL(...)  Logger::get().log(LogLevel::Fatal, __VA_ARGS__)

// HRESULT helper
#define LOG_HR(msg, hr) \
    do { if(FAILED(hr)) LOG_ERROR("%s  HRESULT=0x%08X", msg, (unsigned)(hr)); \
         else            LOG_DEBUG("%s  OK", msg); } while(0)
