/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Native crash handler implementation.
 * Installs signal handlers for fatal signals and writes crash dumps.
 */

#include "crash_handler.h"
#include "log_buffer.h"
#include "x360mu/emulator.h"

#include <signal.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <unwind.h>
#include <dlfcn.h>
#define CRASH_TAG "360mu-crash"
#define CRASH_LOG(...) __android_log_print(ANDROID_LOG_ERROR, CRASH_TAG, __VA_ARGS__)
#else
#define CRASH_LOG(...) fprintf(stderr, "[CRASH] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

// Global state for signal handler (must be async-signal-safe access)
static std::string g_crash_dir;
static Emulator* g_emulator = nullptr;
static struct sigaction g_old_handlers[32] = {};
static const int g_signals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGFPE};
static constexpr int g_signal_count = 4;

static const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        default:      return "UNKNOWN";
    }
}

#ifdef __ANDROID__
// Stack unwinding for Android NDK
struct UnwindState {
    void** frames;
    int frame_count;
    int max_frames;
};

static _Unwind_Reason_Code unwind_callback(struct _Unwind_Context* context, void* arg) {
    auto* state = static_cast<UnwindState*>(arg);
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc && state->frame_count < state->max_frames) {
        state->frames[state->frame_count++] = reinterpret_cast<void*>(pc);
    }
    return _URC_NO_REASON;
}

static std::string capture_stack_trace() {
    constexpr int MAX_FRAMES = 32;
    void* frames[MAX_FRAMES];
    UnwindState state = {frames, 0, MAX_FRAMES};

    _Unwind_Backtrace(unwind_callback, &state);

    std::string result;
    char buf[256];
    for (int i = 0; i < state.frame_count; i++) {
        Dl_info info;
        if (dladdr(frames[i], &info) && info.dli_sname) {
            snprintf(buf, sizeof(buf), "#%02d %p %s+%td (%s)\n",
                     i, frames[i], info.dli_sname,
                     static_cast<char*>(frames[i]) - static_cast<char*>(info.dli_saddr),
                     info.dli_fname ? info.dli_fname : "?");
        } else {
            snprintf(buf, sizeof(buf), "#%02d %p\n", i, frames[i]);
        }
        result += buf;
    }
    return result;
}
#else
static std::string capture_stack_trace() {
    return "(stack trace not available on this platform)\n";
}
#endif

static void crash_signal_handler(int sig, siginfo_t* info, void* ucontext) {
    // Prevent re-entry
    static volatile sig_atomic_t in_handler = 0;
    if (in_handler) {
        _exit(128 + sig);
    }
    in_handler = 1;

    CRASH_LOG("=== FATAL SIGNAL %d (%s) ===", sig, signal_name(sig));

    // Build crash log content
    char log_buf[8192];
    int offset = 0;

    // Timestamp
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);

    offset += snprintf(log_buf + offset, sizeof(log_buf) - offset,
        "=== 360μ Crash Report ===\n"
        "Time: %s\n"
        "Signal: %d (%s)\n",
        time_str, sig, signal_name(sig));

    // Fault address
    if (info) {
        offset += snprintf(log_buf + offset, sizeof(log_buf) - offset,
            "Fault Address: %p\n", info->si_addr);
    }

    // Host CPU registers from ucontext
#if defined(__ANDROID__) && defined(__aarch64__)
    if (ucontext) {
        auto* uc = static_cast<ucontext_t*>(ucontext);
        offset += snprintf(log_buf + offset, sizeof(log_buf) - offset,
            "\n--- Host CPU State (ARM64) ---\n"
            "PC:  0x%016llx\n"
            "LR:  0x%016llx\n"
            "SP:  0x%016llx\n"
            "X0:  0x%016llx  X1:  0x%016llx\n"
            "X2:  0x%016llx  X3:  0x%016llx\n"
            "X28: 0x%016llx  X29: 0x%016llx\n",
            (unsigned long long)uc->uc_mcontext.pc,
            (unsigned long long)uc->uc_mcontext.regs[30],
            (unsigned long long)uc->uc_mcontext.sp,
            (unsigned long long)uc->uc_mcontext.regs[0],
            (unsigned long long)uc->uc_mcontext.regs[1],
            (unsigned long long)uc->uc_mcontext.regs[2],
            (unsigned long long)uc->uc_mcontext.regs[3],
            (unsigned long long)uc->uc_mcontext.regs[28],
            (unsigned long long)uc->uc_mcontext.regs[29]);
    }
#endif

    // Emulator state
    if (g_emulator) {
        auto state = g_emulator->get_state();
        const char* state_str = "Unknown";
        switch (state) {
            case EmulatorState::Running:  state_str = "Running"; break;
            case EmulatorState::Paused:   state_str = "Paused"; break;
            case EmulatorState::Loaded:   state_str = "Loaded"; break;
            case EmulatorState::Ready:    state_str = "Ready"; break;
            case EmulatorState::Stopped:  state_str = "Stopped"; break;
            case EmulatorState::Error:    state_str = "Error"; break;
            default: break;
        }
        offset += snprintf(log_buf + offset, sizeof(log_buf) - offset,
            "\n--- Emulator State ---\n"
            "State: %s\n", state_str);

        auto stats = g_emulator->get_stats();
        offset += snprintf(log_buf + offset, sizeof(log_buf) - offset,
            "FPS: %.1f\n"
            "Frame Time: %.2f ms\n"
            "Frames: %llu\n"
            "CPU Cycles: %llu\n",
            stats.fps, stats.frame_time_ms,
            (unsigned long long)stats.frames_rendered,
            (unsigned long long)stats.cpu_cycles);
    }

    // Stack trace
    std::string trace = capture_stack_trace();
    offset += snprintf(log_buf + offset, sizeof(log_buf) - offset,
        "\n--- Stack Trace ---\n%s", trace.c_str());

    // Recent log entries
    auto& log = LogBuffer::instance();
    auto entries = log.get_filtered(LogSeverity::Warning);
    if (!entries.empty()) {
        offset += snprintf(log_buf + offset, sizeof(log_buf) - offset,
            "\n--- Recent Warnings/Errors ---\n");
        // Last 20 warning/error entries
        int start = entries.size() > 20 ? entries.size() - 20 : 0;
        for (size_t i = start; i < entries.size() && offset < (int)sizeof(log_buf) - 128; i++) {
            const char* sev = entries[i].severity == LogSeverity::Error ? "E" : "W";
            offset += snprintf(log_buf + offset, sizeof(log_buf) - offset,
                "[%s] %s\n", sev, entries[i].message.c_str());
        }
    }

    // Write to file
    if (!g_crash_dir.empty()) {
        char filename[512];
        snprintf(filename, sizeof(filename), "%s/crash_%s_%d.log",
                 g_crash_dir.c_str(), time_str, sig);
        // Replace spaces/colons in filename
        for (char* p = filename + g_crash_dir.size() + 1; *p; p++) {
            if (*p == ' ' || *p == ':') *p = '_';
        }

        FILE* f = fopen(filename, "w");
        if (f) {
            fwrite(log_buf, 1, offset, f);
            fclose(f);
            CRASH_LOG("Crash log written to: %s", filename);
        }
    }

    CRASH_LOG("%s", log_buf);

    // Restore original handler and re-raise
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

void install_crash_handler(const std::string& crash_dir, Emulator* emulator) {
    g_crash_dir = crash_dir;
    g_emulator = emulator;

    // Create crash directory
    mkdir(crash_dir.c_str(), 0755);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (int i = 0; i < g_signal_count; i++) {
        sigaction(g_signals[i], &sa, &g_old_handlers[g_signals[i]]);
    }

    EMU_LOG_I(LogComponent::Core, "Crash handler installed, log dir: %s", crash_dir.c_str());
}

void uninstall_crash_handler() {
    for (int i = 0; i < g_signal_count; i++) {
        sigaction(g_signals[i], &g_old_handlers[g_signals[i]], nullptr);
    }
    g_emulator = nullptr;
}

std::vector<std::string> list_crash_logs(const std::string& crash_dir) {
    std::vector<std::string> results;
    DIR* dir = opendir(crash_dir.c_str());
    if (!dir) return results;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("crash_") == 0 && name.find(".log") != std::string::npos) {
            results.push_back(crash_dir + "/" + name);
        }
    }
    closedir(dir);

    // Sort newest first (filenames contain timestamps)
    std::sort(results.begin(), results.end(), std::greater<>());
    return results;
}

std::string read_crash_log(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return "";
    }

    std::string content(size, '\0');
    fread(content.data(), 1, size, f);
    fclose(f);
    return content;
}

} // namespace x360mu
