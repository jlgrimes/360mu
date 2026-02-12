/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Thread-safe ring buffer log implementation.
 */

#include "log_buffer.h"

#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <sstream>
#include <iomanip>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace x360mu {

static const char* severity_str(LogSeverity s) {
    switch (s) {
        case LogSeverity::Debug:   return "D";
        case LogSeverity::Info:    return "I";
        case LogSeverity::Warning: return "W";
        case LogSeverity::Error:   return "E";
    }
    return "?";
}

static const char* component_str(LogComponent c) {
    switch (c) {
        case LogComponent::Core:   return "CORE";
        case LogComponent::CPU:    return "CPU";
        case LogComponent::GPU:    return "GPU";
        case LogComponent::APU:    return "APU";
        case LogComponent::Kernel: return "KERN";
        case LogComponent::Memory: return "MEM";
        case LogComponent::Input:  return "INPUT";
        case LogComponent::JIT:    return "JIT";
        case LogComponent::Loader: return "LOAD";
    }
    return "???";
}

LogBuffer& LogBuffer::instance() {
    static LogBuffer s_instance;
    return s_instance;
}

LogBuffer::LogBuffer() {
    entries_.resize(DEFAULT_CAPACITY);
    auto now = std::chrono::steady_clock::now();
    start_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

void LogBuffer::log(LogSeverity severity, LogComponent component, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    auto now = std::chrono::steady_clock::now();
    u64 ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() - start_time_ms_;

    // Also forward to Android logcat
#ifdef __ANDROID__
    int prio = ANDROID_LOG_DEBUG;
    switch (severity) {
        case LogSeverity::Info:    prio = ANDROID_LOG_INFO; break;
        case LogSeverity::Warning: prio = ANDROID_LOG_WARN; break;
        case LogSeverity::Error:   prio = ANDROID_LOG_ERROR; break;
        default: break;
    }
    __android_log_print(prio, "360mu", "[%s] %s", component_str(component), buf);
#endif

    std::lock_guard<std::mutex> lock(mutex_);

    auto& entry = entries_[write_pos_];
    entry.timestamp_ms = ts;
    entry.severity = severity;
    entry.component = component;
    entry.message = buf;

    write_pos_++;
    if (write_pos_ >= capacity_) {
        write_pos_ = 0;
        wrapped_ = true;
    }
    total_written_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<LogEntry> LogBuffer::get_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<LogEntry> result;
    if (!wrapped_) {
        result.assign(entries_.begin(), entries_.begin() + write_pos_);
    } else {
        // Return in chronological order: from write_pos_ to end, then 0 to write_pos_
        result.reserve(capacity_);
        result.insert(result.end(), entries_.begin() + write_pos_, entries_.begin() + capacity_);
        result.insert(result.end(), entries_.begin(), entries_.begin() + write_pos_);
    }
    return result;
}

std::vector<LogEntry> LogBuffer::get_filtered(LogSeverity severity_min, int component) const {
    auto all = get_entries();
    std::vector<LogEntry> result;
    result.reserve(all.size());

    for (auto& e : all) {
        if (static_cast<u8>(e.severity) >= static_cast<u8>(severity_min)) {
            if (component < 0 || static_cast<int>(e.component) == component) {
                result.push_back(std::move(e));
            }
        }
    }
    return result;
}

std::string LogBuffer::export_text() const {
    auto entries = get_entries();
    std::ostringstream ss;
    ss << "=== 360μ Log Export ===\n";
    ss << "Entries: " << entries.size() << "\n";
    ss << "Total written: " << total_written_.load() << "\n\n";

    for (auto& e : entries) {
        u64 secs = e.timestamp_ms / 1000;
        u64 ms = e.timestamp_ms % 1000;
        ss << std::setw(5) << secs << "." << std::setfill('0') << std::setw(3) << ms
           << std::setfill(' ')
           << " [" << severity_str(e.severity) << "] "
           << "[" << std::setw(5) << component_str(e.component) << "] "
           << e.message << "\n";
    }
    return ss.str();
}

void LogBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    write_pos_ = 0;
    wrapped_ = false;
}

void LogBuffer::set_capacity(u32 capacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = capacity;
    entries_.resize(capacity);
    write_pos_ = 0;
    wrapped_ = false;
}

} // namespace x360mu
