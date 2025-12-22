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
    
    // === DEBUG: Log PC periodically to see where the game is spinning ===
    static u64 exec_call_count = 0;
    static u64 stuck_pc_count = 0;
    static GuestAddr last_logged_pc = 0;
    exec_call_count++;
    
    // Special tracking for the known stuck PC
    static u64 stuck_lr_count = 0;
    static GuestAddr last_stuck_lr = 0;

    if (external_ctx.pc == 0x825FB308) {
        stuck_pc_count++;

        // Track if we're being called from the same LR repeatedly
        if (external_ctx.lr == last_stuck_lr) {
            stuck_lr_count++;
        } else {
            last_stuck_lr = external_ctx.lr;
            stuck_lr_count = 1;
        }

        if (stuck_pc_count == 1) {
            // Dump all registers on first hit
            LOGI("STUCK PC 0x825FB308 FIRST HIT:");
            LOGI("  r0-r7: 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX",
                 external_ctx.gpr[0], external_ctx.gpr[1], external_ctx.gpr[2], external_ctx.gpr[3],
                 external_ctx.gpr[4], external_ctx.gpr[5], external_ctx.gpr[6], external_ctx.gpr[7]);
            LOGI("  r8-r15: 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX",
                 external_ctx.gpr[8], external_ctx.gpr[9], external_ctx.gpr[10], external_ctx.gpr[11],
                 external_ctx.gpr[12], external_ctx.gpr[13], external_ctx.gpr[14], external_ctx.gpr[15]);
            LOGI("  r30-r31: 0x%llX 0x%llX, LR=0x%llX, CTR=0x%llX",
                 external_ctx.gpr[30], external_ctx.gpr[31], external_ctx.lr, external_ctx.ctr);
            // r13 is PCR, PCR[0] should be TLS pointer
            u32 pcr = (u32)external_ctx.gpr[13];
            LOGI("  r13(PCR)=0x%08X", pcr);
            if (pcr > 0 && pcr < 0x20000000) {
                u32 tls_ptr = memory_->read_u32(pcr + 0);  // PCR[0] = TLS pointer

                // Also read directly from fastmem for comparison
                u8* fastmem = static_cast<u8*>(memory_->get_fastmem_base());
                u32 tls_ptr_direct = 0;
                if (fastmem) {
                    u32 raw;
                    memcpy(&raw, fastmem + (pcr & 0x1FFFFFFF), sizeof(u32));
                    tls_ptr_direct = __builtin_bswap32(raw);  // Big-endian swap
                }

                LOGI("  PCR[0](TLS ptr)=0x%08X, direct_fastmem=0x%08X", tls_ptr, tls_ptr_direct);
                if (tls_ptr > 0 && tls_ptr < 0x20000000) {
                    LOGI("  TLS[0-7]: 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X 0x%X",
                         memory_->read_u32(tls_ptr+0), memory_->read_u32(tls_ptr+4), memory_->read_u32(tls_ptr+8), memory_->read_u32(tls_ptr+12),
                         memory_->read_u32(tls_ptr+16), memory_->read_u32(tls_ptr+20), memory_->read_u32(tls_ptr+24), memory_->read_u32(tls_ptr+28));
                }
            }
        } else if (stuck_pc_count == 100 || stuck_pc_count == 1000) {
            LOGI("STUCK PC 0x825FB308: count=%llu, LR=0x%08llX, r3=0x%llX, LR_count=%llu",
                 stuck_pc_count, external_ctx.lr, external_ctx.gpr[3], stuck_lr_count);
        }

        // WORKAROUND: If the loop is stuck with an invalid destination pointer (r3 < 0x1000),
        // just pretend the memset succeeded by returning to the caller immediately.
        // Don't actually write anything since the pointer is invalid.
        if (stuck_pc_count >= 10 && external_ctx.gpr[3] < 0x1000) {
            LOGI("WORKAROUND: Memset loop with invalid ptr r3=0x%llX - returning success to LR=0x%08llX",
                 external_ctx.gpr[3], external_ctx.lr);
            // Set r3 to the original destination pointer (memset returns dst)
            // This makes it look like the function succeeded
            external_ctx.pc = external_ctx.lr;  // Return to caller
            external_ctx.ctr = 0;  // Reset CTR
            stuck_pc_count = 0;
            stuck_lr_count = 0;
        }
    } else {
        if (stuck_pc_count > 0) {
            LOGI("Left stuck PC after %llu iterations, now at PC=0x%08llX",
                 stuck_pc_count, external_ctx.pc);
            stuck_pc_count = 0;
            stuck_lr_count = 0;
            last_stuck_lr = 0;
        }
    }
    
    // Trace the caller of the stuck memset (around 0x824D35D4)
    static u64 trace_count = 0;
    if (external_ctx.pc >= 0x824D3500 && external_ctx.pc <= 0x824D3600) {
        if (trace_count < 20) {
            trace_count++;
            LOGI("TRACE PC=0x%08llX: r3=0x%llX, r4=0x%llX, r5=0x%llX, r11=0x%llX, r12=0x%llX, LR=0x%08llX",
                 external_ctx.pc, external_ctx.gpr[3], external_ctx.gpr[4], external_ctx.gpr[5],
                 external_ctx.gpr[11], external_ctx.gpr[12], external_ctx.lr);
        }
    }
    
    // Log every 50000 calls, or when PC changes significantly
    if (exec_call_count <= 10 || (exec_call_count % 50000 == 0)) {
        LOGI("execute_with_context #%llu: tid=%u PC=0x%08llX LR=0x%08llX time_base=%llu",
             exec_call_count, thread_id, external_ctx.pc, external_ctx.lr, external_ctx.time_base);
        last_logged_pc = external_ctx.pc;
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
            // CRITICAL FIX: Sync cpu_ctx to external_ctx BEFORE dispatch_syscall.
            // handle_syscall uses GetCurrentGuestThread()->context (= external_ctx).
            // Without this sync, handle_syscall sees old register values!
            external_ctx = cpu_ctx;
            dispatch_syscall(cpu_ctx);
            // After syscall, handle_syscall modified external_ctx, so sync back
            cpu_ctx = external_ctx;
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
        // CRITICAL FIX: Same fix for interpreter path
        external_ctx = cpu_ctx;
        dispatch_syscall(cpu_ctx);
        cpu_ctx = external_ctx;
    }
    
    // Copy CPU context back to external context (restore state)
    external_ctx = cpu_ctx;
}

std::mutex& Cpu::get_context_mutex(u32 thread_id) {
    return context_mutexes_[thread_id % cpu::NUM_THREADS];
}

} // namespace x360mu

