/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Thread-safe ring buffer for log capture.
 * Stores last N log entries with timestamp, component, and severity.
 */

#pragma once

#include "x360mu/types.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace x360mu {

enum class LogSeverity : u8 {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3
};

enum class LogComponent : u8 {
    Core = 0,
    CPU = 1,
    GPU = 2,
    APU = 3,
    Kernel = 4,
    Memory = 5,
    Input = 6,
    JIT = 7,
    Loader = 8
};

struct LogEntry {
    u64 timestamp_ms;       // milliseconds since emulator start
    LogSeverity severity;
    LogComponent component;
    std::string message;
};

/**
 * Global log buffer — singleton ring buffer for all emulator subsystems.
 */
class LogBuffer {
public:
    static LogBuffer& instance();

    /**
     * Add a log entry to the ring buffer.
     * Thread-safe.
     */
    void log(LogSeverity severity, LogComponent component, const char* fmt, ...);

    /**
     * Get a snapshot of all entries currently in the buffer.
     * Returns entries from oldest to newest.
     */
    std::vector<LogEntry> get_entries() const;

    /**
     * Get entries filtered by severity and/or component.
     * severity_min: minimum severity to include (Debug=all, Error=errors only)
     * component: filter to specific component, or -1 for all
     */
    std::vector<LogEntry> get_filtered(LogSeverity severity_min,
                                        int component = -1) const;

    /**
     * Get entries as a formatted string for export.
     */
    std::string export_text() const;

    /**
     * Clear the buffer.
     */
    void clear();

    /**
     * Get total number of entries written (including overwritten).
     */
    u64 total_entries() const { return total_written_.load(); }

    /**
     * Set max buffer size (default 1000).
     */
    void set_capacity(u32 capacity);

private:
    LogBuffer();
    ~LogBuffer() = default;
    LogBuffer(const LogBuffer&) = delete;
    LogBuffer& operator=(const LogBuffer&) = delete;

    static constexpr u32 DEFAULT_CAPACITY = 1000;

    mutable std::mutex mutex_;
    std::vector<LogEntry> entries_;
    u32 capacity_ = DEFAULT_CAPACITY;
    u32 write_pos_ = 0;
    bool wrapped_ = false;
    std::atomic<u64> total_written_{0};

    // Start time for relative timestamps
    u64 start_time_ms_ = 0;
};

// Convenience macros
#define EMU_LOG_D(component, ...) \
    x360mu::LogBuffer::instance().log(x360mu::LogSeverity::Debug, component, __VA_ARGS__)
#define EMU_LOG_I(component, ...) \
    x360mu::LogBuffer::instance().log(x360mu::LogSeverity::Info, component, __VA_ARGS__)
#define EMU_LOG_W(component, ...) \
    x360mu::LogBuffer::instance().log(x360mu::LogSeverity::Warning, component, __VA_ARGS__)
#define EMU_LOG_E(component, ...) \
    x360mu::LogBuffer::instance().log(x360mu::LogSeverity::Error, component, __VA_ARGS__)

} // namespace x360mu
