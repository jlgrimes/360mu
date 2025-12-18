/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * JIT Compiler - PowerPC to ARM64 Translation
 * 
 * This is the core of the emulator's performance, dynamically translating
 * PowerPC code to native ARM64 for near-native execution speed.
 */

#include "jit.h"
#include "../../memory/memory.h"
#include "../xenon/cpu.h"
#include <thread>
#include <unordered_map>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/mman.h>
#define LOG_TAG "360mu-jit"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#include <sys/mman.h>
#define LOGI(...) printf("[JIT] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[JIT ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

// Maximum instructions per compiled block
constexpr u32 MAX_BLOCK_INSTRUCTIONS = 256;

// GPU MMIO base address for runtime checking
constexpr GuestAddr GPU_MMIO_BASE = 0x7FC00000;

//=============================================================================
// C-style helper functions for memory access (callable from JIT)
// These bypass fastmem and go through Memory class for proper MMIO handling
//=============================================================================

extern "C" void jit_mmio_write_u8(void* mem, GuestAddr addr, u8 value) {
    static_cast<Memory*>(mem)->write_u8(addr, value);
}

extern "C" void jit_mmio_write_u16(void* mem, GuestAddr addr, u16 value) {
    static_cast<Memory*>(mem)->write_u16(addr, value);
}

// Debug helper to trace store addresses
extern "C" void jit_trace_store(GuestAddr addr) {
    static int trace_count = 0;
    // Log stores to GPU-ish range
    if (addr >= 0x7F000000 && addr < 0x80000000) {
        if (trace_count < 20) {
            LOGI("Store to potential MMIO: addr=0x%08llX", (unsigned long long)addr);
            trace_count++;
        }
    }
}

// #region agent log - Hypothesis AA: Track JIT writes to event addresses
// Returns 1 if this write should be SKIPPED (blocked), 0 otherwise
extern "C" int jit_trace_event_store(GuestAddr addr, u32 value) {
    // Track writes to our problematic event addresses (both virtual and physical)
    GuestAddr phys = addr;
    if (addr >= 0x80000000 && addr < 0xA0000000) {
        phys = addr & 0x1FFFFFFF;
    }
    
    // Check if in event range for logging
    bool is_event_range = (phys >= 0x1FFEFC20 && phys <= 0x1FFEFC60);
    
    // Only block zero writes to SIGNAL_STATE fields (offset +4 from event base)
    // Known signal_state addresses: 0x1FFEFC30, 0x1FFEFC44, 0x1FFEFC48, 0x1FFEFC4C
    bool is_signal_state = (phys == 0x1FFEFC30 || phys == 0x1FFEFC44 || 
                            phys == 0x1FFEFC48 || phys == 0x1FFEFC4C ||
                            phys == 0x1FFEFC50 || phys == 0x1FFEFC54);
    
    if (is_event_range) {
        static int event_store_log = 0;
        bool should_block = (is_signal_state && value == 0);
        if (event_store_log++ < 200) {
            FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
            if (f) { 
                fprintf(f, "{\"hypothesisId\":\"AA\",\"location\":\"jit_trace_event_store\",\"message\":\"JIT_EVENT_STORE\",\"data\":{\"virt\":\"%08X\",\"phys\":\"%08X\",\"value\":%u,\"call\":%d,\"is_signal\":%d,\"blocked\":%d}}\n", 
                    (u32)addr, (u32)phys, value, event_store_log, is_signal_state ? 1 : 0, should_block ? 1 : 0); 
                fclose(f); 
            }
        }
        
        // FIX: Block zero writes ONLY to signal_state fields
        if (should_block) {
            return 1;  // Skip this write
        }
    }
    return 0;  // Allow write
}
// #endregion

extern "C" void jit_mmio_write_u32(void* mem, GuestAddr addr, u32 value) {
    static int call_count = 0;
    
    // #region agent log - Hypothesis M: Track MMIO addresses and ring buffer writes
    // Ring buffer registers are at offset 0x0700-0x070F (addr 0x7FC01C00-0x7FC01C3F)
    static int mmio_log_count = 0;
    GuestAddr phys_addr = addr;
    if (addr >= 0x80000000 && addr < 0xA0000000) {
        phys_addr = addr & 0x1FFFFFFF;  // Translate to physical
    }
    u32 gpu_offset = (phys_addr >= 0x7FC00000) ? (phys_addr - 0x7FC00000) / 4 : 0;
    
    // Log ring buffer writes specifically (offset 0x700-0x70F)
    if (gpu_offset >= 0x700 && gpu_offset <= 0x70F) {
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { fprintf(f, "{\"hypothesisId\":\"M\",\"location\":\"jit_mmio_write_u32\",\"message\":\"RING BUFFER WRITE\",\"data\":{\"virt_addr\":\"%08X\",\"phys_addr\":\"%08X\",\"offset\":%u,\"value\":%u}}\n", addr, phys_addr, gpu_offset, value); fclose(f); }
    }
    // Log first 50 MMIO writes for debugging
    else if (mmio_log_count++ < 50) {
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { fprintf(f, "{\"hypothesisId\":\"M\",\"location\":\"jit_mmio_write_u32\",\"message\":\"MMIO write\",\"data\":{\"virt_addr\":\"%08X\",\"phys_addr\":\"%08X\",\"offset\":%u,\"value\":%u}}\n", addr, phys_addr, gpu_offset, value); fclose(f); }
    }
    // #endregion
    
    if (call_count < 10) {
        LOGI("MMIO write_u32: addr=0x%08X value=0x%08X", addr, value);
        call_count++;
    }
    static_cast<Memory*>(mem)->write_u32(addr, value);
}

extern "C" void jit_mmio_write_u64(void* mem, GuestAddr addr, u64 value) {
    static_cast<Memory*>(mem)->write_u64(addr, value);
}

extern "C" u8 jit_mmio_read_u8(void* mem, GuestAddr addr) {
    return static_cast<Memory*>(mem)->read_u8(addr);
}

extern "C" u16 jit_mmio_read_u16(void* mem, GuestAddr addr) {
    return static_cast<Memory*>(mem)->read_u16(addr);
}

extern "C" u32 jit_mmio_read_u32(void* mem, GuestAddr addr) {
    u32 value = static_cast<Memory*>(mem)->read_u32(addr);
    
    // SPIN LOOP ANALYSIS: Track frequent reads to detect polling
    static std::unordered_map<GuestAddr, u32> read_counts;
    static int total_reads = 0;
    static int logged_addrs = 0;
    
    read_counts[addr]++;
    total_reads++;
    
    // Log addresses being read more than 100 times (likely polling)
    if (read_counts[addr] == 100 && logged_addrs < 20) {
        logged_addrs++;
        LOGI("POLL DETECTED: addr=0x%08X read 100+ times, value=0x%08X", (u32)addr, value);
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { 
            fprintf(f, "{\"hypothesisId\":\"POLL\",\"message\":\"POLLING_ADDR\",\"data\":{\"addr\":\"%08X\",\"value\":\"%08X\",\"count\":100}}\n", 
                    (u32)addr, value); 
            fclose(f); 
        }
    }
    
    return value;
}

extern "C" u64 jit_mmio_read_u64(void* mem, GuestAddr addr) {
    return static_cast<Memory*>(mem)->read_u64(addr);
}

// Size of temporary code buffer
constexpr size_t TEMP_BUFFER_SIZE = 64 * 1024;

// Minimum cycles before checking for interrupts
constexpr u64 CYCLES_PER_BLOCK = 100;

//=============================================================================
// Register Allocator Implementation
//=============================================================================

RegisterAllocator::RegisterAllocator() {
    reset();
}

void RegisterAllocator::reset() {
    ppc_to_arm_.fill(INVALID_REG);
    arm_to_ppc_.fill(INVALID_REG);
    dirty_.reset();
    temp_available_.set();  // All temps available
    
    // Reserve certain temp registers
    temp_available_[arm64::X16] = false;  // IP0 - used for address calculation
    temp_available_[arm64::X17] = false;  // IP1 - used for address calculation
}

int RegisterAllocator::get_gpr(int ppc_reg) {
    // For now, use simple direct mapping
    // All GPRs are loaded/stored from context on each access
    return arm64::X0;  // Will be loaded by caller
}

void RegisterAllocator::mark_dirty(int ppc_reg) {
    if (ppc_to_arm_[ppc_reg] != INVALID_REG) {
        dirty_.set(ppc_reg);
    }
}

void RegisterAllocator::flush_all(ARM64Emitter& emit) {
    // For simple implementation, nothing to flush as we use immediate load/store
}

void RegisterAllocator::flush_gpr(ARM64Emitter& emit, int ppc_reg) {
    // For simple implementation, nothing to flush
}

int RegisterAllocator::alloc_temp() {
    for (int i = 0; i < 16; i++) {
        if (temp_available_[i]) {
            temp_available_[i] = false;
            return i;
        }
    }
    return arm64::X0;  // Fallback
}

void RegisterAllocator::free_temp(int arm_reg) {
    if (arm_reg >= 0 && arm_reg < 16) {
        temp_available_[arm_reg] = true;
    }
}

bool RegisterAllocator::is_cached(int ppc_reg) const {
    return ppc_to_arm_[ppc_reg] != INVALID_REG;
}

//=============================================================================
// JIT Compiler Core
//=============================================================================

JitCompiler::JitCompiler() = default;

JitCompiler::~JitCompiler() {
    shutdown();
}

Status JitCompiler::initialize(Memory* memory, u64 cache_size) {
    memory_ = memory;
    cache_size_ = cache_size;
    
    // Allocate executable memory for code cache
#ifdef __aarch64__
    code_cache_ = static_cast<u8*>(mmap(
        nullptr, cache_size,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    ));
    
    if (code_cache_ == MAP_FAILED) {
        LOGE("Failed to allocate JIT code cache (%llu bytes)", (unsigned long long)cache_size);
        return Status::OutOfMemory;
    }
    
    // Try to set up fastmem
    fastmem_base_ = static_cast<u8*>(memory_->get_fastmem_base());
    fastmem_enabled_ = (fastmem_base_ != nullptr);
    
    if (fastmem_enabled_) {
        LOGI("Fastmem enabled at %p (0x%llX)", fastmem_base_, 
             (unsigned long long)reinterpret_cast<u64>(fastmem_base_));
    } else {
        LOGE("Fastmem NOT available - JIT will fall back to interpreter");
    }
#else
    // Non-ARM64 fallback (for testing on x86)
    code_cache_ = new u8[cache_size];
    fastmem_enabled_ = false;
#endif
    
    code_write_ptr_ = code_cache_;
    
    // Generate dispatcher and exit stub
    generate_dispatcher();
    generate_exit_stub();
    
    LOGI("JIT initialized with %lluMB cache", (unsigned long long)(cache_size / (1024 * 1024)));
    return Status::Ok;
}

void JitCompiler::shutdown() {
    // Clear block map
    {
        std::lock_guard<std::mutex> lock(block_map_mutex_);
        for (auto& [addr, block] : block_map_) {
            delete block;
        }
        block_map_.clear();
    }
    
    // Free code cache
    if (code_cache_) {
#ifdef __aarch64__
        munmap(code_cache_, cache_size_);
#else
        delete[] code_cache_;
#endif
        code_cache_ = nullptr;
    }
}

u8* JitCompiler::get_memory_base() const {
    return fastmem_enabled_ ? fastmem_base_ : nullptr;
}

u64 JitCompiler::execute(ThreadContext& ctx, u64 cycles) {
    u64 cycles_executed = 0;
    
    // #region agent log - NEW HYPOTHESIS I: Check JIT execution path
    static int jit_exec_log = 0;
    if (jit_exec_log++ < 30) {
        FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
        if (f) { fprintf(f, "{\"hypothesisId\":\"I\",\"location\":\"jit_compiler.cpp:execute\",\"message\":\"JIT execute entry\",\"data\":{\"call\":%d,\"fastmem_enabled\":%d,\"has_dispatcher\":%d,\"pc_before\":%u}}\n", jit_exec_log, fastmem_enabled_, dispatcher_!=nullptr, (u32)ctx.pc); fclose(f); }
    }
    // #endregion
    
#ifdef __aarch64__
    // JIT requires fastmem to be enabled - without it, memory accesses will crash
    if (!fastmem_enabled_) {
        // #region agent log - Hypothesis I: fastmem not enabled
        static int no_fastmem_log = 0;
        if (no_fastmem_log++ < 5) {
            FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
            if (f) { fprintf(f, "{\"hypothesisId\":\"I\",\"location\":\"jit_compiler.cpp:execute\",\"message\":\"FASTMEM NOT ENABLED - falling back\",\"data\":{}}\n"); fclose(f); }
        }
        // #endregion
        // Return 0 to signal CPU should fall back to interpreter
        // Don't set interrupted - that's for syscalls
        return 0;
    }
    
    // Run the dispatcher which will execute compiled code
    if (dispatcher_) {
        ctx.running = true;
        ctx.interrupted = false;
        
        // #region agent log - Hypothesis I: About to enter block loop
        static int loop_entry_log = 0;
        GuestAddr start_pc = ctx.pc;
        // #endregion
        
        // Store cycle limit in context or use register
        while (ctx.running && !ctx.interrupted && cycles_executed < cycles) {
            // Check for PC=0 termination (used for DPC return)
            // When a DPC routine executes 'blr' with LR=0, PC becomes 0
            if (ctx.pc == 0) {
                ctx.running = false;
                break;
            }
            
            // Look up or compile block
            CompiledBlock* block = compile_block(ctx.pc);
            if (!block) {
                LOGE("Failed to compile block at %08llX", (unsigned long long)ctx.pc);
                ctx.interrupted = true;
                break;
            }
            
            // #region agent log - Hypothesis I: Block compiled, about to execute
            static int block_exec_log = 0;
            GuestAddr pc_before = ctx.pc;
            if (block_exec_log++ < 20) {
                FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
                if (f) { fprintf(f, "{\"hypothesisId\":\"I\",\"location\":\"jit_compiler.cpp:execute\",\"message\":\"executing block\",\"data\":{\"block_num\":%d,\"pc\":%u,\"block_size\":%u}}\n", block_exec_log, (u32)ctx.pc, block->size); fclose(f); }
            }
            // #endregion
            
            // Execute the block
            using BlockFn = void(*)(ThreadContext*, u8*);
            BlockFn fn = reinterpret_cast<BlockFn>(block->code);
            
            // Execute the block (fastmem_base is now embedded in block code)
            fn(&ctx, nullptr);
            
            // #region agent log - Hypothesis I: After block execution
            if (block_exec_log <= 20) {
                FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
                if (f) { fprintf(f, "{\"hypothesisId\":\"I\",\"location\":\"jit_compiler.cpp:execute\",\"message\":\"after block\",\"data\":{\"pc_before\":%u,\"pc_after\":%u,\"running\":%d,\"interrupted\":%d}}\n", (u32)pc_before, (u32)ctx.pc, ctx.running, ctx.interrupted); fclose(f); }
            }
            // #endregion
            
            // #region agent log - Hypothesis K: Detect and BREAK spin loops in JIT
            static GuestAddr jit_last_pc = 0;
            static int jit_spin_count = 0;
            if (ctx.pc == pc_before) {
                jit_spin_count++;
                if (jit_spin_count == 50 || jit_spin_count == 500 || jit_spin_count == 5000) {
                    FILE* f = fopen("/data/data/com.x360mu/files/debug.log", "a");
                    if (f) { fprintf(f, "{\"hypothesisId\":\"K\",\"location\":\"jit_compiler.cpp:execute\",\"message\":\"JIT SPIN LOOP\",\"data\":{\"pc\":%u,\"count\":%d,\"r3\":%llu,\"r4\":%llu,\"r5\":%llu}}\n", (u32)ctx.pc, jit_spin_count, ctx.gpr[3], ctx.gpr[4], ctx.gpr[5]); fclose(f); }
                }
                
                // DISABLED: Breaking spin loops by advancing PC causes bad branches
                // Instead, rely on event signaling to naturally break loops
                // The game will spin but our event faking should eventually let it proceed
                if (jit_spin_count >= 10000) {
                    // Just yield to let other threads run and our worker set events
                    std::this_thread::yield();
                    jit_spin_count = 0;  // Reset counter but don't break the loop
                }
            } else {
                jit_spin_count = 0;
            }
            jit_last_pc = ctx.pc;
            // #endregion
            
            cycles_executed += block->size;
            block->execution_count++;
        }
    }
#else
    // Fallback to interpreter on non-ARM64 platforms
    LOGE("JIT only supported on ARM64");
    ctx.interrupted = true;
#endif
    
    return cycles_executed;
}

void JitCompiler::invalidate(GuestAddr addr, u32 size) {
    std::lock_guard<std::mutex> lock(block_map_mutex_);
    
    // Find and remove any blocks that overlap with the modified region
    GuestAddr end_addr = addr + size;
    
    for (auto it = block_map_.begin(); it != block_map_.end();) {
        CompiledBlock* block = it->second;
        
        if (block->start_addr < end_addr && block->end_addr > addr) {
            // Block overlaps with invalidated region
            unlink_block(block);
            delete block;
            it = block_map_.erase(it);
        } else {
            ++it;
        }
    }
}

void JitCompiler::flush_cache() {
    std::lock_guard<std::mutex> lock(block_map_mutex_);
    
    for (auto& [addr, block] : block_map_) {
        delete block;
    }
    block_map_.clear();
    
    // Reset code write pointer (leave room for dispatcher)
    code_write_ptr_ = code_cache_ + 4096;
    stats_ = {};
}

CompiledBlock* JitCompiler::compile_block(GuestAddr addr) {
    std::lock_guard<std::mutex> lock(block_map_mutex_);
    
    // Check cache first
    auto it = block_map_.find(addr);
    if (it != block_map_.end()) {
        stats_.cache_hits++;
        return it->second;
    }
    
    stats_.cache_misses++;
    
    CompiledBlock* block = compile_block_unlocked(addr);
    
    // Try to link this block to others
    if (block) {
        try_link_block(block);
    }
    
    return block;
}

bool JitCompiler::is_block_ending(const DecodedInst& inst) const {
    switch (inst.type) {
        case DecodedInst::Type::Branch:
        case DecodedInst::Type::BranchConditional:
        case DecodedInst::Type::BranchLink:
        case DecodedInst::Type::SC:
        case DecodedInst::Type::RFI:
            return true;
        default:
            return false;
    }
}

void JitCompiler::compile_instruction(ARM64Emitter& emit, ThreadContext& ctx_template,
                                       const DecodedInst& inst, GuestAddr pc) {
    switch (inst.type) {
        case DecodedInst::Type::Add:
        case DecodedInst::Type::AddCarrying:
        case DecodedInst::Type::AddExtended:
            compile_add(emit, inst);
            break;
            
        case DecodedInst::Type::Sub:
        case DecodedInst::Type::SubCarrying:
        case DecodedInst::Type::SubExtended:
            compile_sub(emit, inst);
            break;
            
        case DecodedInst::Type::Mul:
        case DecodedInst::Type::MulHigh:
            compile_mul(emit, inst);
            break;
            
        case DecodedInst::Type::Div:
            compile_div(emit, inst);
            break;
            
        case DecodedInst::Type::And:
        case DecodedInst::Type::Or:
        case DecodedInst::Type::Xor:
        case DecodedInst::Type::Nand:
        case DecodedInst::Type::Nor:
            compile_logical(emit, inst);
            break;
            
        case DecodedInst::Type::Shift:
            compile_shift(emit, inst);
            break;
            
        case DecodedInst::Type::Rotate:
            compile_rotate(emit, inst);
            break;
            
        case DecodedInst::Type::Compare:
        case DecodedInst::Type::CompareLI:
            compile_compare(emit, inst);
            break;
            
        case DecodedInst::Type::Load:
        case DecodedInst::Type::LoadUpdate:
            compile_load(emit, inst);
            break;
            
        case DecodedInst::Type::Store:
        case DecodedInst::Type::StoreUpdate:
            compile_store(emit, inst);
            break;
            
        case DecodedInst::Type::LoadMultiple:
            compile_load_multiple(emit, inst);
            break;
            
        case DecodedInst::Type::StoreMultiple:
            compile_store_multiple(emit, inst);
            break;
            
        case DecodedInst::Type::Branch:
            compile_branch(emit, inst, pc, nullptr);
            break;
            
        case DecodedInst::Type::BranchConditional:
            compile_branch_conditional(emit, inst, pc, nullptr);
            break;
            
        case DecodedInst::Type::BranchLink:
            // blr (opcode 19, xo 16, bo=20) or bctr (opcode 19, xo 528)
            compile_branch_conditional(emit, inst, pc, nullptr);
            break;
            
        case DecodedInst::Type::FAdd:
        case DecodedInst::Type::FSub:
        case DecodedInst::Type::FMul:
        case DecodedInst::Type::FDiv:
        case DecodedInst::Type::FMadd:
            compile_float(emit, inst);
            break;
            
        case DecodedInst::Type::VAdd:
        case DecodedInst::Type::VSub:
        case DecodedInst::Type::VMul:
        case DecodedInst::Type::VLogical:
            compile_vector(emit, inst);
            break;
            
        case DecodedInst::Type::SC:
            compile_syscall(emit, inst);
            break;
            
        case DecodedInst::Type::MTspr:
            compile_mtspr(emit, inst);
            break;
            
        case DecodedInst::Type::MFspr:
            compile_mfspr(emit, inst);
            break;
            
        case DecodedInst::Type::CRLogical:
            compile_cr_logical(emit, inst);
            break;
            
        case DecodedInst::Type::MTcrf:
            compile_mtcrf(emit, inst);
            break;
            
        case DecodedInst::Type::MFcr:
            compile_mfcr(emit, inst);
            break;
            
        case DecodedInst::Type::SYNC:
            // Full memory barrier (PowerPC sync L=0)
            // DMB SY (option 15) - full system data memory barrier
            emit.DMB(15);  // SY - full system barrier
            break;
            
        case DecodedInst::Type::LWSYNC:
            // Lightweight sync (PowerPC sync L=1) - acquire-release semantics
            // DMB ISH (option 11) - inner shareable barrier
            emit.DMB(11);  // ISH - inner shareable (sufficient for SMP)
            break;
            
        case DecodedInst::Type::EIEIO:
            // Enforce In-Order Execution of I/O - store ordering for MMIO
            // DMB ISHST (option 10) - inner shareable store barrier
            emit.DMB(10);  // ISHST - inner shareable store-store barrier
            break;
            
        case DecodedInst::Type::ISYNC:
            // Instruction synchronize - ensures instruction fetch is synchronized
            // ISB - instruction synchronization barrier
            emit.ISB();
            break;
            
        case DecodedInst::Type::DCBF:
        case DecodedInst::Type::DCBST:
        case DecodedInst::Type::DCBT:
        case DecodedInst::Type::ICBI:
            // Cache operations - mostly NOPs for emulator
            emit.NOP();
            break;
            
        case DecodedInst::Type::DCBZ:
            // Data Cache Block Zero - zeros 32 bytes aligned to 32-byte boundary
            compile_dcbz(emit, inst);
            break;
            
        default:
            // Fallback: NOP for unknown instructions
            emit.NOP();
            stats_.interpreter_fallbacks++;
            break;
    }
    
    stats_.instructions_executed++;
}

//=============================================================================
// Integer Instruction Compilation
//=============================================================================

void JitCompiler::compile_add(ARM64Emitter& emit, const DecodedInst& inst) {
    if (inst.opcode == 14) { // addi
        if (inst.ra == 0) {
            // li rD, SIMM
            emit.MOV_imm(arm64::X0, static_cast<u64>(static_cast<s64>(inst.simm)));
        } else {
            load_gpr(emit, arm64::X0, inst.ra);
            if (inst.simm >= 0 && inst.simm < 4096) {
                emit.ADD_imm(arm64::X0, arm64::X0, inst.simm);
            } else if (inst.simm < 0 && -inst.simm < 4096) {
                emit.SUB_imm(arm64::X0, arm64::X0, -inst.simm);
            } else {
                emit.MOV_imm(arm64::X1, static_cast<u64>(static_cast<s64>(inst.simm)));
                emit.ADD(arm64::X0, arm64::X0, arm64::X1);
            }
        }
        store_gpr(emit, inst.rd, arm64::X0);
    }
    else if (inst.opcode == 15) { // addis
        s64 shifted = static_cast<s64>(inst.simm) << 16;
        if (inst.ra == 0) {
            emit.MOV_imm(arm64::X0, static_cast<u64>(shifted));
        } else {
            load_gpr(emit, arm64::X0, inst.ra);
            emit.MOV_imm(arm64::X1, static_cast<u64>(shifted));
            emit.ADD(arm64::X0, arm64::X0, arm64::X1);
        }
        store_gpr(emit, inst.rd, arm64::X0);
    }
    else if (inst.opcode == 12) { // addic
        load_gpr(emit, arm64::X0, inst.ra);
        emit.MOV_imm(arm64::X1, static_cast<u64>(static_cast<s64>(inst.simm)));
        emit.ADDS(arm64::X0, arm64::X0, arm64::X1);
        store_gpr(emit, inst.rd, arm64::X0);
        // Store carry to XER.CA
        emit.CSET(arm64::X2, arm64_cond::CS);
        emit.LDR(arm64::X3, arm64::CTX_REG, ctx_offset_xer());
        emit.BIC(arm64::X3, arm64::X3, arm64::X2);  // Clear CA bit position
        emit.ORR(arm64::X3, arm64::X3, arm64::X2);  // Set new CA
        emit.STR(arm64::X3, arm64::CTX_REG, ctx_offset_xer());
    }
    else if (inst.opcode == 31) { // Extended opcodes
        load_gpr(emit, arm64::X0, inst.ra);
        load_gpr(emit, arm64::X1, inst.rb);
        
        switch (inst.xo) {
            case 266: // add
                emit.ADD(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 10: // addc
                emit.ADDS(arm64::X0, arm64::X0, arm64::X1);
                emit.CSET(arm64::X2, arm64_cond::CS);
                // Store CA in XER (simplified)
                break;
            case 138: // adde
                // Load XER.CA and add with carry
                emit.ADC(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 202: // addze
                load_gpr(emit, arm64::X0, inst.ra);
                // Add with carry from XER
                emit.ADC(arm64::X0, arm64::X0, arm64::XZR);
                break;
            case 234: // addme
                load_gpr(emit, arm64::X0, inst.ra);
                emit.MOV_imm(arm64::X1, ~0ULL);
                emit.ADC(arm64::X0, arm64::X0, arm64::X1);
                break;
        }
        
        store_gpr(emit, inst.rd, arm64::X0);
        
        if (inst.rc) {
            compile_cr_update(emit, 0, arm64::X0);
        }
    }
}

void JitCompiler::compile_sub(ARM64Emitter& emit, const DecodedInst& inst) {
    if (inst.opcode == 8) { // subfic
        emit.MOV_imm(arm64::X0, static_cast<u64>(static_cast<s64>(inst.simm)));
        load_gpr(emit, arm64::X1, inst.ra);
        emit.SUBS(arm64::X0, arm64::X0, arm64::X1);
        store_gpr(emit, inst.rd, arm64::X0);
        // Set CA
        emit.CSET(arm64::X2, arm64_cond::CS);
    }
    else if (inst.opcode == 31) {
        load_gpr(emit, arm64::X0, inst.rb);
        load_gpr(emit, arm64::X1, inst.ra);
        
        switch (inst.xo) {
            case 40: // subf (rb - ra)
                emit.SUB(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 8: // subfc
                emit.SUBS(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 136: // subfe
                // Subtract with borrow
                emit.SBC(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 200: // subfze
                load_gpr(emit, arm64::X0, inst.ra);
                emit.NEG(arm64::X0, arm64::X0);
                // Add CA-1
                break;
            case 232: // subfme
                load_gpr(emit, arm64::X0, inst.ra);
                emit.MOV_imm(arm64::X1, ~0ULL);
                emit.SBC(arm64::X0, arm64::X1, arm64::X0);
                break;
            case 104: // neg
                load_gpr(emit, arm64::X0, inst.ra);
                emit.NEG(arm64::X0, arm64::X0);
                break;
        }
        
        store_gpr(emit, inst.rd, arm64::X0);
        
        if (inst.rc) {
            compile_cr_update(emit, 0, arm64::X0);
        }
    }
}

void JitCompiler::compile_mul(ARM64Emitter& emit, const DecodedInst& inst) {
    if (inst.opcode == 7) { // mulli
        load_gpr(emit, arm64::X0, inst.ra);
        emit.MOV_imm(arm64::X1, static_cast<u64>(static_cast<s64>(inst.simm)));
        emit.MUL(arm64::X0, arm64::X0, arm64::X1);
        store_gpr(emit, inst.rd, arm64::X0);
    }
    else if (inst.opcode == 31) {
        load_gpr(emit, arm64::X0, inst.ra);
        load_gpr(emit, arm64::X1, inst.rb);
        
        switch (inst.xo) {
            case 235: // mullw (32-bit signed)
                emit.SXTW(arm64::X0, arm64::X0);
                emit.SXTW(arm64::X1, arm64::X1);
                emit.MUL(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 233: // mulld (64-bit)
                emit.MUL(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 75: // mulhw (high 32 bits of 32x32 signed)
                emit.SXTW(arm64::X0, arm64::X0);
                emit.SXTW(arm64::X1, arm64::X1);
                emit.SMULH(arm64::X0, arm64::X0, arm64::X1);
                emit.LSR_imm(arm64::X0, arm64::X0, 32);
                break;
            case 11: // mulhwu (high 32 bits of 32x32 unsigned)
                emit.UXTW(arm64::X0, arm64::X0);
                emit.UXTW(arm64::X1, arm64::X1);
                emit.UMULH(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 73: // mulhd (high 64 bits of 64x64 signed)
                emit.SMULH(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 9: // mulhdu (high 64 bits of 64x64 unsigned)
                emit.UMULH(arm64::X0, arm64::X0, arm64::X1);
                break;
        }
        
        store_gpr(emit, inst.rd, arm64::X0);
        
        if (inst.rc) {
            compile_cr_update(emit, 0, arm64::X0);
        }
    }
}

void JitCompiler::compile_div(ARM64Emitter& emit, const DecodedInst& inst) {
    load_gpr(emit, arm64::X0, inst.ra);
    load_gpr(emit, arm64::X1, inst.rb);
    
    // Check for division by zero - if zero, result is undefined
    // We'll emit a conditional to skip if zero
    u8* skip_div = emit.current();
    emit.CBZ(arm64::X1, 0);  // Will patch
    
    switch (inst.xo) {
        case 491: // divw (signed 32-bit)
            emit.SXTW(arm64::X0, arm64::X0);
            emit.SXTW(arm64::X1, arm64::X1);
            emit.SDIV(arm64::X0, arm64::X0, arm64::X1);
            break;
        case 459: // divwu (unsigned 32-bit)
            emit.UXTW(arm64::X0, arm64::X0);
            emit.UXTW(arm64::X1, arm64::X1);
            emit.UDIV(arm64::X0, arm64::X0, arm64::X1);
            break;
        case 489: // divd (signed 64-bit)
            emit.SDIV(arm64::X0, arm64::X0, arm64::X1);
            break;
        case 457: // divdu (unsigned 64-bit)
            emit.UDIV(arm64::X0, arm64::X0, arm64::X1);
            break;
    }
    
    // Patch the skip
    s64 skip_offset = emit.current() - skip_div;
    emit.patch_branch(reinterpret_cast<u32*>(skip_div), emit.current());
    
    store_gpr(emit, inst.rd, arm64::X0);
    
    if (inst.rc) {
        compile_cr_update(emit, 0, arm64::X0);
    }
}

void JitCompiler::compile_logical(ARM64Emitter& emit, const DecodedInst& inst) {
    if (inst.opcode == 24) { // ori
        if (inst.rs == 0 && inst.ra == 0 && inst.uimm == 0) {
            // NOP - ori 0,0,0
            emit.NOP();
            return;
        }
        load_gpr(emit, arm64::X0, inst.rs);
        if (inst.uimm != 0) {
            emit.MOV_imm(arm64::X1, inst.uimm);
            emit.ORR(arm64::X0, arm64::X0, arm64::X1);
        }
        store_gpr(emit, inst.ra, arm64::X0);
    }
    else if (inst.opcode == 25) { // oris
        load_gpr(emit, arm64::X0, inst.rs);
        emit.MOV_imm(arm64::X1, static_cast<u64>(inst.uimm) << 16);
        emit.ORR(arm64::X0, arm64::X0, arm64::X1);
        store_gpr(emit, inst.ra, arm64::X0);
    }
    else if (inst.opcode == 26) { // xori
        load_gpr(emit, arm64::X0, inst.rs);
        if (inst.uimm != 0) {
            emit.MOV_imm(arm64::X1, inst.uimm);
            emit.EOR(arm64::X0, arm64::X0, arm64::X1);
        }
        store_gpr(emit, inst.ra, arm64::X0);
    }
    else if (inst.opcode == 27) { // xoris
        load_gpr(emit, arm64::X0, inst.rs);
        emit.MOV_imm(arm64::X1, static_cast<u64>(inst.uimm) << 16);
        emit.EOR(arm64::X0, arm64::X0, arm64::X1);
        store_gpr(emit, inst.ra, arm64::X0);
    }
    else if (inst.opcode == 28) { // andi.
        load_gpr(emit, arm64::X0, inst.rs);
        emit.MOV_imm(arm64::X1, inst.uimm);
        emit.AND(arm64::X0, arm64::X0, arm64::X1);
        store_gpr(emit, inst.ra, arm64::X0);
        compile_cr_update(emit, 0, arm64::X0);
    }
    else if (inst.opcode == 29) { // andis.
        load_gpr(emit, arm64::X0, inst.rs);
        emit.MOV_imm(arm64::X1, static_cast<u64>(inst.uimm) << 16);
        emit.AND(arm64::X0, arm64::X0, arm64::X1);
        store_gpr(emit, inst.ra, arm64::X0);
        compile_cr_update(emit, 0, arm64::X0);
    }
    else if (inst.opcode == 31) {
        switch (inst.xo) {
            case 28: // and
                load_gpr(emit, arm64::X0, inst.rs);
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND(arm64::X0, arm64::X0, arm64::X1);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 60: // andc
                load_gpr(emit, arm64::X0, inst.rs);
                load_gpr(emit, arm64::X1, inst.rb);
                emit.BIC(arm64::X0, arm64::X0, arm64::X1);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 444: // or
                load_gpr(emit, arm64::X0, inst.rs);
                load_gpr(emit, arm64::X1, inst.rb);
                emit.ORR(arm64::X0, arm64::X0, arm64::X1);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 412: // orc
                load_gpr(emit, arm64::X0, inst.rs);
                load_gpr(emit, arm64::X1, inst.rb);
                emit.ORN(arm64::X0, arm64::X0, arm64::X1);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 316: // xor
                load_gpr(emit, arm64::X0, inst.rs);
                load_gpr(emit, arm64::X1, inst.rb);
                emit.EOR(arm64::X0, arm64::X0, arm64::X1);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 284: // eqv (xor + not)
                load_gpr(emit, arm64::X0, inst.rs);
                load_gpr(emit, arm64::X1, inst.rb);
                emit.EON(arm64::X0, arm64::X0, arm64::X1);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 124: // nor
                load_gpr(emit, arm64::X0, inst.rs);
                load_gpr(emit, arm64::X1, inst.rb);
                emit.ORR(arm64::X0, arm64::X0, arm64::X1);
                emit.MOV_imm(arm64::X1, ~0ULL);
                emit.EOR(arm64::X0, arm64::X0, arm64::X1);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 476: // nand
                load_gpr(emit, arm64::X0, inst.rs);
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND(arm64::X0, arm64::X0, arm64::X1);
                emit.MOV_imm(arm64::X1, ~0ULL);
                emit.EOR(arm64::X0, arm64::X0, arm64::X1);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 26: // cntlzw
                load_gpr(emit, arm64::X0, inst.rs);
                emit.UXTW(arm64::X0, arm64::X0);
                emit.CLZ(arm64::X0, arm64::X0);
                emit.SUB_imm(arm64::X0, arm64::X0, 32);  // Adjust for 64-bit CLZ
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 58: // cntlzd
                load_gpr(emit, arm64::X0, inst.rs);
                emit.CLZ(arm64::X0, arm64::X0);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 922: // extsh
                load_gpr(emit, arm64::X0, inst.rs);
                emit.SXTH(arm64::X0, arm64::X0);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 954: // extsb
                load_gpr(emit, arm64::X0, inst.rs);
                emit.SXTB(arm64::X0, arm64::X0);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
            case 986: // extsw
                load_gpr(emit, arm64::X0, inst.rs);
                emit.SXTW(arm64::X0, arm64::X0);
                store_gpr(emit, inst.ra, arm64::X0);
                break;
        }
        
        if (inst.rc) {
            compile_cr_update(emit, 0, arm64::X0);
        }
    }
}

void JitCompiler::compile_shift(ARM64Emitter& emit, const DecodedInst& inst) {
    load_gpr(emit, arm64::X0, inst.rs);
    
    if (inst.opcode == 31) {
        switch (inst.xo) {
            case 24: // slw (shift left word)
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND_imm(arm64::X1, arm64::X1, 0x3F);
                emit.LSL(arm64::X0, arm64::X0, arm64::X1);
                emit.UXTW(arm64::X0, arm64::X0);  // Clear upper 32 bits
                break;
            case 27: // sld (shift left doubleword)
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND_imm(arm64::X1, arm64::X1, 0x7F);
                emit.LSL(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 536: // srw (shift right word)
                load_gpr(emit, arm64::X1, inst.rb);
                emit.UXTW(arm64::X0, arm64::X0);
                emit.AND_imm(arm64::X1, arm64::X1, 0x3F);
                emit.LSR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 539: // srd (shift right doubleword)
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND_imm(arm64::X1, arm64::X1, 0x7F);
                emit.LSR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 792: // sraw (shift right algebraic word)
                load_gpr(emit, arm64::X1, inst.rb);
                emit.SXTW(arm64::X0, arm64::X0);
                emit.AND_imm(arm64::X1, arm64::X1, 0x3F);
                emit.ASR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 794: // srad (shift right algebraic doubleword)
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND_imm(arm64::X1, arm64::X1, 0x7F);
                emit.ASR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 824: // srawi (shift right algebraic word immediate)
                emit.SXTW(arm64::X0, arm64::X0);
                emit.ASR_imm(arm64::X0, arm64::X0, inst.sh);
                // Set XER.CA if any bits shifted out were 1 and result is negative
                break;
            case 826: // sradi (shift right algebraic doubleword immediate)
                emit.ASR_imm(arm64::X0, arm64::X0, inst.sh);
                break;
        }
        
        store_gpr(emit, inst.ra, arm64::X0);
        
        if (inst.rc) {
            compile_cr_update(emit, 0, arm64::X0);
        }
    }
}

void JitCompiler::compile_rotate(ARM64Emitter& emit, const DecodedInst& inst) {
    if (inst.opcode == 20) { // rlwimi
        load_gpr(emit, arm64::X0, inst.rs);
        load_gpr(emit, arm64::X2, inst.ra);  // Get original ra for insert
        emit.UXTW(arm64::X0, arm64::X0);
        
        // Rotate left
        if (inst.sh != 0) {
            emit.ROR_imm(arm64::X0, arm64::X0, 32 - inst.sh);
        }
        
        // Generate mask
        u32 mask = 0;
        if (inst.mb <= inst.me) {
            for (int i = inst.mb; i <= inst.me; i++) {
                mask |= (0x80000000 >> i);
            }
        } else {
            for (int i = 0; i <= inst.me; i++) {
                mask |= (0x80000000 >> i);
            }
            for (int i = inst.mb; i < 32; i++) {
                mask |= (0x80000000 >> i);
            }
        }
        
        emit.MOV_imm(arm64::X1, mask);
        emit.AND(arm64::X0, arm64::X0, arm64::X1);  // Rotated & mask
        emit.MOV_imm(arm64::X3, ~mask);
        emit.AND(arm64::X2, arm64::X2, arm64::X3);  // Original & ~mask
        emit.ORR(arm64::X0, arm64::X0, arm64::X2);  // Insert
        
        store_gpr(emit, inst.ra, arm64::X0);
    }
    else if (inst.opcode == 21) { // rlwinm
        load_gpr(emit, arm64::X0, inst.rs);
        emit.UXTW(arm64::X0, arm64::X0);
        
        // Rotate left
        if (inst.sh != 0) {
            emit.ROR_imm(arm64::X0, arm64::X0, 32 - inst.sh);
        }
        
        // Generate mask
        u32 mask = 0;
        if (inst.mb <= inst.me) {
            for (int i = inst.mb; i <= inst.me; i++) {
                mask |= (0x80000000 >> i);
            }
        } else {
            for (int i = 0; i <= inst.me; i++) {
                mask |= (0x80000000 >> i);
            }
            for (int i = inst.mb; i < 32; i++) {
                mask |= (0x80000000 >> i);
            }
        }
        
        emit.MOV_imm(arm64::X1, mask);
        emit.AND(arm64::X0, arm64::X0, arm64::X1);
        
        store_gpr(emit, inst.ra, arm64::X0);
    }
    else if (inst.opcode == 23) { // rlwnm
        load_gpr(emit, arm64::X0, inst.rs);
        load_gpr(emit, arm64::X1, inst.rb);
        emit.UXTW(arm64::X0, arm64::X0);
        emit.AND_imm(arm64::X1, arm64::X1, 0x1F);
        
        // Rotate left by rb
        emit.MOV_imm(arm64::X2, 32);
        emit.SUB(arm64::X2, arm64::X2, arm64::X1);
        emit.ROR(arm64::X0, arm64::X0, arm64::X2);
        
        // Generate mask
        u32 mask = 0;
        if (inst.mb <= inst.me) {
            for (int i = inst.mb; i <= inst.me; i++) {
                mask |= (0x80000000 >> i);
            }
        } else {
            for (int i = 0; i <= inst.me; i++) {
                mask |= (0x80000000 >> i);
            }
            for (int i = inst.mb; i < 32; i++) {
                mask |= (0x80000000 >> i);
            }
        }
        
        emit.MOV_imm(arm64::X1, mask);
        emit.AND(arm64::X0, arm64::X0, arm64::X1);
        
        store_gpr(emit, inst.ra, arm64::X0);
    }
    
    if (inst.rc) {
        compile_cr_update(emit, 0, arm64::X0);
    }
}

void JitCompiler::compile_compare(ARM64Emitter& emit, const DecodedInst& inst) {
    int crfd = inst.crfd;
    bool is_64bit = inst.raw & (1 << 21);  // L bit
    
    if (inst.opcode == 11) { // cmpi (signed)
        load_gpr(emit, arm64::X0, inst.ra);
        if (!is_64bit) {
            emit.SXTW(arm64::X0, arm64::X0);
        }
        emit.MOV_imm(arm64::X1, static_cast<u64>(static_cast<s64>(inst.simm)));
        emit.CMP(arm64::X0, arm64::X1);
    }
    else if (inst.opcode == 10) { // cmpli (unsigned)
        load_gpr(emit, arm64::X0, inst.ra);
        if (!is_64bit) {
            emit.UXTW(arm64::X0, arm64::X0);
        }
        emit.MOV_imm(arm64::X1, inst.uimm);
        emit.CMP(arm64::X0, arm64::X1);
    }
    else if (inst.opcode == 31) {
        load_gpr(emit, arm64::X0, inst.ra);
        load_gpr(emit, arm64::X1, inst.rb);
        
        if (inst.xo == 0) { // cmp (signed)
            if (!is_64bit) {
                emit.SXTW(arm64::X0, arm64::X0);
                emit.SXTW(arm64::X1, arm64::X1);
            }
        } else { // cmpl (unsigned)
            if (!is_64bit) {
                emit.UXTW(arm64::X0, arm64::X0);
                emit.UXTW(arm64::X1, arm64::X1);
            }
        }
        emit.CMP(arm64::X0, arm64::X1);
    }
    
    // Set CR field based on comparison
    size_t cr_offset = ctx_offset_cr(crfd);
    
    // LT = negative flag
    emit.CSET(arm64::X2, arm64_cond::LT);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset);
    
    // GT = greater than
    emit.CSET(arm64::X2, arm64_cond::GT);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 1);
    
    // EQ = equal
    emit.CSET(arm64::X2, arm64_cond::EQ);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 2);
    
    // SO = XER.SO (copy from XER, or 0)
    emit.STRB(arm64::XZR, arm64::CTX_REG, cr_offset + 3);
}

//=============================================================================
// Load/Store Compilation
//=============================================================================

void JitCompiler::compile_load(ARM64Emitter& emit, const DecodedInst& inst) {
    // Check if this is an update form (need to save EA)
    bool is_update = (inst.opcode == 33 || inst.opcode == 35 || 
                      inst.opcode == 41 || inst.opcode == 43 ||
                      inst.opcode == 49 || inst.opcode == 51);
    
    // Calculate effective address
    bool is_indexed = (inst.opcode == 31);
    
    if (!is_indexed) {
        // For DS-form instructions (opcodes 58, 62), low 2 bits are sub-opcode, not offset
        s32 offset = inst.simm;
        if (inst.opcode == 58 || inst.opcode == 62) {
            offset &= ~3;  // Mask off sub-opcode bits
        }
        calc_ea(emit, arm64::X0, inst.ra, offset);
    } else {
        calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    }
    
    // Save EA for update forms before translation
    if (is_update && inst.ra != 0) {
        emit.ORR(arm64::X3, arm64::XZR, arm64::X0);
    }
    
    // Translate virtual address to physical and add fastmem base
    emit_translate_address(emit, arm64::X0);
    
    // Load based on opcode
    int dest_reg = arm64::X1;
    
    switch (inst.opcode) {
        case 32: // lwz
        case 33: // lwzu
            emit.LDR(dest_reg, arm64::X0);
            emit.UXTW(dest_reg, dest_reg);
            byteswap32(emit, dest_reg);
            break;
        case 34: // lbz
        case 35: // lbzu
            emit.LDRB(dest_reg, arm64::X0);
            break;
        case 40: // lhz
        case 41: // lhzu
            emit.LDRH(dest_reg, arm64::X0);
            byteswap16(emit, dest_reg);
            break;
        case 42: // lha
        case 43: // lhau
            emit.LDRSH(dest_reg, arm64::X0);
            byteswap16(emit, dest_reg);
            emit.SXTH(dest_reg, dest_reg);
            break;
        case 48: // lfs
        case 49: // lfsu
        case 50: // lfd
        case 51: // lfdu
            // Float loads - handle separately
            emit.LDR(dest_reg, arm64::X0);
            byteswap64(emit, dest_reg);
            break;
        case 58: { // ld/ldu/lwa (DS-form)
            int ds_op = inst.raw & 3;
            emit.LDR(dest_reg, arm64::X0);
            byteswap64(emit, dest_reg);
            if (ds_op == 2) {  // lwa - sign extend
                emit.SXTW(dest_reg, dest_reg);
            }
            break;
        }
        case 31: // Extended loads
            switch (inst.xo) {
                case 23: // lwzx
                    emit.LDR(dest_reg, arm64::X0);
                    emit.UXTW(dest_reg, dest_reg);
                    byteswap32(emit, dest_reg);
                    break;
                case 87: // lbzx
                    emit.LDRB(dest_reg, arm64::X0);
                    break;
                case 279: // lhzx
                    emit.LDRH(dest_reg, arm64::X0);
                    byteswap16(emit, dest_reg);
                    break;
                case 343: // lhax
                    emit.LDRSH(dest_reg, arm64::X0);
                    byteswap16(emit, dest_reg);
                    emit.SXTH(dest_reg, dest_reg);
                    break;
                case 21: // ldx
                    emit.LDR(dest_reg, arm64::X0);
                    byteswap64(emit, dest_reg);
                    break;
            }
            break;
    }
    
    store_gpr(emit, inst.rd, dest_reg);
    
    // Update RA for update forms (use saved EA from X3)
    if (is_update && inst.ra != 0) {
        store_gpr(emit, inst.ra, arm64::X3);
    }
}

void JitCompiler::compile_store(ARM64Emitter& emit, const DecodedInst& inst) {
    // Debug: Log stores that might be MMIO related (addresses < 0x80000000 or containing 0x7FC)
    static int store_compile_count = 0;
    if (store_compile_count < 3) {
        LOGI("Compiling store #%d: opcode=%d, ra=%d, simm=0x%04X, fastmem=%d",
             store_compile_count, inst.opcode, inst.ra, (u16)inst.simm, fastmem_enabled_ ? 1 : 0);
        store_compile_count++;
    }
    
    // Check if this is an update form (need to save EA)
    bool is_update = (inst.opcode == 37 || inst.opcode == 39 || 
                      inst.opcode == 45 || inst.opcode == 53 ||
                      inst.opcode == 55);
    
    // Calculate effective address
    bool is_indexed = (inst.opcode == 31);
    
    if (!is_indexed) {
        // For DS-form instructions (opcodes 58, 62), low 2 bits are sub-opcode, not offset
        s32 offset = inst.simm;
        if (inst.opcode == 58 || inst.opcode == 62) {
            offset &= ~3;  // Mask off sub-opcode bits
        }
        calc_ea(emit, arm64::X0, inst.ra, offset);
    } else {
        calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    }
    
    // Save EA for update forms before translation
    if (is_update && inst.ra != 0) {
        emit.ORR(arm64::X3, arm64::XZR, arm64::X0);
    }
    
    // Load value to store
    load_gpr(emit, arm64::X1, inst.rs);
    
    // Save original virtual address for MMIO path (X2 = original EA)
    emit.ORR(arm64::X2, arm64::XZR, arm64::X0);
    
    // === FIX v2: Proper MMIO detection with correct virtual address handling ===
    // Virtual addresses 0x80000000-0x9FFFFFFF map to physical by &0x1FFFFFFF
    // Addresses >= 0xA0000000 are kernel/other and should NOT go to GPU MMIO
    
    // First check: if addr >= 0xA0000000, it's NOT MMIO (kernel space)
    emit.MOV_imm(arm64::X16, 0xA0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* kernel_space = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Branch if addr >= 0xA0000000 (kernel, not MMIO)
    
    // Check if virtual (0x80000000-0x9FFFFFFF)
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_physical = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0x80000000 (already physical)
    
    // It's usermode virtual (0x80000000-0x9FFFFFFF) - translate to physical
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // Patch is_physical branch to here (now X0 has physical address)
    emit.patch_branch(reinterpret_cast<u32*>(is_physical), emit.current());
    
    // Now X0 = physical address - check if it's in GPU MMIO range (0x7FC00000-0x7FFFFFFF)
    emit.MOV_imm(arm64::X16, GPU_MMIO_BASE);  // 0x7FC00000
    emit.CMP(arm64::X0, arm64::X16);
    u8* not_mmio = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if phys < 0x7FC00000 (not MMIO)
    
    // Also check upper bound (must be < 0x80000000 to be valid physical MMIO)
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* above_mmio = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Branch if phys >= 0x80000000 (not in GPU range)
    
    // === MMIO PATH ===
    // Call helper function with ORIGINAL virtual address (X2)
    // jit_mmio_write_XX(memory, addr, value) - Memory class will handle MMIO routing
    
    // Load memory pointer into X0
    emit.LDR(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, memory));
    
    // Setup args: X0=memory, X1=addr (original virtual), X2=value
    emit.ORR(arm64::X16, arm64::XZR, arm64::X1);  // X16 = value (temp)
    emit.ORR(arm64::X1, arm64::XZR, arm64::X2);   // X1 = original addr (from X2)
    emit.ORR(arm64::X2, arm64::XZR, arm64::X16);  // X2 = value
    
    // Determine helper function based on store size
    u64 mmio_helper = 0;
    switch (inst.opcode) {
        case 36: case 37: // stw/stwu
            mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u32);
            break;
        case 38: case 39: // stb/stbu
            mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u8);
            break;
        case 44: case 45: // sth/sthu
            mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u16);
            break;
        case 62: // std/stdu
            mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u64);
            break;
        case 31: // Extended stores - check xo
            switch (inst.xo) {
                case 151: mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u32); break; // stwx
                case 215: mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u8); break;  // stbx
                case 407: mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u16); break; // sthx
                case 149: mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u64); break; // stdx
                default:  mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u32); break;
            }
            break;
        default:
            mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u32);
            break;
    }
    
    emit.MOV_imm(arm64::X16, mmio_helper);
    emit.BLR(arm64::X16);
    
    // Jump past fastmem path
    u8* skip_fastmem = emit.current();
    emit.B(0);
    
    // === FASTMEM PATH ===
    // Patch all non-MMIO branches here
    emit.patch_branch(reinterpret_cast<u32*>(kernel_space), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(not_mmio), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(above_mmio), emit.current());
    
    // Reload original EA and value since we may have clobbered registers
    emit.ORR(arm64::X0, arm64::XZR, arm64::X2);  // Restore original EA from X2
    load_gpr(emit, arm64::X1, inst.rs);
    
    // #region agent log - Hypothesis AB: Check for event address writes in fastmem
    // Check if this is a write to our problematic event addresses (before translation)
    // Only for 32-bit stores (stw/stwu/stwx)
    u8* skip_store_label = nullptr;
    if (inst.opcode == 36 || inst.opcode == 37 || (inst.opcode == 31 && inst.xo == 151)) {
        // Check if addr is in our event range (0x9FFEFC40-0x9FFEFC50 or physical equivalents)
        // We check by masking to get physical address and comparing
        // Physical range: 0x1FFEFC40 - 0x1FFEFC50
        
        // Save original address and value
        emit.ORR(arm64::X17, arm64::XZR, arm64::X0);
        emit.ORR(arm64::X18, arm64::XZR, arm64::X1);
        
        // Compute physical: if >= 0x80000000, mask with 0x1FFFFFFF
        emit.MOV_imm(arm64::X16, 0x80000000ULL);
        emit.CMP(arm64::X0, arm64::X16);
        u8* already_phys = emit.current();
        emit.B_cond(arm64_cond::CC, 0);  // If < 0x80000000, skip masking
        emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
        emit.AND(arm64::X0, arm64::X0, arm64::X16);
        emit.patch_branch(reinterpret_cast<u32*>(already_phys), emit.current());
        
        // Check if in range 0x1FFEFC20 - 0x1FFEFC64 (expanded to cover all init events)
        emit.MOV_imm(arm64::X16, 0x1FFEFC20ULL);
        emit.CMP(arm64::X0, arm64::X16);
        u8* below_range = emit.current();
        emit.B_cond(arm64_cond::CC, 0);  // Below range
        
        emit.MOV_imm(arm64::X16, 0x1FFEFC64ULL);
        emit.CMP(arm64::X0, arm64::X16);
        u8* above_range = emit.current();
        emit.B_cond(arm64_cond::CS, 0);  // Above range
        
        // In range - call trace/block function with original virtual addr (X17) and value (X18)
        emit.STP(arm64::X17, arm64::X18, arm64::SP, -16);
        emit.STP(arm64::X2, arm64::X30, arm64::SP, -32);
        emit.SUB_imm(arm64::SP, arm64::SP, 32);
        emit.ORR(arm64::X0, arm64::XZR, arm64::X17);  // addr = original virtual
        emit.ORR(arm64::X1, arm64::XZR, arm64::X18);  // value
        emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(&jit_trace_event_store));
        emit.BLR(arm64::X16);
        // X0 now contains: 1 if write should be blocked, 0 otherwise
        emit.ORR(arm64::X4, arm64::XZR, arm64::X0);  // Save result
        emit.ADD_imm(arm64::SP, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X30, arm64::SP, -32);
        emit.LDP(arm64::X17, arm64::X18, arm64::SP, -16);
        
        // If X4 == 1, skip the store entirely
        emit.CBNZ(arm64::X4, 0);
        skip_store_label = emit.current() - 4;
        
        emit.patch_branch(reinterpret_cast<u32*>(below_range), emit.current());
        emit.patch_branch(reinterpret_cast<u32*>(above_range), emit.current());
        
        // Restore original address and value
        emit.ORR(arm64::X0, arm64::XZR, arm64::X17);
        emit.ORR(arm64::X1, arm64::XZR, arm64::X18);
    }
    // #endregion
    
    // Translate virtual address to physical and add fastmem base
    emit_translate_address(emit, arm64::X0);
    
    // Store based on opcode
    switch (inst.opcode) {
        case 36: // stw
        case 37: // stwu
            byteswap32(emit, arm64::X1);
            emit.STR(arm64::X1, arm64::X0);
            break;
        case 38: // stb
        case 39: // stbu
            emit.STRB(arm64::X1, arm64::X0);
            break;
        case 44: // sth
        case 45: // sthu
            byteswap16(emit, arm64::X1);
            emit.STRH(arm64::X1, arm64::X0);
            break;
        case 52: // stfs
        case 53: // stfsu
        case 54: // stfd
        case 55: // stfdu
            byteswap64(emit, arm64::X1);
            emit.STR(arm64::X1, arm64::X0);
            break;
        case 62: // std/stdu (DS-form)
            byteswap64(emit, arm64::X1);
            emit.STR(arm64::X1, arm64::X0);
            break;
        case 31: // Extended stores
            switch (inst.xo) {
                case 151: // stwx
                    byteswap32(emit, arm64::X1);
                    emit.STR(arm64::X1, arm64::X0);
                    break;
                case 215: // stbx
                    emit.STRB(arm64::X1, arm64::X0);
                    break;
                case 407: // sthx
                    byteswap16(emit, arm64::X1);
                    emit.STRH(arm64::X1, arm64::X0);
                    break;
                case 149: // stdx
                    byteswap64(emit, arm64::X1);
                    emit.STR(arm64::X1, arm64::X0);
                    break;
            }
            break;
    }
    
    // === DONE ===
    // Patch skip_fastmem branch to here
    emit.patch_branch(reinterpret_cast<u32*>(skip_fastmem), emit.current());
    
    // Patch skip_store_label (if set) to skip the entire store for blocked event writes
    if (skip_store_label) {
        s64 skip_offset = emit.current() - skip_store_label;
        *reinterpret_cast<u32*>(skip_store_label) = 0xB5000000 | ((skip_offset >> 2) << 5) | arm64::X4;
    }
    
    // Update RA for update forms (use saved EA from X3)
    if (is_update && inst.ra != 0) {
        store_gpr(emit, inst.ra, arm64::X3);
    }
}

void JitCompiler::compile_load_multiple(ARM64Emitter& emit, const DecodedInst& inst) {
    calc_ea(emit, arm64::X0, inst.ra, inst.simm);
    emit_translate_address(emit, arm64::X0);
    
    for (u32 r = inst.rd; r < 32; r++) {
        emit.LDR(arm64::X1, arm64::X0, (r - inst.rd) * 4);
        byteswap32(emit, arm64::X1);
        store_gpr(emit, r, arm64::X1);
    }
}

void JitCompiler::compile_store_multiple(ARM64Emitter& emit, const DecodedInst& inst) {
    calc_ea(emit, arm64::X0, inst.ra, inst.simm);
    
    // #region agent log - Hypothesis AD: Trace store multiple to event range
    // Save base address before translation for checking
    emit.ORR(arm64::X17, arm64::XZR, arm64::X0);
    
    // Compute physical address
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* stm_phys = emit.current();
    emit.B_cond(arm64_cond::CC, 0);
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    emit.patch_branch(reinterpret_cast<u32*>(stm_phys), emit.current());
    
    // Calculate end address: base + (32 - rs) * 4
    u32 num_regs = 32 - inst.rs;
    emit.ADD_imm(arm64::X16, arm64::X0, num_regs * 4);
    
    // Check if range overlaps with 0x1FFEFC40-0x1FFEFC54
    emit.MOV_imm(arm64::X15, 0x1FFEFC54ULL);
    emit.CMP(arm64::X0, arm64::X15);
    u8* stm_above = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // start >= end of event range
    
    emit.MOV_imm(arm64::X15, 0x1FFEFC40ULL);
    emit.CMP(arm64::X16, arm64::X15);
    u8* stm_below = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // end <= start of event range
    
    // Overlaps! Log it
    emit.STP(arm64::X17, arm64::X0, arm64::SP, -16);
    emit.STP(arm64::X2, arm64::X30, arm64::SP, -32);
    emit.SUB_imm(arm64::SP, arm64::SP, 32);
    emit.ORR(arm64::X0, arm64::XZR, arm64::X17);  // virtual addr
    emit.MOV_imm(arm64::X1, 0xFFFFFFFF);  // special value to indicate stmw
    emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(&jit_trace_event_store));
    emit.BLR(arm64::X16);
    emit.ADD_imm(arm64::SP, arm64::SP, 32);
    emit.LDP(arm64::X2, arm64::X30, arm64::SP, -32);
    emit.LDP(arm64::X17, arm64::X0, arm64::SP, -16);
    
    emit.patch_branch(reinterpret_cast<u32*>(stm_above), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(stm_below), emit.current());
    
    emit.ORR(arm64::X0, arm64::XZR, arm64::X17);  // Restore original
    // #endregion
    
    emit_translate_address(emit, arm64::X0);
    
    for (u32 r = inst.rs; r < 32; r++) {
        load_gpr(emit, arm64::X1, r);
        byteswap32(emit, arm64::X1);
        emit.STR(arm64::X1, arm64::X0, (r - inst.rs) * 4);
    }
}

//=============================================================================
// Atomic Operations (lwarx/stwcx) - Per-Thread Reservation
//=============================================================================

void JitCompiler::compile_atomic_load(ARM64Emitter& emit, const DecodedInst& inst) {
    // lwarx rD, rA, rB - Load Word And Reserve Indexed
    calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    
    // Save untranslated address for reservation
    emit.ORR(arm64::X2, arm64::XZR, arm64::X0);
    
    emit_translate_address(emit, arm64::X0);
    
    // Load the value with exclusive access
    // ARM64: LDAXR for acquire semantics
    emit.LDR(arm64::X1, arm64::X0, 0);
    byteswap32(emit, arm64::X1);
    
    store_gpr(emit, inst.rd, arm64::X1);
    
    // Store per-thread reservation in ThreadContext
    emit.STR(arm64::X2, arm64::CTX_REG, offsetof(ThreadContext, reservation_addr));
    emit.MOV_imm(arm64::X3, 4);  // reservation size = 4 bytes
    emit.STR(arm64::X3, arm64::CTX_REG, offsetof(ThreadContext, reservation_size));
    emit.MOV_imm(arm64::X3, 1);  // has_reservation = true
    emit.STRB(arm64::X3, arm64::CTX_REG, offsetof(ThreadContext, has_reservation));
}

void JitCompiler::compile_atomic_store(ARM64Emitter& emit, const DecodedInst& inst) {
    // stwcx. rS, rA, rB - Store Word Conditional Indexed
    calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    
    // #region agent log - Hypothesis AC: Trace atomic stores to event addresses
    // Save address for tracing
    emit.ORR(arm64::X17, arm64::XZR, arm64::X0);
    load_gpr(emit, arm64::X4, inst.rs);  // Value being stored
    
    // Check if in event range (physical 0x1FFEFC40-0x1FFEFC54)
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* atomic_phys = emit.current();
    emit.B_cond(arm64_cond::CC, 0);
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    emit.patch_branch(reinterpret_cast<u32*>(atomic_phys), emit.current());
    
    emit.MOV_imm(arm64::X16, 0x1FFEFC40ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* atomic_below = emit.current();
    emit.B_cond(arm64_cond::CC, 0);
    
    emit.MOV_imm(arm64::X16, 0x1FFEFC54ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* atomic_above = emit.current();
    emit.B_cond(arm64_cond::CS, 0);
    
    // In range - call trace
    emit.STP(arm64::X17, arm64::X4, arm64::SP, -16);
    emit.STP(arm64::X2, arm64::X30, arm64::SP, -32);
    emit.SUB_imm(arm64::SP, arm64::SP, 32);
    emit.ORR(arm64::X0, arm64::XZR, arm64::X17);
    emit.ORR(arm64::X1, arm64::XZR, arm64::X4);
    emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(&jit_trace_event_store));
    emit.BLR(arm64::X16);
    emit.ADD_imm(arm64::SP, arm64::SP, 32);
    emit.LDP(arm64::X2, arm64::X30, arm64::SP, -32);
    emit.LDP(arm64::X17, arm64::X4, arm64::SP, -16);
    
    emit.patch_branch(reinterpret_cast<u32*>(atomic_below), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(atomic_above), emit.current());
    emit.ORR(arm64::X0, arm64::XZR, arm64::X17);  // Restore address
    // #endregion
    
    // Check if reservation is valid
    emit.LDRB(arm64::X3, arm64::CTX_REG, offsetof(ThreadContext, has_reservation));
    emit.CBZ(arm64::X3, 0);  // Branch if no reservation
    u8* no_reservation = emit.current() - 4;
    
    // Load reservation address (untranslated) from context
    emit.LDR(arm64::X2, arm64::CTX_REG, offsetof(ThreadContext, reservation_addr));
    
    // Compare addresses (both untranslated)
    emit.CMP(arm64::X0, arm64::X2);
    
    // If not equal, set CR0.EQ=0 and skip store
    u8* skip = emit.current();
    emit.B_cond(arm64_cond::NE, 0);
    
    // Addresses match - translate and do the store
    emit_translate_address(emit, arm64::X0);
    
    load_gpr(emit, arm64::X1, inst.rs);
    byteswap32(emit, arm64::X1);
    emit.STR(arm64::X1, arm64::X0, 0);
    
    // Set CR0.EQ=1 (success)
    emit.MOV_imm(arm64::X2, 1);
    emit.STRB(arm64::X2, arm64::CTX_REG, ctx_offset_cr(0) + 2); // EQ
    emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(0) + 0); // LT
    emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(0) + 1); // GT
    
    u8* done = emit.current();
    emit.B(0);
    
    // Patch no_reservation branch (CBZ)
    s64 no_res_offset = emit.current() - no_reservation;
    *reinterpret_cast<u32*>(no_reservation) = 0xB4000000 | ((no_res_offset >> 2) << 5) | arm64::X3;
    
    // Patch skip branch
    s64 skip_offset = emit.current() - skip;
    *reinterpret_cast<u32*>(skip) = 0x54000000 | ((skip_offset >> 2) << 5) | arm64_cond::NE;
    
    // Set CR0.EQ=0 (failure)
    emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(0) + 2); // EQ = 0
    emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(0) + 0); // LT = 0
    emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(0) + 1); // GT = 0
    
    // Patch done branch
    s64 done_offset = emit.current() - done;
    *reinterpret_cast<u32*>(done) = 0x14000000 | ((done_offset >> 2) & 0x03FFFFFF);
    
    // Clear reservation (has_reservation = false)
    emit.STRB(arm64::XZR, arm64::CTX_REG, offsetof(ThreadContext, has_reservation));
}

//=============================================================================
// Cache Operations
//=============================================================================

void JitCompiler::compile_dcbz(ARM64Emitter& emit, const DecodedInst& inst) {
    // dcbz - Data Cache Block Zero: zeros 32 bytes aligned to 32-byte boundary
    // Address = (rA|0) + rB, aligned to 32 bytes
    
    calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    
    // Align to 32 bytes (clear lower 5 bits)
    emit.MOV_imm(arm64::X16, ~31ULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // #region agent log - Hypothesis AE: Trace DCBZ operations
    // Check if in event range (physical 0x1FFEFC40-0x1FFEFC60 considering 32-byte block)
    emit.ORR(arm64::X17, arm64::XZR, arm64::X0);  // Save for later
    
    // Compute physical
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* dcbz_phys = emit.current();
    emit.B_cond(arm64_cond::CC, 0);
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    emit.patch_branch(reinterpret_cast<u32*>(dcbz_phys), emit.current());
    
    // Check if overlaps 0x1FFEFC40-0x1FFEFC60 (32 bytes from 0x40 could go to 0x60)
    emit.ADD_imm(arm64::X16, arm64::X0, 32);  // end of block
    emit.MOV_imm(arm64::X15, 0x1FFEFC40ULL);
    emit.CMP(arm64::X16, arm64::X15);
    u8* dcbz_below = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // end <= 0x1FFEFC40
    
    emit.MOV_imm(arm64::X15, 0x1FFEFC60ULL);
    emit.CMP(arm64::X0, arm64::X15);
    u8* dcbz_above = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // start >= 0x1FFEFC60
    
    // Overlaps! Log it
    emit.STP(arm64::X17, arm64::X0, arm64::SP, -16);
    emit.STP(arm64::X2, arm64::X30, arm64::SP, -32);
    emit.SUB_imm(arm64::SP, arm64::SP, 32);
    emit.ORR(arm64::X0, arm64::XZR, arm64::X17);  // virtual addr
    emit.MOV_imm(arm64::X1, 0xDCB20000);  // special marker for dcbz
    emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(&jit_trace_event_store));
    emit.BLR(arm64::X16);
    emit.ADD_imm(arm64::SP, arm64::SP, 32);
    emit.LDP(arm64::X2, arm64::X30, arm64::SP, -32);
    emit.LDP(arm64::X17, arm64::X0, arm64::SP, -16);
    
    emit.patch_branch(reinterpret_cast<u32*>(dcbz_below), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(dcbz_above), emit.current());
    
    emit.ORR(arm64::X0, arm64::XZR, arm64::X17);  // Restore original
    // #endregion
    
    // Translate address and zero 32 bytes
    emit_translate_address(emit, arm64::X0);
    
    // Zero 32 bytes (use 4x STP of XZR pairs = 4 * 16 = 64 bytes... wait, that's too much)
    // Zero using 4 STR of 64-bit zeros = 4 * 8 = 32 bytes
    emit.STR(arm64::XZR, arm64::X0, 0);
    emit.STR(arm64::XZR, arm64::X0, 8);
    emit.STR(arm64::XZR, arm64::X0, 16);
    emit.STR(arm64::XZR, arm64::X0, 24);
}

//=============================================================================
// Additional Instructions
//=============================================================================

void JitCompiler::compile_extsb(ARM64Emitter& emit, const DecodedInst& inst) {
    load_gpr(emit, arm64::X0, inst.rs);
    emit.SXTB(arm64::X0, arm64::X0);
    store_gpr(emit, inst.ra, arm64::X0);
    
    if (inst.rc) {
        compile_cr_update(emit, 0, arm64::X0);
    }
}

void JitCompiler::compile_extsh(ARM64Emitter& emit, const DecodedInst& inst) {
    load_gpr(emit, arm64::X0, inst.rs);
    emit.SXTH(arm64::X0, arm64::X0);
    store_gpr(emit, inst.ra, arm64::X0);
    
    if (inst.rc) {
        compile_cr_update(emit, 0, arm64::X0);
    }
}

void JitCompiler::compile_extsw(ARM64Emitter& emit, const DecodedInst& inst) {
    load_gpr(emit, arm64::X0, inst.rs);
    emit.SXTW(arm64::X0, arm64::X0);
    store_gpr(emit, inst.ra, arm64::X0);
    
    if (inst.rc) {
        compile_cr_update(emit, 0, arm64::X0);
    }
}

void JitCompiler::compile_cntlzw(ARM64Emitter& emit, const DecodedInst& inst) {
    load_gpr(emit, arm64::X0, inst.rs);
    emit.UXTW(arm64::X0, arm64::X0);  // Zero-extend to 64-bit
    emit.CLZ(arm64::X0, arm64::X0);
    emit.SUB_imm(arm64::X0, arm64::X0, 32);  // Adjust for 64-bit CLZ on 32-bit value
    store_gpr(emit, inst.ra, arm64::X0);
    
    if (inst.rc) {
        compile_cr_update(emit, 0, arm64::X0);
    }
}

void JitCompiler::compile_cntlzd(ARM64Emitter& emit, const DecodedInst& inst) {
    load_gpr(emit, arm64::X0, inst.rs);
    emit.CLZ(arm64::X0, arm64::X0);
    store_gpr(emit, inst.ra, arm64::X0);
    
    if (inst.rc) {
        compile_cr_update(emit, 0, arm64::X0);
    }
}

//=============================================================================
// Branch Compilation
//=============================================================================

void JitCompiler::compile_branch(ARM64Emitter& emit, const DecodedInst& inst, 
                                  GuestAddr pc, CompiledBlock* block) {
    GuestAddr target;
    bool absolute = inst.raw & 2;
    bool link = inst.raw & 1;
    
    if (absolute) {
        target = inst.li;
    } else {
        target = pc + inst.li;
    }
    
    // Save link register if LK=1
    if (link) {
        emit.MOV_imm(arm64::X0, pc + 4);
        emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_lr());
    }
    
    // Update PC
    emit.MOV_imm(arm64::X0, target);
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
    
    // Return from block (will continue in dispatcher)
    emit_block_epilogue(emit, current_block_inst_count_);
}

void JitCompiler::compile_branch_conditional(ARM64Emitter& emit, const DecodedInst& inst,
                                             GuestAddr pc, CompiledBlock* block) {
    u8 bo = inst.bo;
    u8 bi = inst.bi;
    
    // Calculate targets
    GuestAddr target_taken = 0;
    GuestAddr target_not_taken = pc + 4;
    
    bool decrement_ctr = !(bo & 0x04);
    bool test_ctr_zero = (bo & 0x02);
    bool test_cond = !(bo & 0x10);
    bool cond_value = (bo & 0x08);
    bool is_lr_target = false;
    bool is_ctr_target = false;
    
    if (inst.opcode == 16) { // bc
        if (inst.raw & 2) { // AA (absolute)
            target_taken = inst.simm & ~3;
        } else {
            target_taken = pc + (inst.simm & ~3);
        }
    } else if (inst.opcode == 19) {
        if (inst.xo == 16) { // bclr
            is_lr_target = true;
        } else if (inst.xo == 528) { // bcctr
            is_ctr_target = true;
        }
    }
    
    // Collect skip branch sites for patching
    std::vector<u8*> skip_branches;
    
    // Handle CTR decrement (not for bcctr)
    if (decrement_ctr && !is_ctr_target) {
        emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
        emit.SUB_imm(arm64::X0, arm64::X0, 1);
        emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
        
        // Test CTR
        u8* skip = emit.current();
        if (test_ctr_zero) { // Branch if CTR == 0
            emit.CBNZ(arm64::X0, 0);  // Skip to not-taken if CTR != 0
        } else { // Branch if CTR != 0
            emit.CBZ(arm64::X0, 0);   // Skip to not-taken if CTR == 0
        }
        skip_branches.push_back(skip);
    }
    
    // Handle condition test
    if (test_cond) {
        int cr_field = bi / 4;
        int cr_bit = bi % 4;
        
        emit.LDRB(arm64::X0, arm64::CTX_REG, ctx_offset_cr(cr_field) + cr_bit);
        
        u8* skip = emit.current();
        if (cond_value) { // Test for 1
            emit.CBZ(arm64::X0, 0);   // Skip to not-taken if bit is 0
        } else { // Test for 0
            emit.CBNZ(arm64::X0, 0);  // Skip to not-taken if bit is 1
        }
        skip_branches.push_back(skip);
    }
    
    // ---- Branch taken path ----
    
    // Save link register if LK=1
    if (inst.raw & 1) { // LK
        emit.MOV_imm(arm64::X0, pc + 4);
        emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_lr());
    }
    
    // Set target PC
    if (is_lr_target) {
        emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_lr());
        emit.AND_imm(arm64::X0, arm64::X0, ~3ULL);
    } else if (is_ctr_target) {
        emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
        emit.AND_imm(arm64::X0, arm64::X0, ~3ULL);
    } else {
        emit.MOV_imm(arm64::X0, target_taken);
    }
    
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
    emit_block_epilogue(emit, current_block_inst_count_);
    
    // ---- Not-taken path ----
    u8* not_taken_start = emit.current();
    
    // Patch all skip branches to jump here
    for (u8* skip : skip_branches) {
        s64 skip_offset = not_taken_start - skip;
        u32* patch_addr = reinterpret_cast<u32*>(skip);
        s32 imm19 = skip_offset >> 2;
        *patch_addr = (*patch_addr & 0xFF00001F) | ((imm19 & 0x7FFFF) << 5);
    }
    
    // Not-taken: continue to next instruction
    emit.MOV_imm(arm64::X0, target_not_taken);
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
    emit_block_epilogue(emit, current_block_inst_count_);
}

void JitCompiler::compile_branch_to_lr(ARM64Emitter& emit, const DecodedInst& inst, 
                                       CompiledBlock* block) {
    // bclr - branch conditional to LR
    // This is handled by compile_branch_conditional with xo=16
}

void JitCompiler::compile_branch_to_ctr(ARM64Emitter& emit, const DecodedInst& inst,
                                        CompiledBlock* block) {
    // bcctr - branch conditional to CTR
    // This is handled by compile_branch_conditional with xo=528
}

//=============================================================================
// Float Compilation
//=============================================================================

void JitCompiler::compile_float(ARM64Emitter& emit, const DecodedInst& inst) {
    // Load FPR operands
    load_fpr(emit, 0, inst.ra);
    load_fpr(emit, 1, inst.rb);
    
    switch (inst.xo) {
        case 21: // fadd
            emit.FADD_vec(0, 0, 1, true);
            break;
        case 20: // fsub
            emit.FSUB_vec(0, 0, 1, true);
            break;
        case 25: // fmul
            load_fpr(emit, 1, (inst.raw >> 6) & 0x1F); // FRC
            emit.FMUL_vec(0, 0, 1, true);
            break;
        case 18: // fdiv
            emit.FDIV_vec(0, 0, 1, true);
            break;
        case 29: // fmadd
        case 28: // fmsub
        case 31: // fnmadd
        case 30: // fnmsub
            load_fpr(emit, 2, (inst.raw >> 6) & 0x1F); // FRC
            emit.FMADD_vec(0, 0, 2, 1, true);
            if (inst.xo == 28 || inst.xo == 30) {
                // Negate for fmsub/fnmsub
            }
            break;
    }
    
    store_fpr(emit, inst.rd, 0);
}

//=============================================================================
// Vector Compilation (VMX128 -> NEON)
//=============================================================================

void JitCompiler::compile_vector(ARM64Emitter& emit, const DecodedInst& inst) {
    // Load vector operands
    load_vr(emit, 0, inst.ra);
    load_vr(emit, 1, inst.rb);
    
    switch (inst.type) {
        case DecodedInst::Type::VAdd:
            emit.FADD_vec(0, 0, 1, false);
            break;
        case DecodedInst::Type::VSub:
            emit.FSUB_vec(0, 0, 1, false);
            break;
        case DecodedInst::Type::VMul:
            emit.FMUL_vec(0, 0, 1, false);
            break;
        case DecodedInst::Type::VLogical:
            emit.AND_vec(0, 0, 1);
            break;
        default:
            emit.NOP();
            break;
    }
    
    store_vr(emit, inst.rd, 0);
}

//=============================================================================
// System Instruction Compilation
//=============================================================================

void JitCompiler::compile_syscall(ARM64Emitter& emit, const DecodedInst& inst) {
    // Set interrupted flag to signal syscall to dispatcher
    emit.MOV_imm(arm64::X0, 1);
    emit.STRB(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, interrupted));
    
    // Return from block to handle syscall
    // Note: PC is NOT updated here - the dispatcher will handle PC advancement
    emit_block_epilogue(emit, current_block_inst_count_);
}

void JitCompiler::compile_mtspr(ARM64Emitter& emit, const DecodedInst& inst) {
    u32 spr = ((inst.raw >> 16) & 0x1F) | ((inst.raw >> 6) & 0x3E0);
    
    load_gpr(emit, arm64::X0, inst.rs);
    
    switch (spr) {
        case 8: // LR
            emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_lr());
            break;
        case 9: // CTR
            emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
            break;
        case 1: // XER
            emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_xer());
            break;
        default:
            // Ignore other SPRs
            break;
    }
}

void JitCompiler::compile_mfspr(ARM64Emitter& emit, const DecodedInst& inst) {
    u32 spr = ((inst.raw >> 16) & 0x1F) | ((inst.raw >> 6) & 0x3E0);
    
    switch (spr) {
        case 8: // LR
            emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_lr());
            break;
        case 9: // CTR
            emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
            break;
        case 1: // XER
            emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_xer());
            break;
        case 268: // TBL (time base lower)
        case 284: // TBL alternate encoding
            // Load time base and return lower 32 bits
            emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_time_base());
            // Mask to 32 bits (upper bits will be zero-extended by store_gpr)
            break;
        case 269: // TBU (time base upper)
        case 285: // TBU alternate encoding
            // Load time base and return upper 32 bits
            emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_time_base());
            emit.LSR_imm(arm64::X0, arm64::X0, 32);
            break;
        default:
            emit.MOV_imm(arm64::X0, 0);
            break;
    }
    
    store_gpr(emit, inst.rd, arm64::X0);
}

void JitCompiler::compile_cr_logical(ARM64Emitter& emit, const DecodedInst& inst) {
    // CR logical operations (opcode 19)
    // Format: crbD, crbA, crbB
    int crbd = (inst.raw >> 21) & 0x1F;
    int crba = (inst.raw >> 16) & 0x1F;
    int crbb = (inst.raw >> 11) & 0x1F;
    
    // Get CR field and bit positions
    int crfd = crbd / 4;
    int crfa = crba / 4;
    int crfb = crbb / 4;
    int bitd = crbd % 4;
    int bita = crba % 4;
    int bitb = crbb % 4;
    
    // Load source bits
    emit.LDRB(arm64::X0, arm64::CTX_REG, ctx_offset_cr(crfa) + bita);
    emit.LDRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(crfb) + bitb);
    
    switch (inst.xo) {
        case 257: // crand
            emit.AND(arm64::X0, arm64::X0, arm64::X1);
            break;
        case 449: // cror
            emit.ORR(arm64::X0, arm64::X0, arm64::X1);
            break;
        case 193: // crxor
            emit.EOR(arm64::X0, arm64::X0, arm64::X1);
            break;
        case 225: // crnand
            emit.AND(arm64::X0, arm64::X0, arm64::X1);
            emit.EOR_imm(arm64::X0, arm64::X0, 1);
            break;
        case 33:  // crnor
            emit.ORR(arm64::X0, arm64::X0, arm64::X1);
            emit.EOR_imm(arm64::X0, arm64::X0, 1);
            break;
        case 289: // creqv
            emit.EOR(arm64::X0, arm64::X0, arm64::X1);
            emit.EOR_imm(arm64::X0, arm64::X0, 1);
            break;
        case 129: // crandc (a AND NOT b)
            emit.EOR_imm(arm64::X1, arm64::X1, 1);
            emit.AND(arm64::X0, arm64::X0, arm64::X1);
            break;
        case 417: // crorc (a OR NOT b)
            emit.EOR_imm(arm64::X1, arm64::X1, 1);
            emit.ORR(arm64::X0, arm64::X0, arm64::X1);
            break;
        default:
            // Unknown CR op, NOP
            return;
    }
    
    // Mask to single bit and store result
    emit.AND_imm(arm64::X0, arm64::X0, 1);
    emit.STRB(arm64::X0, arm64::CTX_REG, ctx_offset_cr(crfd) + bitd);
}

//=============================================================================
// CR Operations
//=============================================================================

void JitCompiler::compile_mtcrf(ARM64Emitter& emit, const DecodedInst& inst) {
    // mtcrf crM, rS - Move to CR fields
    // crM is 8-bit field mask (bits 12-19)
    u8 crm = (inst.raw >> 12) & 0xFF;
    
    load_gpr(emit, arm64::X0, inst.rs);
    
    // Process each CR field
    for (int i = 0; i < 8; i++) {
        if (crm & (0x80 >> i)) {
            // Extract 4 bits for this field from RS
            // CR field i is bits (28 - i*4) to (31 - i*4) in the 32-bit view
            int shift = 28 - i * 4;
            emit.LSR_imm(arm64::X1, arm64::X0, shift);
            emit.AND_imm(arm64::X1, arm64::X1, 0xF);
            
            // Split into individual bits
            // LT (bit 3), GT (bit 2), EQ (bit 1), SO (bit 0)
            emit.LSR_imm(arm64::X2, arm64::X1, 3);
            emit.AND_imm(arm64::X2, arm64::X2, 1);
            emit.STRB(arm64::X2, arm64::CTX_REG, ctx_offset_cr(i) + 0); // LT
            
            emit.LSR_imm(arm64::X2, arm64::X1, 2);
            emit.AND_imm(arm64::X2, arm64::X2, 1);
            emit.STRB(arm64::X2, arm64::CTX_REG, ctx_offset_cr(i) + 1); // GT
            
            emit.LSR_imm(arm64::X2, arm64::X1, 1);
            emit.AND_imm(arm64::X2, arm64::X2, 1);
            emit.STRB(arm64::X2, arm64::CTX_REG, ctx_offset_cr(i) + 2); // EQ
            
            emit.AND_imm(arm64::X2, arm64::X1, 1);
            emit.STRB(arm64::X2, arm64::CTX_REG, ctx_offset_cr(i) + 3); // SO
        }
    }
}

void JitCompiler::compile_mfcr(ARM64Emitter& emit, const DecodedInst& inst) {
    // mfcr rD - Move from CR
    // Build the 32-bit CR value from individual fields
    emit.MOV_imm(arm64::X0, 0);
    
    for (int i = 0; i < 8; i++) {
        int shift = 28 - i * 4;
        
        // Load and combine each bit of CR field i
        emit.LDRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(i) + 0); // LT
        emit.LSL_imm(arm64::X1, arm64::X1, shift + 3);
        emit.ORR(arm64::X0, arm64::X0, arm64::X1);
        
        emit.LDRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(i) + 1); // GT
        emit.LSL_imm(arm64::X1, arm64::X1, shift + 2);
        emit.ORR(arm64::X0, arm64::X0, arm64::X1);
        
        emit.LDRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(i) + 2); // EQ
        emit.LSL_imm(arm64::X1, arm64::X1, shift + 1);
        emit.ORR(arm64::X0, arm64::X0, arm64::X1);
        
        emit.LDRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(i) + 3); // SO
        emit.LSL_imm(arm64::X1, arm64::X1, shift);
        emit.ORR(arm64::X0, arm64::X0, arm64::X1);
    }
    
    store_gpr(emit, inst.rd, arm64::X0);
}

void JitCompiler::compile_cr_update(ARM64Emitter& emit, int field, int result_reg) {
    size_t cr_offset = ctx_offset_cr(field);
    
    // Compare result with 0
    emit.CMP_imm(result_reg, 0);
    
    // LT = result < 0 (signed)
    emit.CSET(arm64::X2, arm64_cond::LT);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset);
    
    // GT = result > 0 (signed)
    emit.CSET(arm64::X2, arm64_cond::GT);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 1);
    
    // EQ = result == 0
    emit.CSET(arm64::X2, arm64_cond::EQ);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 2);
    
    // SO = keep existing (XER.SO)
}

//=============================================================================
// Helpers
//=============================================================================

void JitCompiler::load_gpr(ARM64Emitter& emit, int arm_reg, int ppc_reg) {
    if (ppc_reg == 0) {
        emit.MOV_imm(arm_reg, 0);
    } else {
        emit.LDR(arm_reg, arm64::CTX_REG, ctx_offset_gpr(ppc_reg));
    }
}

void JitCompiler::store_gpr(ARM64Emitter& emit, int ppc_reg, int arm_reg) {
    // Note: r0 CAN be written to as a destination register in most instructions.
    // The "r0 = 0" special case only applies when r0 is used as a BASE register
    // for load/store address calculation (rA field), not when it's a destination (rD).
    emit.STR(arm_reg, arm64::CTX_REG, ctx_offset_gpr(ppc_reg));
}

void JitCompiler::load_fpr(ARM64Emitter& emit, int neon_reg, int ppc_reg) {
    emit.LDR_vec(neon_reg, arm64::CTX_REG, ctx_offset_fpr(ppc_reg));
}

void JitCompiler::store_fpr(ARM64Emitter& emit, int ppc_reg, int neon_reg) {
    emit.STR_vec(neon_reg, arm64::CTX_REG, ctx_offset_fpr(ppc_reg));
}

void JitCompiler::load_vr(ARM64Emitter& emit, int neon_reg, int ppc_reg) {
    emit.LDR_vec(neon_reg, arm64::CTX_REG, ctx_offset_vr(ppc_reg));
}

void JitCompiler::store_vr(ARM64Emitter& emit, int ppc_reg, int neon_reg) {
    emit.STR_vec(neon_reg, arm64::CTX_REG, ctx_offset_vr(ppc_reg));
}

void JitCompiler::calc_ea(ARM64Emitter& emit, int dest_reg, int ra, s16 offset) {
    if (ra == 0) {
        emit.MOV_imm(dest_reg, static_cast<u64>(static_cast<s64>(offset)));
    } else {
        load_gpr(emit, dest_reg, ra);
        if (offset != 0) {
            if (offset > 0 && offset < 4096) {
                emit.ADD_imm(dest_reg, dest_reg, offset);
            } else if (offset < 0 && -offset < 4096) {
                emit.SUB_imm(dest_reg, dest_reg, -offset);
            } else {
                emit.MOV_imm(arm64::X16, static_cast<u64>(static_cast<s64>(offset)));
                emit.ADD(dest_reg, dest_reg, arm64::X16);
            }
        }
    }
}

void JitCompiler::calc_ea_indexed(ARM64Emitter& emit, int dest_reg, int ra, int rb) {
    if (ra == 0) {
        load_gpr(emit, dest_reg, rb);
    } else {
        load_gpr(emit, dest_reg, ra);
        load_gpr(emit, arm64::X16, rb);
        emit.ADD(dest_reg, dest_reg, arm64::X16);
    }
}

void JitCompiler::emit_translate_address(ARM64Emitter& emit, int addr_reg) {
    // Translate Xbox 360 address to host fastmem address
    // Simple approach: ALWAYS mask with 0x1FFFFFFF to get physical offset (512MB)
    // This works for both virtual (0x8XXXXXXX) and physical (0x0XXXXXXX) addresses
    if (!fastmem_enabled_) return;
    
    // addr = addr & 0x1FFFFFFF (get physical offset within 512MB)
    emit.AND_imm(addr_reg, addr_reg, 0x1FFFFFFFULL);
    
    // Add fastmem base
    u64 base_addr = reinterpret_cast<u64>(fastmem_base_);
    emit.MOV_imm(arm64::X16, base_addr);
    emit.ADD(addr_reg, addr_reg, arm64::X16);
}

void JitCompiler::byteswap32(ARM64Emitter& emit, int reg) {
    emit.REV32(reg, reg);
}

void JitCompiler::byteswap16(ARM64Emitter& emit, int reg) {
    emit.REV16(reg, reg);
    emit.UXTH(reg, reg);
}

void JitCompiler::byteswap64(ARM64Emitter& emit, int reg) {
    emit.REV(reg, reg);
}

//=============================================================================
// Block Prologue/Epilogue
//=============================================================================

void JitCompiler::emit_block_prologue(ARM64Emitter& emit) {
    // Block entry: X0 = ThreadContext*
    // Save callee-saved registers that we'll use
    emit.STP(arm64::X29, arm64::X30, arm64::SP, -16);
    emit.STP(arm64::X19, arm64::X20, arm64::SP, -32);
    emit.STP(arm64::X21, arm64::X22, arm64::SP, -48);
    emit.SUB_imm(arm64::SP, arm64::SP, 48);
    
    // Set up context register (X19)
    emit.ORR(arm64::CTX_REG, arm64::XZR, arm64::X0);
    
    // Note: MEM_BASE (X20) is no longer used - fastmem_base is loaded
    // directly into X16 in emit_translate_address for each memory access.
    // This avoids potential register corruption issues and simplifies debugging.
}

void JitCompiler::emit_block_epilogue(ARM64Emitter& emit, u32 inst_count) {
    // Increment time base register by actual cycles executed
    // ~4 cycles per instruction to approximate Xbox 360's ~50MHz time base
    u32 cycles = inst_count * 4;
    emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_time_base());
    emit.ADD_imm(arm64::X0, arm64::X0, cycles);
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_time_base());
    
    // Restore callee-saved registers and return
    emit.ADD_imm(arm64::SP, arm64::SP, 48);
    emit.LDP(arm64::X21, arm64::X22, arm64::SP, -48);
    emit.LDP(arm64::X19, arm64::X20, arm64::SP, -32);
    emit.LDP(arm64::X29, arm64::X30, arm64::SP, -16);
    emit.RET();
}

//=============================================================================
// Block Linking
//=============================================================================

void JitCompiler::try_link_block(CompiledBlock* block) {
    // Link exits to already-compiled blocks
    for (auto& link : block->links) {
        if (link.linked) continue;
        
        auto it = block_map_.find(link.target);
        if (it != block_map_.end()) {
            CompiledBlock* target = it->second;
            
            u32* patch_addr = reinterpret_cast<u32*>(
                static_cast<u8*>(block->code) + link.patch_offset
            );
            s64 offset = static_cast<u8*>(target->code) - 
                        reinterpret_cast<u8*>(patch_addr);
            
            if (offset >= -128*1024*1024 && offset < 128*1024*1024) {
                s32 imm26 = offset >> 2;
                *patch_addr = 0x14000000 | (imm26 & 0x03FFFFFF);
                link.linked = true;
                
#ifdef __aarch64__
                __builtin___clear_cache(
                    reinterpret_cast<char*>(patch_addr),
                    reinterpret_cast<char*>(patch_addr) + 4
                );
#endif
            }
        }
    }
    
    // Link other blocks to this one
    for (auto& [addr, other] : block_map_) {
        if (other == block) continue;
        
        for (auto& link : other->links) {
            if (link.linked) continue;
            if (link.target != block->start_addr) continue;
            
            u32* patch_addr = reinterpret_cast<u32*>(
                static_cast<u8*>(other->code) + link.patch_offset
            );
            s64 offset = static_cast<u8*>(block->code) - 
                        reinterpret_cast<u8*>(patch_addr);
            
            if (offset >= -128*1024*1024 && offset < 128*1024*1024) {
                s32 imm26 = offset >> 2;
                *patch_addr = 0x14000000 | (imm26 & 0x03FFFFFF);
                link.linked = true;
                
#ifdef __aarch64__
                __builtin___clear_cache(
                    reinterpret_cast<char*>(patch_addr),
                    reinterpret_cast<char*>(patch_addr) + 4
                );
#endif
            }
        }
    }
}

void JitCompiler::unlink_block(CompiledBlock* block) {
    for (auto& [addr, other] : block_map_) {
        for (auto& link : other->links) {
            if (link.target == block->start_addr && link.linked) {
                link.linked = false;
            }
        }
    }
}

//=============================================================================
// Dispatcher
//=============================================================================

extern "C" void* jit_lookup_block(JitCompiler* jit, GuestAddr pc) {
    return jit->lookup_block_for_dispatch(pc);
}

void* JitCompiler::lookup_block_for_dispatch(GuestAddr pc) {
    std::lock_guard<std::mutex> lock(block_map_mutex_);
    
    auto it = block_map_.find(pc);
    if (it != block_map_.end()) {
        stats_.cache_hits++;
        return it->second->code;
    }
    
    stats_.cache_misses++;
    
    CompiledBlock* block = compile_block_unlocked(pc);
    if (block) {
        return block->code;
    }
    
    return nullptr;
}

CompiledBlock* JitCompiler::compile_block_unlocked(GuestAddr addr) {
    // Allocate new block
    CompiledBlock* block = new CompiledBlock();
    block->start_addr = addr;
    block->code = code_write_ptr_;
    block->execution_count = 0;
    
    // Create temporary buffer for code generation
    u8 temp_buffer[TEMP_BUFFER_SIZE];
    ARM64Emitter emit(temp_buffer, TEMP_BUFFER_SIZE);
    
    ThreadContext ctx_template = {};
    
    GuestAddr pc = addr;
    u32 inst_count = 0;
    bool block_ended = false;
    
    // Reset instruction count for time_base tracking
    current_block_inst_count_ = 0;
    
    // Emit block prologue
    // The block is called with: X0 = ThreadContext*, X1 = memory_base
    // We need to set up CTX_REG (X19) and MEM_BASE (X20)
    emit_block_prologue(emit);
    
    while (!block_ended && inst_count < MAX_BLOCK_INSTRUCTIONS) {
        // Fetch instruction from PPC memory (big-endian)
        u32 ppc_inst = memory_->read_u32(pc);
        
        // Decode
        DecodedInst decoded = Decoder::decode(ppc_inst);
        decoded.raw = ppc_inst;  // Store raw for some instructions
        
        // Debug: log each instruction being compiled
        LOGD("JIT compiling PC=0x%08llX inst=0x%08X type=%d opcode=%d", 
             (unsigned long long)pc, ppc_inst, (int)decoded.type, decoded.opcode);
        
        // Track instruction count for time_base (including this instruction)
        current_block_inst_count_ = inst_count + 1;
        
        // Compile instruction
        compile_instruction(emit, ctx_template, decoded, pc);
        
        inst_count++;
        pc += 4;
        
        // Check if this instruction ends the block
        if (is_block_ending(decoded)) {
            block_ended = true;
        }
    }
    
    // If block didn't end with a branch, add fallthrough
    if (!block_ended) {
        emit.MOV_imm(arm64::X0, pc);
        emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
        emit_block_epilogue(emit, inst_count);
    }
    
    block->size = inst_count;
    block->end_addr = pc;
    block->code_size = emit.size();
    
    // Check for code cache overflow
    if (code_write_ptr_ + emit.size() > code_cache_ + cache_size_) {
        LOGE("JIT code cache overflow! Flushing cache.");
        // Clear all blocks except this one
        for (auto& [a, b] : block_map_) {
            if (b != block) delete b;
        }
        block_map_.clear();
        code_write_ptr_ = code_cache_ + 4096;  // Leave room for dispatcher
        block->code = code_write_ptr_;
    }
    
    // Copy code to executable cache
    memcpy(code_write_ptr_, temp_buffer, emit.size());
    code_write_ptr_ += emit.size();
    
    // Align to 16 bytes
    code_write_ptr_ = reinterpret_cast<u8*>(
        (reinterpret_cast<uintptr_t>(code_write_ptr_) + 15) & ~15
    );
    
#ifdef __aarch64__
    // Clear instruction cache
    __builtin___clear_cache(
        reinterpret_cast<char*>(block->code),
        reinterpret_cast<char*>(block->code) + block->code_size
    );
#endif
    
    // Calculate code hash for SMC detection
    block->hash = 0;
    for (u32 i = 0; i < inst_count; i++) {
        block->hash ^= memory_->read_u32(addr + i * 4);
        block->hash = (block->hash << 5) | (block->hash >> 59);
    }
    
    // Add to cache
    block_map_[addr] = block;
    
    stats_.blocks_compiled++;
    stats_.code_bytes_used = code_write_ptr_ - code_cache_;
    
    LOGD("Compiled block at %08llX (%u instructions, %u bytes)", 
         (unsigned long long)addr, inst_count, (unsigned)block->code_size);
    
    // Debug: dump first 64 instructions of compiled code
    if (block->code_size > 0) {
        LOGI("Block at %08llX code dump (first %u bytes):", 
             (unsigned long long)addr, std::min(block->code_size, 256u));
        u32* code_ptr = reinterpret_cast<u32*>(block->code);
        for (size_t i = 0; i < std::min((size_t)block->code_size / 4, (size_t)64); i++) {
            if (i % 8 == 0) {
                LOGI("  %04zX: %08X %08X %08X %08X %08X %08X %08X %08X",
                     i * 4,
                     (i+0 < block->code_size/4) ? code_ptr[i+0] : 0,
                     (i+1 < block->code_size/4) ? code_ptr[i+1] : 0,
                     (i+2 < block->code_size/4) ? code_ptr[i+2] : 0,
                     (i+3 < block->code_size/4) ? code_ptr[i+3] : 0,
                     (i+4 < block->code_size/4) ? code_ptr[i+4] : 0,
                     (i+5 < block->code_size/4) ? code_ptr[i+5] : 0,
                     (i+6 < block->code_size/4) ? code_ptr[i+6] : 0,
                     (i+7 < block->code_size/4) ? code_ptr[i+7] : 0);
                i += 7;  // Loop will add 1 more
            }
        }
    }
    
    return block;
}

void JitCompiler::generate_dispatcher() {
#ifdef __aarch64__
    ARM64Emitter emit(code_cache_, 4096);
    
    // Dispatcher entry point
    // Arguments: X0 = ThreadContext*, X1 = JitCompiler*
    
    // Save callee-saved registers
    emit.STP(arm64::X29, arm64::X30, arm64::SP, -16);
    emit.STP(arm64::X19, arm64::X20, arm64::SP, -32);
    emit.STP(arm64::X21, arm64::X22, arm64::SP, -48);
    emit.STP(arm64::X23, arm64::X24, arm64::SP, -64);
    emit.STP(arm64::X25, arm64::X26, arm64::SP, -80);
    emit.STP(arm64::X27, arm64::X28, arm64::SP, -96);
    emit.SUB_imm(arm64::SP, arm64::SP, 112);
    
    // Set up context register
    emit.ORR(arm64::CTX_REG, arm64::XZR, arm64::X0);
    
    // Save JIT pointer
    emit.ORR(arm64::JIT_REG, arm64::XZR, arm64::X1);
    
    // Note: MEM_BASE (X20) is no longer used - fastmem_base is loaded
    // directly into X16 in emit_translate_address for each memory access.
    
    // Main loop would go here, but we use execute() loop instead
    // Just restore and return for now
    
    emit.ADD_imm(arm64::SP, arm64::SP, 112);
    emit.LDP(arm64::X27, arm64::X28, arm64::SP, -96);
    emit.LDP(arm64::X25, arm64::X26, arm64::SP, -80);
    emit.LDP(arm64::X23, arm64::X24, arm64::SP, -64);
    emit.LDP(arm64::X21, arm64::X22, arm64::SP, -48);
    emit.LDP(arm64::X19, arm64::X20, arm64::SP, -32);
    emit.LDP(arm64::X29, arm64::X30, arm64::SP, -16);
    emit.RET();
    
    dispatcher_ = reinterpret_cast<DispatcherFunc>(code_cache_);
    
    __builtin___clear_cache(
        reinterpret_cast<char*>(code_cache_),
        reinterpret_cast<char*>(code_cache_) + emit.size()
    );
    
    code_write_ptr_ = code_cache_ + emit.size();
    code_write_ptr_ = reinterpret_cast<u8*>(
        (reinterpret_cast<uintptr_t>(code_write_ptr_) + 15) & ~15
    );
    
    LOGI("Dispatcher generated (%zu bytes)", emit.size());
#endif
}

void JitCompiler::generate_exit_stub() {
#ifdef __aarch64__
    exit_stub_ = code_write_ptr_;
    
    ARM64Emitter emit(code_write_ptr_, 256);
    
    // Exit stub - just return
    emit.RET();
    
    __builtin___clear_cache(
        reinterpret_cast<char*>(exit_stub_),
        reinterpret_cast<char*>(exit_stub_) + emit.size()
    );
    
    code_write_ptr_ += emit.size();
    code_write_ptr_ = reinterpret_cast<u8*>(
        (reinterpret_cast<uintptr_t>(code_write_ptr_) + 15) & ~15
    );
#endif
}

// Static helper implementations
void JitCompiler::helper_syscall(ThreadContext* ctx, JitCompiler* jit) {
    ctx->interrupted = true;
}

void JitCompiler::helper_read_u8(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u8* result) {
    *result = jit->memory_->read_u8(addr);
}

void JitCompiler::helper_read_u16(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u16* result) {
    *result = jit->memory_->read_u16(addr);
}

void JitCompiler::helper_read_u32(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u32* result) {
    *result = jit->memory_->read_u32(addr);
}

void JitCompiler::helper_read_u64(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u64* result) {
    *result = jit->memory_->read_u64(addr);
}

void JitCompiler::helper_write_u8(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u8 value) {
    jit->memory_->write_u8(addr, value);
}

void JitCompiler::helper_write_u16(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u16 value) {
    jit->memory_->write_u16(addr, value);
}

void JitCompiler::helper_write_u32(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u32 value) {
    jit->memory_->write_u32(addr, value);
}

void JitCompiler::helper_write_u64(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u64 value) {
    jit->memory_->write_u64(addr, value);
}

} // namespace x360mu
