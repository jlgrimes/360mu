#pragma once

#include <atomic>

namespace x360mu {

/**
 * Feature Flags for runtime configuration of emulator behavior.
 * 
 * Usage:
 *   if (FeatureFlags::jit_trace_memory) { ... }
 *   FeatureFlags::jit_trace_memory = true;  // Enable at runtime
 * 
 * All flags default to false (disabled) for production performance.
 * Enable flags via JNI, debug menu, or at startup for development.
 */
struct FeatureFlags {
    // === JIT Debug Flags ===
    
    // Trace all memory accesses (very verbose, major perf impact)
    static inline std::atomic<bool> jit_trace_memory{false};
    
    // Trace memory accesses in mirror range (0x20000000-0x7FFFFFFF)
    static inline std::atomic<bool> jit_trace_mirror_access{false};
    
    // Trace memory accesses near 512MB boundary
    static inline std::atomic<bool> jit_trace_boundary_access{false};
    
    // Trace block execution (logs every N blocks)
    static inline std::atomic<bool> jit_trace_blocks{false};
    
    // Trace MMIO reads/writes
    static inline std::atomic<bool> jit_trace_mmio{false};
    
    // === GPU Debug Flags ===
    
    // Trace GPU register writes
    static inline std::atomic<bool> gpu_trace_registers{false};
    
    // Trace shader compilation
    static inline std::atomic<bool> gpu_trace_shaders{false};
    
    // Trace draw calls
    static inline std::atomic<bool> gpu_trace_draws{false};
    
    // === Kernel Debug Flags ===
    
    // Trace syscalls
    static inline std::atomic<bool> kernel_trace_syscalls{false};
    
    // Trace threading operations
    static inline std::atomic<bool> kernel_trace_threads{false};
    
    // Trace file I/O
    static inline std::atomic<bool> kernel_trace_files{false};
    
    // === Performance Flags ===
    
    // Skip shader cache (force recompile)
    static inline std::atomic<bool> skip_shader_cache{false};
    
    // Skip block cache (force recompile)  
    static inline std::atomic<bool> skip_block_cache{false};
    
    // === Compatibility Flags ===
    
    // Use slow path for all memory (disable fastmem)
    static inline std::atomic<bool> disable_fastmem{false};
    
    // Force interpreter mode (no JIT)
    static inline std::atomic<bool> force_interpreter{false};
};

// Convenience macros for conditional logging
#define FF_LOG_IF(flag, tag, ...) \
    do { if (::x360mu::FeatureFlags::flag.load(std::memory_order_relaxed)) { \
        __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__); \
    } } while(0)

#define FF_LOG_ERROR_IF(flag, tag, ...) \
    do { if (::x360mu::FeatureFlags::flag.load(std::memory_order_relaxed)) { \
        __android_log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__); \
    } } while(0)

} // namespace x360mu
