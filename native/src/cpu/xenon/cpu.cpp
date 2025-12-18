/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * PowerPC Xenon CPU implementation
 */

#include "cpu.h"
#include "memory/memory.h"
#include "kernel/kernel.h"

#ifdef X360MU_JIT_ENABLED
#include "../jit/jit.h"
#endif

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-cpu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[CPU] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[CPU ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

Cpu::Cpu() = default;
Cpu::~Cpu() = default;

Status Cpu::initialize(Memory* memory, const CpuConfig& config) {
    memory_ = memory;
    config_ = config;
    
    LOGI("Initializing CPU subsystem");
    LOGI("  Cores: %d, Threads: %d", cpu::NUM_CORES, cpu::NUM_THREADS);
    LOGI("  JIT: %s", config_.enable_jit ? "enabled" : "disabled");
    
    // Reset all thread contexts
    for (u32 i = 0; i < cpu::NUM_THREADS; i++) {
        contexts_[i].reset();
        contexts_[i].thread_id = i;
        contexts_[i].memory = memory;  // Set memory pointer for MMIO access
    }
    
    // Create interpreter (always needed as fallback)
    interpreter_ = std::make_unique<Interpreter>(memory_);
    
#ifdef X360MU_JIT_ENABLED
    if (config_.enable_jit) {
        LOGI("Initializing JIT compiler (cache: %llu MB)", config_.jit_cache_size / MB);
        jit_ = std::make_unique<JitCompiler>();
        if (jit_->initialize(memory_, config_.jit_cache_size) != Status::Ok) {
            LOGE("Failed to initialize JIT compiler, falling back to interpreter");
            jit_.reset();
        }
    }
#endif
    
    LOGI("CPU subsystem initialized");
    return Status::Ok;
}

void Cpu::shutdown() {
    LOGI("Shutting down CPU subsystem");
    
    // Stop all threads
    for (u32 i = 0; i < cpu::NUM_THREADS; i++) {
        contexts_[i].running = false;
    }
    
#ifdef X360MU_JIT_ENABLED
    jit_.reset();
#endif
    interpreter_.reset();
}

void Cpu::reset() {
    LOGI("Resetting CPU state");
    
    for (u32 i = 0; i < cpu::NUM_THREADS; i++) {
        contexts_[i].reset();
        contexts_[i].thread_id = i;
        contexts_[i].memory = memory_;  // Preserve memory pointer
    }
    
#ifdef X360MU_JIT_ENABLED
    if (jit_) {
        jit_->flush_cache();
    }
#endif
}

void Cpu::execute(u64 cycles) {
    // Distribute cycles across all running threads
    // Simple round-robin scheduling
    
    u64 cycles_per_thread = cycles / cpu::NUM_THREADS;
    u64 remaining = cycles % cpu::NUM_THREADS;
    
    for (u32 i = 0; i < cpu::NUM_THREADS; i++) {
        if (contexts_[i].running) {
            u64 thread_cycles = cycles_per_thread;
            if (remaining > 0) {
                thread_cycles++;
                remaining--;
            }
            execute_thread(i, thread_cycles);
        }
    }
}

void Cpu::execute_thread(u32 thread_id, u64 cycles) {
    ThreadContext& ctx = contexts_[thread_id];
    
    if (!ctx.running) {
        return;
    }
    
#ifdef X360MU_JIT_ENABLED
    if (jit_ && config_.enable_jit) {
        // JIT execution path
        jit_->execute(ctx, cycles);
        
        // Check for syscall after JIT execution too
        if (ctx.interrupted) {
            ctx.interrupted = false;
            dispatch_syscall(ctx);
        }
        return;
    }
#endif
    
    // Interpreter fallback
    interpreter_->execute(ctx, cycles);
    
    // Check for syscall after execution
    if (ctx.interrupted) {
        ctx.interrupted = false;
        dispatch_syscall(ctx);
    }
}

void Cpu::dispatch_syscall(ThreadContext& ctx) {
    // r0 contains: (module_id << 16) | ordinal
    // This encoding is set up by the import thunks (Task A.4)
    u32 ordinal = ctx.gpr[0] & 0xFFFF;
    u32 module = (ctx.gpr[0] >> 16) & 0xFF;
    
    if (kernel_) {
        kernel_->handle_syscall(ordinal, module);
    } else {
        LOGE("Syscall with no kernel: module=%u, ordinal=%u", module, ordinal);
    }
}

Status Cpu::start_thread(u32 thread_id, GuestAddr entry_point, GuestAddr stack) {
    if (thread_id >= cpu::NUM_THREADS) {
        LOGE("Invalid thread ID: %d", thread_id);
        return Status::InvalidArgument;
    }
    
    ThreadContext& ctx = contexts_[thread_id];
    
    // Initialize thread context
    ctx.reset();
    ctx.thread_id = thread_id;
    ctx.memory = memory_;  // Set memory pointer for MMIO access
    ctx.pc = entry_point;
    ctx.gpr[1] = stack; // Stack pointer in r1
    ctx.gpr[13] = 0;    // TLS pointer (will be set by kernel)
    ctx.running = true;
    
    LOGI("Started thread %d at entry 0x%08X, stack 0x%08X",
         thread_id, entry_point, stack);
    
    return Status::Ok;
}

void Cpu::stop_thread(u32 thread_id) {
    if (thread_id >= cpu::NUM_THREADS) {
        return;
    }
    
    contexts_[thread_id].running = false;
    LOGI("Stopped thread %d", thread_id);
}

void Cpu::raise_interrupt(u32 thread_id, u32 interrupt) {
    if (thread_id >= cpu::NUM_THREADS) {
        return;
    }
    
    contexts_[thread_id].interrupted = true;
}

void Cpu::clear_interrupt(u32 thread_id, u32 interrupt) {
    if (thread_id >= cpu::NUM_THREADS) {
        return;
    }
    
    contexts_[thread_id].interrupted = false;
}

ThreadContext& Cpu::get_context(u32 thread_id) {
    return contexts_[thread_id % cpu::NUM_THREADS];
}

const ThreadContext& Cpu::get_context(u32 thread_id) const {
    return contexts_[thread_id % cpu::NUM_THREADS];
}

bool Cpu::any_running() const {
    for (const auto& ctx : contexts_) {
        if (ctx.running) {
            return true;
        }
    }
    return false;
}

} // namespace x360mu

