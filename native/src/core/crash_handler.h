/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Native crash handler - captures SIGSEGV, SIGBUS, SIGABRT, SIGFPE
 * and dumps CPU/GPU state + stack trace to crash log files.
 */

#pragma once

#include "x360mu/types.h"
#include <string>
#include <functional>

namespace x360mu {

class Emulator;

/**
 * Crash report data written to disk on fatal signal
 */
struct CrashReport {
    // Signal info
    int signal_number = 0;
    std::string signal_name;
    std::string fault_address;

    // CPU state at crash
    std::string pc;
    std::string lr;
    std::string sp;

    // Guest (PPC) state if available
    std::string guest_pc;
    std::string guest_lr;

    // GPU state
    std::string last_pm4_opcode;
    std::string gpu_state_summary;

    // Stack trace (host)
    std::string stack_trace;

    // Timestamp
    std::string timestamp;
    std::string crash_log_path;
};

/**
 * Install signal handlers for crash reporting.
 * Must be called once at emulator init time.
 *
 * @param crash_dir Directory to write crash logs to
 * @param emulator  Pointer to emulator for state capture (may be null)
 */
void install_crash_handler(const std::string& crash_dir, Emulator* emulator = nullptr);

/**
 * Uninstall signal handlers and restore defaults.
 */
void uninstall_crash_handler();

/**
 * List crash log files in the crash directory, newest first.
 */
std::vector<std::string> list_crash_logs(const std::string& crash_dir);

/**
 * Read a crash log file and return its contents.
 */
std::string read_crash_log(const std::string& path);

} // namespace x360mu
