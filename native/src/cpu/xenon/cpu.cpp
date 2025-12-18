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
    
    // #region agent log - NEW HYPOTHESIS H: Check if CPU is executing
    static int exec_log = 0;
    if (exec_log++ < 20) {
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { fprintf(f, "{\"hypothesisId\":\"H\",\"location\":\"cpu.cpp:execute_thread\",\"message\":\"execute_thread called\",\"data\":{\"call\":%d,\"thread_id\":%u,\"running\":%d,\"pc\":%u,\"cycles\":%llu}}\n", exec_log, thread_id, ctx.running, (u32)ctx.pc, (unsigned long long)cycles); fclose(f); }
    }
    // #endregion
    
    if (!ctx.running) {
        return;
    }
    
#ifdef X360MU_JIT_ENABLED
    if (jit_ && config_.enable_jit) {
        // JIT execution path
        u64 executed = jit_->execute(ctx, cycles);
        
        // Check for syscall after JIT execution too
        if (ctx.interrupted) {
            ctx.interrupted = false;
            dispatch_syscall(ctx);
        }
        
        // If JIT actually executed something, we're done
        // Otherwise fall through to interpreter (JIT may have bailed out)
        if (executed > 0) {
            return;
        }
        // Fall through to interpreter
        LOGI("JIT returned 0 cycles - falling back to interpreter");
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
    
    // #region agent log - HYPOTHESIS L: Check syscalls being made
    static int syscall_log = 0;
    // Log first 200 syscalls, plus always log ExCreateThread (ordinal 14)
    if (syscall_log++ < 200 || ordinal == 14 || (ordinal != 2168 && syscall_log < 500)) {
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { fprintf(f, "{\"hypothesisId\":\"L\",\"location\":\"cpu.cpp:dispatch_syscall\",\"message\":\"SYSCALL\",\"data\":{\"call\":%d,\"r0\":%llu,\"module\":%u,\"ordinal\":%u,\"pc\":%u,\"lr\":%u}}\n", syscall_log, ctx.gpr[0], module, ordinal, (u32)ctx.pc, (u32)ctx.lr); fclose(f); }
    }
    // #endregion
    
    // SPIN LOOP ANALYSIS: Log LR for ordinal 2168 (KeSetEventBoostPriority)
    // This tells us where the spin loop is calling from
    static int spin_log = 0;
    static bool dumped_spin_code = false;
    if (ordinal == 2168 && spin_log++ < 10) {
        LOGI("KeSetEventBoostPriority called from LR=0x%08X, r3(event)=0x%08X, r4(boost)=0x%08X",
             (u32)ctx.lr, (u32)ctx.gpr[3], (u32)ctx.gpr[4]);
        
        // Dump the instructions around LR to understand the spin loop
        if (!dumped_spin_code && memory_ && ctx.lr >= 0x82000000 && ctx.lr < 0x90000000) {
            dumped_spin_code = true;
            LOGI("=== SPIN LOOP CODE DUMP (around LR=0x%08X) ===", (u32)ctx.lr);
            // Dump 16 instructions before and after LR
            GuestAddr start = (ctx.lr - 64) & ~3;  // Align to 4 bytes
            for (int i = 0; i < 32; i++) {
                GuestAddr addr = start + i * 4;
                u32 inst = memory_->read_u32(addr);
                LOGI("  0x%08X: %08X%s", (u32)addr, inst, (addr == ctx.lr) ? " <-- LR" : "");
            }
            LOGI("=== END SPIN LOOP CODE DUMP ===");
            
            // Also dump key registers
            LOGI("Key registers: r1(SP)=0x%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X",
                 (u32)ctx.gpr[1], (u32)ctx.gpr[3], (u32)ctx.gpr[4], (u32)ctx.gpr[5],
                 (u32)ctx.gpr[6], (u32)ctx.gpr[7], (u32)ctx.gpr[8], (u32)ctx.gpr[9]);
        }
    }
    
    // Debug: Log syscall dispatch
    static int dispatch_count = 0;
    if (dispatch_count < 10) {
        LOGI("dispatch_syscall: r0=0x%llX -> module=%u, ordinal=%u, PC=0x%llX, LR=0x%llX",
             ctx.gpr[0], module, ordinal, ctx.pc, ctx.lr);
        dispatch_count++;
    }
    
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

void Cpu::execute_with_context(u32 thread_id, ThreadContext& external_ctx, u64 cycles) {
    if (thread_id >= cpu::NUM_THREADS) {
        return;
    }
    
    // Lock the context for this thread
    std::lock_guard<std::mutex> lock(context_mutexes_[thread_id]);
    
    ThreadContext& cpu_ctx = contexts_[thread_id];
    
    // Copy external context to CPU context (save state)
    cpu_ctx = external_ctx;
    cpu_ctx.running = true;
    cpu_ctx.memory = memory_;  // Ensure memory pointer is valid
    
    // Execute using the CPU's context
#ifdef X360MU_JIT_ENABLED
    if (jit_ && config_.enable_jit) {
        u64 executed = jit_->execute(cpu_ctx, cycles);
        if (cpu_ctx.interrupted) {
            cpu_ctx.interrupted = false;
            dispatch_syscall(cpu_ctx);
        }
        if (executed > 0) {
            // Copy CPU context back to external context
            external_ctx = cpu_ctx;
            return;
        }
    }
#endif
    
    // Interpreter fallback
    interpreter_->execute(cpu_ctx, cycles);
    
    if (cpu_ctx.interrupted) {
        cpu_ctx.interrupted = false;
        dispatch_syscall(cpu_ctx);
    }
    
    // Copy CPU context back to external context (restore state)
    external_ctx = cpu_ctx;
}

std::mutex& Cpu::get_context_mutex(u32 thread_id) {
    return context_mutexes_[thread_id % cpu::NUM_THREADS];
}

} // namespace x360mu

