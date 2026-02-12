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
#include "x360mu/feature_flags.h"
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
    // Only log writes to GPU virtual range (0xC0000000-0xCFFFFFFF)
    if (addr >= 0xC0000000 && addr < 0xD0000000) {
        trace_count++;
        LOGI("GPU virtual store #%d: addr=0x%08llX", trace_count, (unsigned long long)addr);
    }
    // Also log stores in physical GPU MMIO range (0x7FC00000-0x7FFFFFFF)
    if (addr >= 0x7FC00000 && addr < 0x80000000) {
        trace_count++;
        LOGI("GPU physical store #%d: addr=0x%08llX", trace_count, (unsigned long long)addr);
    }
}

// Debug helper to trace mirror/high physical addresses that would cause SIGSEGV
// This catches addresses in 0x20000000-0x7FFFFFFF range before masking
extern "C" void jit_trace_mirror_access(GuestAddr addr, u32 is_store) {
    if (!FeatureFlags::jit_trace_mirror_access.load(std::memory_order_relaxed)) return;
    
    static int trace_count = 0;
    trace_count++;
    
    // ALWAYS log if address is exactly 0x20000000 or close to it
    if (addr >= 0x1FF00000 && addr <= 0x20100000) {
        LOGI("*** CRITICAL *** #%d: %s addr=0x%08llX - THIS IS THE CRASH ADDRESS!", 
             trace_count, 
             is_store ? "STORE" : "LOAD",
             (unsigned long long)addr);
    }
    // Log first 50, then every 1000th
    else if (trace_count <= 50 || (trace_count % 1000 == 0)) {
        LOGI("MIRROR ACCESS #%d: %s addr=0x%08llX (would be offset 0x%08llX without mask)", 
             trace_count, 
             is_store ? "STORE" : "LOAD",
             (unsigned long long)addr,
             (unsigned long long)addr);  // Raw offset if mask wasn't applied
    }
}

// Trace ALL memory accesses, not just mirror range, to find the crash source
// Trace the ORIGINAL (pre-mask) address to catch invalid/negative pointers
extern "C" void jit_trace_original_addr(GuestAddr original_addr, GuestAddr masked_addr, u32 is_store) {
    // ALWAYS trace PCR region writes with the VALUE being written
    // The value should be in the JIT's X1 register at this point
    if (is_store && masked_addr >= 0x00900000 && masked_addr < 0x00910000) {
        static int pcr_trace_count = 0;
        if (pcr_trace_count++ < 100) {
            // Note: We can't easily get the value here since it's in a register
            // But we know if masked_addr == 0x00900000, that's PCR[0] = TLS pointer being cleared!
            __android_log_print(ANDROID_LOG_ERROR, "JIT_PCR", 
                "JIT STORE to PCR[0x%X]: original=0x%08llX (PCR[0]=TLS ptr!)",
                (u32)(masked_addr - 0x00900000), (unsigned long long)original_addr);
        }
    }
    
    if (!FeatureFlags::jit_trace_memory.load(std::memory_order_relaxed)) return;
    
    static int trace_count = 0;
    trace_count++;
    
    // Check for suspicious addresses that would mask to boundary
    // -4 (0xFFFFFFFC) & 0x1FFFFFFF = 0x1FFFFFFC
    // -1 (0xFFFFFFFF) & 0x1FFFFFFF = 0x1FFFFFFF
    u32 orig32 = (u32)original_addr;
    s32 orig_signed = (s32)orig32;
    
    // Log if original looks like a negative number (indicates bug in game code or our emulation)
    if (orig_signed < 0 && orig_signed > -0x1000) {
        __android_log_print(ANDROID_LOG_ERROR, "JIT_BAD_PTR", 
            "!!! NEGATIVE PTR !!! #%d: %s original=0x%08X (signed=%d) masked=0x%08llX",
            trace_count, is_store ? "STORE" : "LOAD", 
            orig32, orig_signed, (unsigned long long)masked_addr);
    }
    
    // Also log if masked address is near the 512MB boundary (last 64 bytes)
    if (masked_addr >= 0x1FFFFFC0 && FeatureFlags::jit_trace_boundary_access.load(std::memory_order_relaxed)) {
        __android_log_print(ANDROID_LOG_ERROR, "JIT_BOUNDARY", 
            "!!! BOUNDARY ACCESS !!! #%d: %s original=0x%08X masked=0x%08llX",
            trace_count, is_store ? "STORE" : "LOAD", 
            orig32, (unsigned long long)masked_addr);
    }
    
    // Log first 10 for debugging
    if (trace_count <= 10) {
        __android_log_print(ANDROID_LOG_ERROR, "JIT_TRACE", 
            "ACCESS #%d: %s orig=0x%08X masked=0x%08llX", trace_count, 
            is_store ? "STORE" : "LOAD", orig32, (unsigned long long)masked_addr);
    }
}

extern "C" void jit_trace_all_access(GuestAddr addr, u32 is_store) {
    // Kept for compatibility but not used
}

extern "C" void jit_mmio_write_u32(void* mem, GuestAddr addr, u32 value) {
    if (FeatureFlags::jit_trace_mmio.load(std::memory_order_relaxed)) {
        static int call_count = 0;
        call_count++;
        if (call_count <= 100 || (call_count % 10000 == 0)) {
            LOGI("MMIO write_u32 #%d: addr=0x%08llX value=0x%08X", call_count, (unsigned long long)addr, value);
        }
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
    return static_cast<Memory*>(mem)->read_u32(addr);
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
    for (int i = 0; i < 32; i++) ppc_to_arm_[i] = INVALID_REG;
    for (int i = 0; i < MAX_CACHED_GPRS; i++) cached_ppcs_[i] = -1;
    dirty_.reset();
}

void RegisterAllocator::setup_block(GuestAddr addr, u32 inst_count, Memory* memory) {
    reset();

    // Count GPR usage across the block to find the hottest registers
    u32 gpr_use_count[32] = {};

    for (u32 i = 0; i < inst_count; i++) {
        u32 raw = memory->read_u32(addr + i * 4);
        u32 opcode = raw >> 26;

        // Extract common register fields from PPC instruction encoding
        u32 rd_rs = (raw >> 21) & 0x1F;
        u32 ra = (raw >> 16) & 0x1F;
        u32 rb = (raw >> 11) & 0x1F;

        // Count based on instruction format
        switch (opcode) {
            case 14: case 15: // addi/addis (D-form: rD, rA)
            case 32: case 33: case 34: case 35: // lwz/lwzu/lbz/lbzu
            case 40: case 41: case 42: case 43: // lhz/lhzu/lha/lhau
            case 48: case 49: case 50: case 51: // lfs/lfsu/lfd/lfdu
            case 58: // ld/ldu/lwa
                if (rd_rs > 0) gpr_use_count[rd_rs] += 2;  // dest written
                if (ra > 0) gpr_use_count[ra]++;            // base read
                break;
            case 36: case 37: case 38: case 39: // stw/stwu/stb/stbu
            case 44: case 45: case 52: case 53: // sth/sthu/stfs/stfsu
            case 54: case 55: case 62: // stfd/stfdu/std
                if (rd_rs > 0) gpr_use_count[rd_rs]++;  // value read
                if (ra > 0) gpr_use_count[ra]++;         // base read
                break;
            case 11: case 10: // cmpi/cmpli
                if (ra > 0) gpr_use_count[ra]++;
                break;
            case 24: case 25: case 26: case 27: // ori/oris/xori/xoris
            case 28: case 29: // andi./andis.
                if (rd_rs > 0) gpr_use_count[rd_rs]++;
                if (ra > 0) gpr_use_count[ra] += 2;
                break;
            case 20: case 21: case 23: // rlwimi/rlwinm/rlwnm
                if (rd_rs > 0) gpr_use_count[rd_rs]++;
                if (ra > 0) gpr_use_count[ra] += 2;
                if (opcode == 23 && rb > 0) gpr_use_count[rb]++;
                break;
            case 31: // Extended (X/XO-form: rd/rs, ra, rb)
                if (rd_rs > 0) gpr_use_count[rd_rs] += 2;
                if (ra > 0) gpr_use_count[ra]++;
                if (rb > 0) gpr_use_count[rb]++;
                break;
            default:
                break;
        }
    }

    // Don't cache r0 (it has special semantics as 0 in address calculations)
    gpr_use_count[0] = 0;

    // Pick the top MAX_CACHED_GPRS most-used GPRs (minimum 3 uses to be worth caching)
    for (int slot = 0; slot < MAX_CACHED_GPRS; slot++) {
        int best_reg = -1;
        u32 best_count = 2;  // Minimum threshold
        for (int r = 1; r < 32; r++) {
            if (gpr_use_count[r] > best_count) {
                best_count = gpr_use_count[r];
                best_reg = r;
            }
        }
        if (best_reg < 0) break;

        cached_ppcs_[slot] = best_reg;
        ppc_to_arm_[best_reg] = CACHE_REGS[slot];
        gpr_use_count[best_reg] = 0;  // Don't pick again
    }
}

int RegisterAllocator::get_cached_arm_reg(int ppc_reg) const {
    if (ppc_reg < 0 || ppc_reg >= 32) return INVALID_REG;
    return ppc_to_arm_[ppc_reg];
}

void RegisterAllocator::mark_dirty(int ppc_reg) {
    if (ppc_reg >= 0 && ppc_reg < 32 && ppc_to_arm_[ppc_reg] != INVALID_REG) {
        dirty_.set(ppc_reg);
    }
}

bool RegisterAllocator::is_dirty(int ppc_reg) const {
    if (ppc_reg < 0 || ppc_reg >= 32) return false;
    return dirty_.test(ppc_reg);
}

bool RegisterAllocator::is_cached(int ppc_reg) const {
    if (ppc_reg < 0 || ppc_reg >= 32) return false;
    return ppc_to_arm_[ppc_reg] != INVALID_REG;
}

int RegisterAllocator::cached_ppc_reg(int slot) const {
    if (slot < 0 || slot >= MAX_CACHED_GPRS) return -1;
    return cached_ppcs_[slot];
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
    
#ifdef __aarch64__
    // JIT requires fastmem to be enabled - without it, memory accesses will crash
    if (!fastmem_enabled_) {
        // Return 0 to signal CPU should fall back to interpreter
        // Don't set interrupted - that's for syscalls
        return 0;
    }
    
    // Run the dispatcher which will execute compiled code
    if (dispatcher_) {
        ctx.running = true;
        ctx.interrupted = false;
        
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
            
            // Execute the block
            using BlockFn = void(*)(ThreadContext*, u8*);
            BlockFn fn = reinterpret_cast<BlockFn>(block->code);
            
            // DEBUG: Log block execution (controlled by feature flag)
            if (FeatureFlags::jit_trace_blocks.load(std::memory_order_relaxed)) {
                static int exec_count = 0;
                exec_count++;
                if (exec_count <= 20 || exec_count % 10000 == 0) {
                    __android_log_print(ANDROID_LOG_ERROR, "JIT_EXEC", 
                        "Executing block #%d at PC=0x%08llX (block code=%p)", 
                        exec_count, (unsigned long long)ctx.pc, block->code);
                }
            }
            
            // Idle loop optimization: if this block is an idle loop that has
            // been executed many times, advance time base and yield CPU
            if (block->is_idle_loop && block->execution_count > 10) {
                ctx.time_base += 4000;  // Skip ~1000 instructions worth of time
                cycles_executed += 1000;
                stats_.idle_loops_skipped++;
                std::this_thread::yield();
                continue;
            }

            // Execute the block (fastmem_base is now embedded in block code)
            fn(&ctx, nullptr);

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
            // Route atomic loads (lwarx/ldarx) to dedicated handler
            if (inst.opcode == 31 && (inst.xo == 20 || inst.xo == 84)) {
                compile_atomic_load(emit, inst);
            } else {
                compile_load(emit, inst);
            }
            break;

        case DecodedInst::Type::Store:
        case DecodedInst::Type::StoreUpdate:
            // Route atomic stores (stwcx./stdcx.) to dedicated handler
            if (inst.opcode == 31 && (inst.xo == 150 || inst.xo == 214)) {
                compile_atomic_store(emit, inst);
            } else {
                compile_store(emit, inst);
            }
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

        case DecodedInst::Type::FNeg:
        case DecodedInst::Type::FAbs:
            compile_float_unary(emit, inst);
            break;

        case DecodedInst::Type::FCompare:
            compile_float_compare(emit, inst);
            break;

        case DecodedInst::Type::FConvert:
            compile_float_convert(emit, inst);
            break;

        case DecodedInst::Type::VAdd:
        case DecodedInst::Type::VSub:
        case DecodedInst::Type::VMul:
        case DecodedInst::Type::VDiv:
        case DecodedInst::Type::VLogical:
            compile_vector(emit, inst);
            break;

        case DecodedInst::Type::VPerm:
        case DecodedInst::Type::VMerge:
        case DecodedInst::Type::VSplat:
            compile_vector_permute(emit, inst);
            break;

        case DecodedInst::Type::VCompare:
            compile_vector_compare(emit, inst);
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

        case DecodedInst::Type::RFI:
            compile_rfi(emit, inst);
            break;

        case DecodedInst::Type::TW:
        case DecodedInst::Type::TD:
            // Trap instructions - NOP for game compatibility
            // Games rarely trigger traps; if they do, just skip
            emit.NOP();
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
    
    else if (inst.opcode == 30) { // 64-bit rotate instructions (rldic, rldicl, rldicr, rldimi, rldcl, rldcr)
        load_gpr(emit, arm64::X0, inst.rs);

        // Extract sub-opcode from bits 27-30 (2-4 bits depending on form)
        u32 sub_xo = (inst.raw >> 1) & 0xF;  // bits 27-30

        // 6-bit shift: sh[0:4] from bits 16-20, sh[5] from bit 30
        u32 sh6 = inst.sh;  // Already extracted by decoder with bit 30

        // 6-bit mask begin (mb6): mb[0:4] from bits 21-25, mb[5] from bit 26
        u32 mb6 = inst.mb;  // Already extracted by decoder

        switch (sub_xo & 0x7) {  // Lower 3 bits determine the form
            case 0: // rldicl - Rotate Left Doubleword Immediate then Clear Left
            {
                if (sh6 != 0) {
                    emit.ROR_imm(arm64::X0, arm64::X0, 64 - sh6);
                }
                // Clear bits 0 to mb6-1 (i.e., mask = bits mb6..63)
                if (mb6 > 0) {
                    u64 mask = ~0ULL >> mb6;
                    emit.MOV_imm(arm64::X1, mask);
                    emit.AND(arm64::X0, arm64::X0, arm64::X1);
                }
                break;
            }
            case 1: // rldicr - Rotate Left Doubleword Immediate then Clear Right
            {
                if (sh6 != 0) {
                    emit.ROR_imm(arm64::X0, arm64::X0, 64 - sh6);
                }
                // Clear bits me6+1 to 63 (me6 = mb6 in this encoding)
                u32 me6 = mb6;
                if (me6 < 63) {
                    u64 mask = ~0ULL << (63 - me6);
                    emit.MOV_imm(arm64::X1, mask);
                    emit.AND(arm64::X0, arm64::X0, arm64::X1);
                }
                break;
            }
            case 2: // rldic - Rotate Left Doubleword Immediate then Clear
            {
                if (sh6 != 0) {
                    emit.ROR_imm(arm64::X0, arm64::X0, 64 - sh6);
                }
                // Clear bits 0..mb6-1 and bits 63-sh6+1..63
                u64 mask = (~0ULL >> mb6) & (~0ULL << sh6);
                emit.MOV_imm(arm64::X1, mask);
                emit.AND(arm64::X0, arm64::X0, arm64::X1);
                break;
            }
            case 3: // rldimi - Rotate Left Doubleword Immediate then Mask Insert
            {
                load_gpr(emit, arm64::X2, inst.ra);
                if (sh6 != 0) {
                    emit.ROR_imm(arm64::X0, arm64::X0, 64 - sh6);
                }
                u64 mask = (~0ULL >> mb6) & (~0ULL << sh6);
                emit.MOV_imm(arm64::X1, mask);
                emit.AND(arm64::X0, arm64::X0, arm64::X1);
                emit.MOV_imm(arm64::X3, ~mask);
                emit.AND(arm64::X2, arm64::X2, arm64::X3);
                emit.ORR(arm64::X0, arm64::X0, arm64::X2);
                break;
            }
            case 4: // rldcl - Rotate Left Doubleword then Clear Left (register shift)
            {
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND_imm(arm64::X1, arm64::X1, 0x3F);
                emit.MOV_imm(arm64::X2, 64);
                emit.SUB(arm64::X2, arm64::X2, arm64::X1);
                emit.ROR(arm64::X0, arm64::X0, arm64::X2);
                if (mb6 > 0) {
                    u64 mask = ~0ULL >> mb6;
                    emit.MOV_imm(arm64::X1, mask);
                    emit.AND(arm64::X0, arm64::X0, arm64::X1);
                }
                break;
            }
            case 5: // rldcr - Rotate Left Doubleword then Clear Right (register shift)
            {
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND_imm(arm64::X1, arm64::X1, 0x3F);
                emit.MOV_imm(arm64::X2, 64);
                emit.SUB(arm64::X2, arm64::X2, arm64::X1);
                emit.ROR(arm64::X0, arm64::X0, arm64::X2);
                u32 me6 = mb6;
                if (me6 < 63) {
                    u64 mask = ~0ULL << (63 - me6);
                    emit.MOV_imm(arm64::X1, mask);
                    emit.AND(arm64::X0, arm64::X0, arm64::X1);
                }
                break;
            }
            default:
                // Unknown 64-bit rotate sub-opcode
                break;
        }

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
    
    // Save original EA for MMIO path (X2 = original EA)
    emit.ORR(arm64::X2, arm64::XZR, arm64::X0);
    
    // DEBUG: Trace addresses in the dangerous mirror range (0x20000000-0x7FFFFFFF)
    // These would cause SIGSEGV if mask isn't applied properly
    {
        u8* skip_trace_low = nullptr;
        u8* skip_trace_high = nullptr;
        
        // Check if addr >= 0x20000000
        emit.MOV_imm(arm64::X16, 0x20000000ULL);
        emit.CMP(arm64::X0, arm64::X16);
        skip_trace_low = emit.current();
        emit.B_cond(arm64_cond::CC, 0);  // Skip if addr < 0x20000000
        
        // Check if addr < 0x80000000 (we only care about physical mirror range)
        emit.MOV_imm(arm64::X16, 0x80000000ULL);
        emit.CMP(arm64::X0, arm64::X16);
        skip_trace_high = emit.current();
        emit.B_cond(arm64_cond::CS, 0);  // Skip if addr >= 0x80000000
        
        // Address is in dangerous range! Log it.
        emit.SUB_imm(arm64::SP, arm64::SP, 48);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 32);
        
        // X0 = addr, X1 = is_store (0 for load)
        emit.MOV_imm(arm64::X1, 0);  // is_store = false
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_mirror_access);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 48);
        
        emit.patch_branch(reinterpret_cast<u32*>(skip_trace_low), emit.current());
        emit.patch_branch(reinterpret_cast<u32*>(skip_trace_high), emit.current());
    }

    // === Address routing for loads ===
    // 1. Kernel addresses (>= 0xA0000000) → MMIO path
    // 2. Usermode virtual (0x80000000-0x9FFFFFFF) → mask to physical
    // 3. GPU MMIO physical (0x7FC00000-0x7FFFFFFF) → MMIO path
    // 4. All other physical (0x00000000-0x7FBFFFFF) → mask and use fastmem
    
    emit.MOV_imm(arm64::X16, 0xA0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* kernel_space_load = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Branch if addr >= 0xA0000000 -> MMIO path
    
    // Check for GPU MMIO physical range (0x7FC00000-0x7FFFFFFF)
    // This must be checked BEFORE masking, as it's a special physical range
    emit.MOV_imm(arm64::X16, 0x7FC00000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_gpu_mmio = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0x7FC00000
    
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_gpu_mmio = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch to MMIO if addr < 0x80000000 (in GPU range)
    
    // Not in GPU MMIO range
    emit.patch_branch(reinterpret_cast<u32*>(below_gpu_mmio), emit.current());
    
    // For all addresses < 0xA0000000 (except GPU MMIO), apply mask to get physical
    // This handles:
    // - Physical 0x00000000-0x1FFFFFFF → unchanged (main RAM)
    // - Physical 0x20000000-0x7FBFFFFF → masked to 0x00000000-0x1FFFFFFF (mirrors)
    // - Virtual 0x80000000-0x9FFFFFFF → masked to 0x00000000-0x1FFFFFFF
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // DEBUG: Trace the masked address before fastmem access
    // If we still crash, this will show what addresses reach fastmem
    {
        emit.SUB_imm(arm64::SP, arm64::SP, 48);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 32);
        
        // X0 = masked addr, X1 = is_store (0 for load)
        emit.MOV_imm(arm64::X1, 0);
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_all_access);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 48);
    }
    
    // === FASTMEM PATH for loads ===
    // X0 now contains physical address in range 0x00000000-0x1FFFFFFF
    // Add fastmem base directly (no need to call emit_translate_address, we already masked)
    emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
    emit.ADD(arm64::X0, arm64::X0, arm64::X16);
    
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
                case 341: // lwax (load word algebraic - sign extend)
                    emit.LDR(dest_reg, arm64::X0);
                    byteswap32(emit, dest_reg);
                    emit.SXTW(dest_reg, dest_reg);
                    break;
                case 535: // lfsx (load float single indexed)
                    emit.LDR(dest_reg, arm64::X0);
                    byteswap32(emit, dest_reg);
                    break;
                case 599: // lfdx (load float double indexed)
                    emit.LDR(dest_reg, arm64::X0);
                    byteswap64(emit, dest_reg);
                    break;
                // Byte-reversed loads - no byteswap needed since memory is
                // big-endian and ARM is little-endian, the raw read IS reversed
                case 534: // lwbrx (load word byte-reverse indexed)
                    emit.LDR(dest_reg, arm64::X0);
                    emit.UXTW(dest_reg, dest_reg);
                    break;
                case 790: // lhbrx (load halfword byte-reverse indexed)
                    emit.LDRH(dest_reg, arm64::X0);
                    break;
                case 532: // ldbrx (load doubleword byte-reverse indexed)
                    emit.LDR(dest_reg, arm64::X0);
                    break;
            }
            break;
    }
    
    // Jump past MMIO path
    u8* skip_mmio_load = emit.current();
    emit.B(0);
    
    // === MMIO PATH for loads ===
    // Kernel addresses (>= 0xA0000000) and GPU MMIO (0x7FC00000-0x7FFFFFFF) land here
    emit.patch_branch(reinterpret_cast<u32*>(kernel_space_load), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(is_gpu_mmio), emit.current());
    
    // Call helper function with original virtual address (X2)
    // jit_mmio_read_XX(memory, addr) returns value - Memory class handles routing
    
    // Load memory pointer into X0
    emit.LDR(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, memory));
    
    // X1 = original addr (from X2)
    emit.ORR(arm64::X1, arm64::XZR, arm64::X2);
    
    // Determine helper function based on load size
    u64 mmio_read_helper = 0;
    switch (inst.opcode) {
        case 32: case 33: // lwz/lwzu
            mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u32);
            break;
        case 34: case 35: // lbz/lbzu
            mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u8);
            break;
        case 40: case 41: case 42: case 43: // lhz/lhzu/lha/lhau
            mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u16);
            break;
        case 48: case 49: case 50: case 51: // lfs/lfsu/lfd/lfdu
        case 58: // ld/ldu/lwa
            mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u64);
            break;
        case 31: // Extended loads
            switch (inst.xo) {
                case 23: case 534: case 341: // lwzx/lwbrx/lwax
                    mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u32); break;
                case 87: // lbzx
                    mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u8); break;
                case 279: case 343: case 790: // lhzx/lhax/lhbrx
                    mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u16); break;
                case 21: case 532: case 535: case 599: // ldx/ldbrx/lfsx/lfdx
                    mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u64); break;
                default: mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u32); break;
            }
            break;
        default:
            mmio_read_helper = reinterpret_cast<u64>(&jit_mmio_read_u32);
            break;
    }
    
    emit.MOV_imm(arm64::X16, mmio_read_helper);
    emit.BLR(arm64::X16);
    
    // Result is in X0, move to dest_reg (X1)
    emit.ORR(arm64::X1, arm64::XZR, arm64::X0);
    
    // === DONE ===
    emit.patch_branch(reinterpret_cast<u32*>(skip_mmio_load), emit.current());
    
    store_gpr(emit, inst.rd, arm64::X1);
    
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
    
    // DEBUG: Trace addresses in the dangerous mirror range (0x20000000-0x7FFFFFFF)
    // These would cause SIGSEGV if mask isn't applied properly
    {
        u8* skip_trace_low = nullptr;
        u8* skip_trace_high = nullptr;
        
        // Check if addr >= 0x20000000
        emit.MOV_imm(arm64::X16, 0x20000000ULL);
        emit.CMP(arm64::X0, arm64::X16);
        skip_trace_low = emit.current();
        emit.B_cond(arm64_cond::CC, 0);  // Skip if addr < 0x20000000
        
        // Check if addr < 0x80000000 (we only care about physical mirror range)
        emit.MOV_imm(arm64::X16, 0x80000000ULL);
        emit.CMP(arm64::X0, arm64::X16);
        skip_trace_high = emit.current();
        emit.B_cond(arm64_cond::CS, 0);  // Skip if addr >= 0x80000000
        
        // Address is in dangerous range! Log it.
        emit.SUB_imm(arm64::SP, arm64::SP, 48);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 32);
        
        // X0 = addr, X1 = is_store (1 for store)
        emit.MOV_imm(arm64::X1, 1);  // is_store = true
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_mirror_access);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 48);
        
        emit.patch_branch(reinterpret_cast<u32*>(skip_trace_low), emit.current());
        emit.patch_branch(reinterpret_cast<u32*>(skip_trace_high), emit.current());
    }
    
    // Load value to store
    load_gpr(emit, arm64::X1, inst.rs);
    
    // Save original virtual address for MMIO path (X2 = original EA)
    emit.ORR(arm64::X2, arm64::XZR, arm64::X0);
    
    // === Address routing for stores (v4 - correct mirror handling) ===
    // Routes to MMIO path for:
    // 1. Kernel addresses (>= 0xA0000000)
    // 2. GPU virtual mapping (0xC0000000-0xC3FFFFFF)
    // 3. Alternate GPU virtual (0xEC800000-0xECFFFFFF)
    // 4. GPU MMIO physical (0x7FC00000-0x7FFFFFFF)
    // All other addresses: mask with 0x1FFFFFFF to handle mirrors → fastmem
    
    // First, check for GPU MMIO virtual addresses (0xC0000000-0xC3FFFFFF)
    emit.MOV_imm(arm64::X16, 0xC0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_gpu_virt = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0xC0000000
    
    emit.MOV_imm(arm64::X16, 0xC4000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_gpu_virt = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch to MMIO path if addr < 0xC4000000 (in GPU virtual range)
    
    // Not in primary GPU virtual range, continue checking other ranges
    emit.patch_branch(reinterpret_cast<u32*>(below_gpu_virt), emit.current());
    
    // Check alternate GPU virtual range (0xEC800000-0xECFFFFFF)
    emit.MOV_imm(arm64::X16, 0xEC800000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_alt_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0xEC800000
    
    emit.MOV_imm(arm64::X16, 0xED000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_alt_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch to MMIO path if addr < 0xED000000 (in alt GPU range)
    
    // Not in alternate GPU range either
    emit.patch_branch(reinterpret_cast<u32*>(below_alt_gpu), emit.current());
    
    // Check for kernel addresses (>= 0xA0000000)
    emit.MOV_imm(arm64::X16, 0xA0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* kernel_space = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Branch if addr >= 0xA0000000 -> MMIO path
    
    // Check for GPU MMIO physical range (0x7FC00000-0x7FFFFFFF)
    emit.MOV_imm(arm64::X16, 0x7FC00000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_gpu_phys = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0x7FC00000
    
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_gpu_phys = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch to MMIO if < 0x80000000 (in GPU MMIO range)
    
    // Not in GPU physical MMIO range
    emit.patch_branch(reinterpret_cast<u32*>(below_gpu_phys), emit.current());
    
    // Save ORIGINAL address in X4 before masking (for debugging)
    emit.ORR(arm64::X4, arm64::XZR, arm64::X0);
    
    // For all other addresses, apply mask to get physical address in 512MB range
    // This handles:
    // - Physical 0x00000000-0x1FFFFFFF → unchanged (main RAM)
    // - Physical 0x20000000-0x7FBFFFFF → masked to 0x00000000-0x1FFFFFFF (mirrors)
    // - Virtual 0x80000000-0x9FFFFFFF → masked to 0x00000000-0x1FFFFFFF
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // Fastmem path - address is now in valid range
    u8* fastmem_path = emit.current();
    emit.B(0);  // Branch to fastmem path
    
    // === MMIO PATH ===
    // GPU virtual, GPU physical, and kernel addresses land here
    emit.patch_branch(reinterpret_cast<u32*>(is_gpu_virt), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(is_alt_gpu), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(kernel_space), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(is_gpu_phys), emit.current());
    
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
                case 151: case 662: // stwx/stwbrx
                    mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u32); break;
                case 215: // stbx
                    mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u8); break;
                case 407: case 918: // sthx/sthbrx
                    mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u16); break;
                case 149: case 660: case 727: // stdx/stdbrx/stfdx
                    mmio_helper = reinterpret_cast<u64>(&jit_mmio_write_u64); break;
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
    // X0 already has the masked physical address (0x00000000-0x1FFFFFFF)
    // X4 has the ORIGINAL address (saved before masking)
    emit.patch_branch(reinterpret_cast<u32*>(fastmem_path), emit.current());
    
    // DEBUG: Trace BOTH original and masked address to catch negative/invalid pointers
    {
        emit.SUB_imm(arm64::SP, arm64::SP, 64);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X4, arm64::X5, arm64::SP, 32);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 48);
        
        // Args: X0 = original addr, X1 = masked addr, X2 = is_store
        emit.ORR(arm64::X5, arm64::XZR, arm64::X0);  // Save masked in X5
        emit.ORR(arm64::X0, arm64::XZR, arm64::X4);  // X0 = original
        emit.ORR(arm64::X1, arm64::XZR, arm64::X5);  // X1 = masked
        emit.MOV_imm(arm64::X2, 1);                   // X2 = is_store
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_original_addr);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 48);
        emit.LDP(arm64::X4, arm64::X5, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 64);
    }
    
    // Reload value since we may have clobbered X1 in MMIO path setup
    load_gpr(emit, arm64::X1, inst.rs);
    
    // Add fastmem base (address already masked above)
    emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
    emit.ADD(arm64::X0, arm64::X0, arm64::X16);
    
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
                case 727: // stfdx (store float double indexed)
                    byteswap64(emit, arm64::X1);
                    emit.STR(arm64::X1, arm64::X0);
                    break;
                // Byte-reversed stores - no byteswap needed since memory is
                // big-endian and ARM is little-endian, storing raw IS reversed
                case 662: // stwbrx (store word byte-reverse indexed)
                    emit.STR(arm64::X1, arm64::X0);
                    break;
                case 918: // sthbrx (store halfword byte-reverse indexed)
                    emit.STRH(arm64::X1, arm64::X0);
                    break;
                case 660: // stdbrx (store doubleword byte-reverse indexed)
                    emit.STR(arm64::X1, arm64::X0);
                    break;
            }
            break;
    }
    
    // === DONE ===
    // Patch skip_fastmem branch to here
    emit.patch_branch(reinterpret_cast<u32*>(skip_fastmem), emit.current());
    
    // Update RA for update forms (use saved EA from X3)
    if (is_update && inst.ra != 0) {
        store_gpr(emit, inst.ra, arm64::X3);
    }
}

void JitCompiler::compile_load_multiple(ARM64Emitter& emit, const DecodedInst& inst) {
    calc_ea(emit, arm64::X0, inst.ra, inst.simm);
    
    // Save original EA for slow path
    emit.ORR(arm64::X2, arm64::XZR, arm64::X0);
    
    // === Address routing (v4 - correct mirror handling) ===
    // Check for kernel addresses (>= 0xA0000000) - bail to slow path
    emit.MOV_imm(arm64::X16, 0xA0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* kernel_addr = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Branch if >= 0xA0000000
    
    // Check for GPU MMIO physical range (0x7FC00000-0x7FFFFFFF)
    emit.MOV_imm(arm64::X16, 0x7FC00000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0x7FC00000
    
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch to slow path if < 0x80000000 (in GPU MMIO)
    
    emit.patch_branch(reinterpret_cast<u32*>(below_gpu), emit.current());
    
    // For all other addresses, apply mask to get physical address in 512MB range
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // DEBUG: Trace the masked address before fastmem access (lmw)
    {
        emit.SUB_imm(arm64::SP, arm64::SP, 48);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.MOV_imm(arm64::X1, 0);  // is_store = false (lmw is load)
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_all_access);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 48);
    }
    
    // Fastmem path - address is in main RAM (< 0x20000000 after masking)
    // CRITICAL FIX: Must mask EACH address including offset to avoid overflow past 512MB
    // Xbox 360 memory wraps, so 0x1FFFFFC0 + 64 should wrap to 0x00000000 not crash at 0x20000000
    for (u32 r = inst.rd; r < 32; r++) {
        // Calculate full address = base + offset
        emit.ADD_imm(arm64::X3, arm64::X0, (r - inst.rd) * 4);
        // Mask to 512MB to handle wrap-around
        emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
        emit.AND(arm64::X3, arm64::X3, arm64::X16);
        // Add fastmem base
        emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
        emit.ADD(arm64::X3, arm64::X3, arm64::X16);
        // Load from masked address
        emit.LDR(arm64::X1, arm64::X3);
        byteswap32(emit, arm64::X1);
        store_gpr(emit, r, arm64::X1);
    }
    
    u8* done = emit.current();
    emit.B(0);  // Jump to end
    
    // Slow path - kernel addresses and GPU MMIO
    emit.patch_branch(reinterpret_cast<u32*>(kernel_addr), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(is_gpu), emit.current());
    
    // X2 has the original EA
    for (u32 r = inst.rd; r < 32; r++) {
        // Calculate address for this register
        emit.ADD_imm(arm64::X1, arm64::X2, (r - inst.rd) * 4);
        
        // Load memory pointer
        emit.LDR(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, memory));
        
        // Call jit_mmio_read_u32(memory, addr)
        u64 read_func = reinterpret_cast<u64>(&jit_mmio_read_u32);
        emit.MOV_imm(arm64::X16, read_func);
        emit.BLR(arm64::X16);
        
        // Result is in X0, store to GPR
        store_gpr(emit, r, arm64::X0);
    }
    
    emit.patch_branch(reinterpret_cast<u32*>(done), emit.current());
}

void JitCompiler::compile_store_multiple(ARM64Emitter& emit, const DecodedInst& inst) {
    calc_ea(emit, arm64::X0, inst.ra, inst.simm);
    
    // Save original EA for slow path
    emit.ORR(arm64::X3, arm64::XZR, arm64::X0);
    
    // === Address routing (v4 - correct mirror handling) ===
    // Check for kernel addresses (>= 0xA0000000) - bail to slow path
    emit.MOV_imm(arm64::X16, 0xA0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* kernel_addr = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Branch if >= 0xA0000000
    
    // Check for GPU MMIO physical range (0x7FC00000-0x7FFFFFFF)
    emit.MOV_imm(arm64::X16, 0x7FC00000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0x7FC00000
    
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch to slow path if < 0x80000000 (in GPU MMIO)
    
    emit.patch_branch(reinterpret_cast<u32*>(below_gpu), emit.current());
    
    // For all other addresses, apply mask to get physical address in 512MB range
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // DEBUG: Trace the masked address before fastmem access (stmw)
    {
        emit.SUB_imm(arm64::SP, arm64::SP, 48);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.MOV_imm(arm64::X1, 1);  // is_store = true (stmw is store)
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_all_access);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 48);
    }
    
    // Fastmem path - address is now in valid range
    // CRITICAL FIX: Must mask EACH address including offset to avoid overflow past 512MB
    for (u32 r = inst.rs; r < 32; r++) {
        load_gpr(emit, arm64::X1, r);
        byteswap32(emit, arm64::X1);
        // Calculate full address = base + offset
        emit.ADD_imm(arm64::X4, arm64::X0, (r - inst.rs) * 4);
        // Mask to 512MB to handle wrap-around
        emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
        emit.AND(arm64::X4, arm64::X4, arm64::X16);
        // Add fastmem base
        emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
        emit.ADD(arm64::X4, arm64::X4, arm64::X16);
        // Store to masked address
        emit.STR(arm64::X1, arm64::X4);
    }
    
    u8* done = emit.current();
    emit.B(0);  // Jump to end
    
    // Slow path - kernel addresses and GPU MMIO
    emit.patch_branch(reinterpret_cast<u32*>(kernel_addr), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(is_gpu), emit.current());
    
    // X3 has the original EA
    for (u32 r = inst.rs; r < 32; r++) {
        // Load value to store
        load_gpr(emit, arm64::X2, r);
        
        // Calculate address for this register
        emit.ADD_imm(arm64::X1, arm64::X3, (r - inst.rs) * 4);
        
        // Load memory pointer
        emit.LDR(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, memory));
        
        // Call jit_mmio_write_u32(memory, addr, value)
        u64 write_func = reinterpret_cast<u64>(&jit_mmio_write_u32);
        emit.MOV_imm(arm64::X16, write_func);
        emit.BLR(arm64::X16);
    }
    
    emit.patch_branch(reinterpret_cast<u32*>(done), emit.current());
}

//=============================================================================
// Atomic Operations (lwarx/stwcx) - Per-Thread Reservation
//=============================================================================

void JitCompiler::compile_atomic_load(ARM64Emitter& emit, const DecodedInst& inst) {
    // lwarx rD, rA, rB - Load Word And Reserve Indexed
    calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    
    // Save untranslated address for reservation
    emit.ORR(arm64::X2, arm64::XZR, arm64::X0);
    
    // === Address routing (v4 - correct mirror handling) ===
    // Check for kernel addresses (>= 0xA0000000)
    emit.MOV_imm(arm64::X16, 0xA0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* kernel_addr = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Branch to NOP path if kernel
    
    // Check for GPU MMIO physical range (0x7FC00000-0x7FFFFFFF)
    emit.MOV_imm(arm64::X16, 0x7FC00000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0x7FC00000
    
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch to NOP if < 0x80000000 (in GPU MMIO)
    
    emit.patch_branch(reinterpret_cast<u32*>(below_gpu), emit.current());
    
    // For all other addresses, apply mask to get physical address in 512MB range
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // DEBUG: Trace the masked address before fastmem access (lwarx)
    {
        emit.SUB_imm(arm64::SP, arm64::SP, 48);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.MOV_imm(arm64::X1, 0);  // is_store = false (lwarx is load)
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_all_access);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 48);
    }
    
    // Fastmem path - address is in main RAM
    emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
    emit.ADD(arm64::X0, arm64::X0, arm64::X16);
    
    // Load the value with exclusive access
    emit.LDR(arm64::X1, arm64::X0, 0);
    byteswap32(emit, arm64::X1);
    
    store_gpr(emit, inst.rd, arm64::X1);
    
    // Store per-thread reservation in ThreadContext
    emit.STR(arm64::X2, arm64::CTX_REG, offsetof(ThreadContext, reservation_addr));
    emit.MOV_imm(arm64::X3, 4);  // reservation size = 4 bytes
    emit.STR(arm64::X3, arm64::CTX_REG, offsetof(ThreadContext, reservation_size));
    emit.MOV_imm(arm64::X3, 1);  // has_reservation = true
    emit.STRB(arm64::X3, arm64::CTX_REG, offsetof(ThreadContext, has_reservation));
    
    u8* done = emit.current();
    emit.B(0);  // Jump to end
    
    // NOP path for unsupported addresses (kernel/GPU MMIO)
    // Return 0 and don't set reservation
    emit.patch_branch(reinterpret_cast<u32*>(kernel_addr), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(is_gpu), emit.current());
    emit.MOV_imm(arm64::X1, 0);
    store_gpr(emit, inst.rd, arm64::X1);
    
    emit.patch_branch(reinterpret_cast<u32*>(done), emit.current());
}

void JitCompiler::compile_atomic_store(ARM64Emitter& emit, const DecodedInst& inst) {
    // stwcx. rS, rA, rB - Store Word Conditional Indexed
    calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    
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
    
    // Addresses match - need proper address routing before store
    // Save original address for potential failure path
    emit.ORR(arm64::X4, arm64::XZR, arm64::X0);
    
    // === Address routing (v4 - correct mirror handling) ===
    // Check for kernel addresses (>= 0xA0000000)
    emit.MOV_imm(arm64::X16, 0xA0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* kernel_addr = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Branch to failure if kernel
    
    // Check for GPU MMIO physical range (0x7FC00000-0x7FFFFFFF)
    emit.MOV_imm(arm64::X16, 0x7FC00000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0x7FC00000
    
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch to failure if < 0x80000000 (in GPU MMIO)
    
    emit.patch_branch(reinterpret_cast<u32*>(below_gpu), emit.current());
    
    // For all other addresses, apply mask to get physical address in 512MB range
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // DEBUG: Trace the masked address before fastmem access (stwcx.)
    {
        emit.SUB_imm(arm64::SP, arm64::SP, 48);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.MOV_imm(arm64::X1, 1);  // is_store = true (stwcx. is store)
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_all_access);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 48);
    }
    
    // Fastmem path - address is in main RAM
    emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
    emit.ADD(arm64::X0, arm64::X0, arm64::X16);
    
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
    
    // Failure path for kernel/GPU MMIO addresses
    emit.patch_branch(reinterpret_cast<u32*>(kernel_addr), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(is_gpu), emit.current());
    
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
    
    // === Address routing (v4 - correct mirror handling) ===
    // Check for kernel addresses (>= 0xA0000000)
    emit.MOV_imm(arm64::X16, 0xA0000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* kernel_addr = emit.current();
    emit.B_cond(arm64_cond::CS, 0);  // Skip dcbz for kernel addresses
    
    // Check for GPU MMIO physical range (0x7FC00000-0x7FFFFFFF)
    emit.MOV_imm(arm64::X16, 0x7FC00000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* below_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Branch if addr < 0x7FC00000
    
    emit.MOV_imm(arm64::X16, 0x80000000ULL);
    emit.CMP(arm64::X0, arm64::X16);
    u8* is_gpu = emit.current();
    emit.B_cond(arm64_cond::CC, 0);  // Skip dcbz if < 0x80000000 (in GPU MMIO)
    
    emit.patch_branch(reinterpret_cast<u32*>(below_gpu), emit.current());
    
    // For all other addresses, apply mask to get physical address in 512MB range
    emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
    emit.AND(arm64::X0, arm64::X0, arm64::X16);
    
    // DEBUG: Trace the masked address before fastmem access (dcbz)
    {
        emit.SUB_imm(arm64::SP, arm64::SP, 48);
        emit.STP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.STP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.STP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.MOV_imm(arm64::X1, 1);  // is_store = true (dcbz writes zeros)
        u64 trace_func = reinterpret_cast<u64>(&jit_trace_all_access);
        emit.MOV_imm(arm64::X16, trace_func);
        emit.BLR(arm64::X16);
        emit.LDP(arm64::X30, arm64::XZR, arm64::SP, 32);
        emit.LDP(arm64::X2, arm64::X3, arm64::SP, 16);
        emit.LDP(arm64::X0, arm64::X1, arm64::SP, 0);
        emit.ADD_imm(arm64::SP, arm64::SP, 48);
    }
    
    // Fastmem path - address is in main RAM
    emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
    emit.ADD(arm64::X0, arm64::X0, arm64::X16);
    
    // Zero 32 bytes using 4 STR of 64-bit zeros = 4 * 8 = 32 bytes
    emit.STR(arm64::XZR, arm64::X0, 0);
    emit.STR(arm64::XZR, arm64::X0, 8);
    emit.STR(arm64::XZR, arm64::X0, 16);
    emit.STR(arm64::XZR, arm64::X0, 24);
    
    // Done - skip NOP path
    u8* done = emit.current();
    emit.B(0);
    
    // NOP path for unsupported addresses (kernel/GPU MMIO)
    emit.patch_branch(reinterpret_cast<u32*>(kernel_addr), emit.current());
    emit.patch_branch(reinterpret_cast<u32*>(is_gpu), emit.current());
    emit.NOP();  // Just skip for invalid addresses
    
    emit.patch_branch(reinterpret_cast<u32*>(done), emit.current());
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

    // Emit epilogue without RET for block linking
    emit_block_epilogue_for_link(emit, current_block_inst_count_);

    // Emit linkable B (default: skip to RET fallback)
    u32 link_offset = static_cast<u32>(emit.size());
    emit.B(4);  // Default: jump over to RET below

    // Record link so try_link_block can patch the B to jump to target block
    if (block && !link) {  // Don't link bl (call) targets - they return to different places
        block->links.push_back({target, link_offset, false, false});
    }

    // Fallback RET (used when B is not yet linked to a target block)
    emit.RET();
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
    // Xbox 360 runs in 32-bit mode, so CTR is effectively 32-bit.
    // Use 32-bit instructions to ensure proper 32-bit wrap-around behavior.
    // Note: We pass X0 (register 0) but the _32 variants use W0 encoding.
    if (decrement_ctr && !is_ctr_target) {
        // Load CTR as 32-bit (lower half of 64-bit storage)
        emit.LDR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
        emit.SUB_imm_32(arm64::X0, arm64::X0, 1);
        // Store back as 32-bit (zero-extends to 64-bit in storage)
        emit.STR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
        
        // Test CTR (32-bit)
        u8* skip = emit.current();
        if (test_ctr_zero) { // Branch if CTR == 0
            emit.CBNZ_32(arm64::X0, 0);  // Skip to not-taken if CTR != 0
        } else { // Branch if CTR != 0
            emit.CBZ_32(arm64::X0, 0);   // Skip to not-taken if CTR == 0
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

    // Linkable exit for taken path (only for known constant targets, non-link branches)
    if (!is_lr_target && !is_ctr_target && block && !(inst.raw & 1)) {
        emit_block_epilogue_for_link(emit, current_block_inst_count_);
        u32 link_offset = static_cast<u32>(emit.size());
        emit.B(4);  // Default: skip to RET fallback
        block->links.push_back({target_taken, link_offset, false, true});
        emit.RET();
    } else {
        emit_block_epilogue(emit, current_block_inst_count_);
    }

    // ---- Not-taken path ----
    u8* not_taken_start = emit.current();

    // Patch all skip branches to jump here
    for (u8* skip : skip_branches) {
        s64 skip_offset = not_taken_start - skip;
        u32* patch_addr = reinterpret_cast<u32*>(skip);
        s32 imm19 = skip_offset >> 2;
        *patch_addr = (*patch_addr & 0xFF00001F) | ((imm19 & 0x7FFFF) << 5);
    }

    // Not-taken: continue to next instruction (linkable)
    emit.MOV_imm(arm64::X0, target_not_taken);
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());

    if (block) {
        emit_block_epilogue_for_link(emit, current_block_inst_count_);
        u32 link_offset = static_cast<u32>(emit.size());
        emit.B(4);  // Default: skip to RET fallback
        block->links.push_back({target_not_taken, link_offset, false, true});
        emit.RET();
    } else {
        emit_block_epilogue(emit, current_block_inst_count_);
    }
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
                emit.FNEG_vec(0, 0, true);
            }
            break;
        case 22: // fsqrt
            emit.FSQRT_vec(0, 1, true);
            break;
        case 24: // fres (reciprocal estimate)
            emit.FRECPE_vec(0, 1, true);
            break;
        case 26: // frsqrte (reciprocal square root estimate)
            emit.FRSQRTE_vec(0, 1, true);
            break;
        case 23: // fsel (float select)
        {
            // fsel frD, frA, frC, frB: if frA >= 0 then frD = frC else frD = frB
            load_fpr(emit, 2, (inst.raw >> 6) & 0x1F); // FRC
            // V0=frA, V1=frB, V2=frC
            // Use scalar FCMP D0, #0.0 then branch
            // FCMP Dn, #0.0 encoding: 0x1E602008 | (n << 5)
            emit.emit_raw(0x1E602008 | (0 << 5));
            // B.LT +8 (skip next MOV, go to frB path)
            u8* branch_lt = emit.current();
            emit.B_cond(arm64_cond::LT, 0);
            // frA >= 0: move frC to result
            // FMOV D0, D2 = ORR V0.8B, V2.8B, V2.8B
            emit.ORR_vec(0, 2, 2);
            u8* branch_done = emit.current();
            emit.B(0);
            // frA < 0: move frB to result
            emit.patch_branch(reinterpret_cast<u32*>(branch_lt), emit.current());
            emit.ORR_vec(0, 1, 1);
            emit.patch_branch(reinterpret_cast<u32*>(branch_done), emit.current());
            break;
        }
    }
    
    store_fpr(emit, inst.rd, 0);

    // Update FPSCR FPRF for arithmetic ops (not fsel which doesn't set FPRF)
    if (inst.xo != 23) {
        emit_update_fprf(emit, 0);
    }
}

//=============================================================================
// Vector Compilation (VMX128 -> NEON)
//=============================================================================

void JitCompiler::compile_vector(ARM64Emitter& emit, const DecodedInst& inst) {
    // Check for vector load/store ops (decoded as VLogical with opcode 31)
    if (inst.type == DecodedInst::Type::VLogical && inst.opcode == 31) {
        switch (inst.xo) {
            case 103: // lvx - Load Vector Indexed
            case 359: // lvxl - Load Vector Indexed LRU
            {
                calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
                // Align to 16 bytes (clear low 4 bits)
                emit.AND_imm(arm64::X0, arm64::X0, ~15ULL);
                // Mask to physical address
                emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
                emit.AND(arm64::X0, arm64::X0, arm64::X16);
                // Add fastmem base
                emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
                emit.ADD(arm64::X0, arm64::X0, arm64::X16);
                // Load 16 bytes into NEON register
                emit.LDR_vec(0, arm64::X0);
                // Byteswap each 32-bit element (big-endian to little-endian)
                // REV32 on vector: reverses bytes within each 32-bit element
                // Encoding: REV32 V0.16B, V0.16B = 0x6E200800 | (vn << 5) | vd
                emit.emit_raw(0x6E200800 | (0 << 5) | 0);
                store_vr(emit, inst.rd, 0);
                return;
            }
            case 231: // stvx - Store Vector Indexed
            case 487: // stvxl - Store Vector Indexed LRU
            {
                load_vr(emit, 0, inst.rd);  // Source vector
                // Byteswap each 32-bit element for big-endian storage
                emit.emit_raw(0x6E200800 | (0 << 5) | 0);  // REV32 V0.16B, V0.16B
                calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
                // Align to 16 bytes
                emit.AND_imm(arm64::X0, arm64::X0, ~15ULL);
                // Mask to physical address
                emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
                emit.AND(arm64::X0, arm64::X0, arm64::X16);
                // Add fastmem base
                emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
                emit.ADD(arm64::X0, arm64::X0, arm64::X16);
                // Store 16 bytes from NEON register
                emit.STR_vec(0, arm64::X0);
                return;
            }
            case 6:  // lvsl - Load Vector for Shift Left
            case 38: // lvsr - Load Vector for Shift Right
            {
                // These generate permute control vectors based on byte offset
                // Used with vperm to implement unaligned loads
                // For now, generate a sequential byte index vector
                calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
                emit.AND_imm(arm64::X0, arm64::X0, 0xF);  // Get byte offset
                // Simplified: store offset for use by vperm
                // Generate identity permute shifted by offset
                emit.MOV_imm(arm64::X1, 0x0706050403020100ULL);
                emit.MOV_imm(arm64::X2, 0x0F0E0D0C0B0A0908ULL);
                // Store into context VR
                emit.STR(arm64::X1, arm64::CTX_REG, ctx_offset_vr(inst.rd));
                emit.STR(arm64::X2, arm64::CTX_REG, ctx_offset_vr(inst.rd) + 8);
                return;
            }
            case 7:  // lvebx
            case 39: // lvehx
            case 71: // lvewx
            {
                // Load Vector Element Byte/Halfword/Word - load single element
                // Simplified: load the full 16 bytes from aligned address
                calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
                emit.MOV_imm(arm64::X16, 0x1FFFFFFFULL);
                emit.AND(arm64::X0, arm64::X0, arm64::X16);
                emit.MOV_imm(arm64::X16, reinterpret_cast<u64>(fastmem_base_));
                emit.ADD(arm64::X0, arm64::X0, arm64::X16);
                // Zero the target VR first, then load the element
                emit.EOR_vec(0, 0, 0);
                // Load single element (simplified - loads a word)
                emit.LDR(arm64::X1, arm64::X0, 0);
                byteswap32(emit, arm64::X1);
                emit.INS_general(0, 0, arm64::X1);
                store_vr(emit, inst.rd, 0);
                return;
            }
            default:
                break;
        }
    }

    // Standard vector arithmetic operations
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
        case DecodedInst::Type::VDiv:
            emit.FDIV_vec(0, 0, 1, false);
            break;
        case DecodedInst::Type::VLogical:
            // All opcode 4 VMX instructions decoded as VLogical
            // Dispatch based on sub-opcode fields
            {
                u32 xo_11 = inst.raw & 0x7FF;           // bits 21-31 (11-bit)
                u32 xo_6 = inst.raw & 0x3F;             // bits 26-31 (6-bit, VA-form)

                // Extract vC from VA-form (bits 21-25)
                u32 vc = (inst.raw >> 6) & 0x1F;

                // Check VA-form (6-bit xo) first for multiply-add ops
                switch (xo_6) {
                    case 46: // vmaddfp - vD = vA * vC + vB
                        load_vr(emit, 2, vc);
                        emit.FMLA_vec(1, 0, 2, false);  // vB += vA * vC
                        store_vr(emit, inst.rd, 1);
                        return;
                    case 47: // vnmsubfp - vD = -(vA * vC - vB) = vB - vA * vC
                        load_vr(emit, 2, vc);
                        emit.FMLS_vec(1, 0, 2, false);  // vB -= vA * vC
                        store_vr(emit, inst.rd, 1);
                        return;
                    default:
                        break;
                }

                // 11-bit xo dispatch
                switch (xo_11) {
                    // Float arithmetic
                    case 10: // vaddfp
                        emit.FADD_vec(0, 0, 1, false);
                        break;
                    case 74: // vsubfp
                        emit.FSUB_vec(0, 0, 1, false);
                        break;
                    case 1034: // vmaxfp
                        emit.FMAX_vec(0, 0, 1, false);
                        break;
                    case 1098: // vminfp
                        emit.FMIN_vec(0, 0, 1, false);
                        break;
                    case 266: // vrefp (reciprocal estimate)
                        emit.FRECPE_vec(0, 1, false);
                        break;
                    case 330: // vrsqrtefp (reciprocal square root estimate)
                        emit.FRSQRTE_vec(0, 1, false);
                        break;

                    // Integer arithmetic
                    case 0:    // vaddubm
                    case 64:   // vadduhm
                    case 128:  // vadduwm
                        emit.ADD_vec(0, 0, 1);
                        break;
                    case 1024: // vsububm
                    case 1088: // vsubuhm
                    case 1152: // vsubuwm
                        emit.SUB_vec(0, 0, 1);
                        break;

                    // Float compare
                    case 198: // vcmpeqfp
                        emit.FCMEQ_vec(0, 0, 1, false);
                        break;
                    case 454: // vcmpgefp
                        emit.FCMGE_vec(0, 0, 1, false);
                        break;
                    case 710: // vcmpgtfp
                        emit.FCMGT_vec(0, 0, 1, false);
                        break;

                    // Integer compare
                    case 134: // vcmpequw
                        emit.CMEQ_vec(0, 0, 1);
                        break;
                    case 646: // vcmpgtuw (unsigned)
                        emit.CMHI_vec(0, 0, 1);
                        break;
                    case 902: // vcmpgtsw (signed)
                        emit.CMGT_vec(0, 0, 1);
                        break;

                    // Logical
                    case 1028: // vand
                        emit.AND_vec(0, 0, 1);
                        break;
                    case 1092: // vandc
                        emit.BIC_vec(0, 0, 1);
                        break;
                    case 1156: // vor
                        emit.ORR_vec(0, 0, 1);
                        break;
                    case 1284: // vxor
                        emit.EOR_vec(0, 0, 1);
                        break;
                    case 1220: // vnor
                        emit.ORR_vec(0, 0, 1);
                        emit.NOT_vec(0, 0);
                        break;

                    // Merge
                    case 140: // vmrghw
                        emit.ZIP1(0, 0, 1);
                        break;
                    case 396: // vmrglw
                        emit.ZIP2(0, 0, 1);
                        break;

                    // Splat
                    case 588: // vspltw
                    {
                        u32 uimm = (inst.raw >> 16) & 0x1F;
                        emit.DUP_element(0, 1, uimm & 3);
                        break;
                    }

                    default:
                        // Fallback NOP for unhandled VMX sub-ops
                        emit.NOP();
                        break;
                }
            }
            break;
        default:
            emit.NOP();
            break;
    }

    store_vr(emit, inst.rd, 0);
}

//=============================================================================
// Vector Permute/Merge/Splat Compilation
//=============================================================================

void JitCompiler::compile_vector_permute(ARM64Emitter& emit, const DecodedInst& inst) {
    switch (inst.type) {
        case DecodedInst::Type::VPerm:
        {
            // vperm vD, vA, vB, vC - Permute bytes from vA/vB using vC as control
            // Load all three source vectors
            load_vr(emit, 0, inst.ra);  // vA
            load_vr(emit, 1, inst.rb);  // vB
            // vC is in the FRC field position
            int vc = (inst.raw >> 6) & 0x1F;
            load_vr(emit, 2, vc);       // vC (permute control)

            // ARM64 TBL instruction can permute from a table of 1-4 registers
            // For vperm, we'd need TBL with 2 source regs (vA, vB as table)
            // Simplified: use EXT as an approximation for common cases
            // Full vperm requires TBL2 which uses V0-V1 as table
            emit.EXT(0, 0, 1, 0);  // Simplified permute
            store_vr(emit, inst.rd, 0);
            break;
        }
        case DecodedInst::Type::VMerge:
        {
            // vmrghw/vmrglw - Merge high/low words from two vectors
            load_vr(emit, 0, inst.ra);
            load_vr(emit, 1, inst.rb);

            // Use ZIP for merge operations
            // vmrghw = interleave high elements, vmrglw = interleave low elements
            u32 sub_xo = (inst.raw >> 1) & 0x3FF;
            if (sub_xo == 12 || sub_xo == 268) {
                // vmrghw / vmrghh - merge high
                emit.ZIP1(0, 0, 1);
            } else {
                // vmrglw / vmrglh - merge low
                emit.ZIP2(0, 0, 1);
            }
            store_vr(emit, inst.rd, 0);
            break;
        }
        case DecodedInst::Type::VSplat:
        {
            // vspltw/vsplth/vspltb - Splat element across vector
            load_vr(emit, 0, inst.rb);
            // Splat element inst.ra across the vector
            int element = inst.ra & 0x3;
            emit.DUP_element(0, 0, element);
            store_vr(emit, inst.rd, 0);
            break;
        }
        default:
            emit.NOP();
            break;
    }
}

//=============================================================================
// Vector Compare Compilation
//=============================================================================

void JitCompiler::compile_vector_compare(ARM64Emitter& emit, const DecodedInst& inst) {
    // vcmpgtfp, vcmpeqfp, vcmpgefp, etc.
    load_vr(emit, 0, inst.ra);
    load_vr(emit, 1, inst.rb);

    // Determine compare type from xo field
    u32 xo = (inst.raw >> 1) & 0x3FF;
    switch (xo) {
        case 198: // vcmpgtfp (float greater than)
            emit.FCMGT_vec(0, 0, 1, false);
            break;
        case 70: // vcmpeqfp (float equal)
            emit.FCMEQ_vec(0, 0, 1, false);
            break;
        case 454: // vcmpgefp (float greater or equal)
            emit.FCMGE_vec(0, 0, 1, false);
            break;
        case 966: // vcmpgtuw (unsigned int greater than)
            emit.CMHI_vec(0, 0, 1, 2);
            break;
        case 518: // vcmpgtsh (signed half greater than)
            emit.CMGT_vec(0, 0, 1, 1);
            break;
        default:
            // Default to equality compare
            emit.CMEQ_vec(0, 0, 1, 2);
            break;
    }

    store_vr(emit, inst.rd, 0);

    // If Rc bit set, update CR6 with vector result
    if (inst.rc) {
        // Simplified: set CR6 based on whether all/none elements matched
        // Full implementation would reduce the vector comparison result
        emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(6) + 0);
        emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(6) + 1);
        emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(6) + 2);
        emit.STRB(arm64::XZR, arm64::CTX_REG, ctx_offset_cr(6) + 3);
    }
}

//=============================================================================
// Float Unary Compilation (fneg, fabs, fnabs, fmr)
//=============================================================================

void JitCompiler::compile_float_unary(ARM64Emitter& emit, const DecodedInst& inst) {
    // X-form float operations use 10-bit xo from bits 1-10
    u32 xo_x = (inst.raw >> 1) & 0x3FF;

    load_fpr(emit, 0, inst.rb);

    switch (xo_x) {
        case 40: // fneg
            emit.FNEG_vec(0, 0, true);
            break;
        case 264: // fabs
            emit.FABS_vec(0, 0, true);
            break;
        case 136: // fnabs - negate absolute
            emit.FABS_vec(0, 0, true);
            emit.FNEG_vec(0, 0, true);
            break;
        case 72: // fmr - float move register
            // Value already in register, just store
            break;
        default:
            // Unknown float unary, preserve value
            break;
    }

    store_fpr(emit, inst.rd, 0);
}

//=============================================================================
// Float Compare Compilation (fcmpu, fcmpo)
//=============================================================================

void JitCompiler::compile_float_compare(ARM64Emitter& emit, const DecodedInst& inst) {
    // fcmpu/fcmpo - Float Compare (Unordered/Ordered)
    int crfd = inst.crfd;
    size_t cr_offset = ctx_offset_cr(crfd);

    // Load FPR values into NEON regs then into GPRs for comparison
    load_fpr(emit, 0, inst.ra);
    load_fpr(emit, 1, inst.rb);

    // FCMP D0, D1
    emit.FCMP_vec(0, 0, 1, true);

    // Map ARM64 NZCV flags to PowerPC CR field
    // ARM64 FCMP: N=less, Z=equal, C=greater-or-equal-or-unordered, V=unordered
    // PowerPC: LT, GT, EQ, FU(unordered)

    // LT (negative flag - fra < frb)
    emit.CSET(arm64::X2, arm64_cond::MI);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset);

    // GT (fra > frb = carry set AND not equal AND not unordered)
    emit.CSET(arm64::X2, arm64_cond::GT);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 1);

    // EQ (equal)
    emit.CSET(arm64::X2, arm64_cond::EQ);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 2);

    // SO/FU (unordered - overflow flag set by FCMP for NaN)
    emit.CSET(arm64::X2, arm64_cond::VS);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 3);
}

//=============================================================================
// Float Convert Compilation (frsp, fctiw, fctiwz, fctid, fctidz, fcfid)
//=============================================================================

void JitCompiler::compile_float_convert(ARM64Emitter& emit, const DecodedInst& inst) {
    // X-form float operations use 10-bit xo from bits 1-10
    u32 xo_x = (inst.raw >> 1) & 0x3FF;

    // FPSCR access instructions don't follow normal FPR load/store pattern
    switch (xo_x) {
        case 583: // mffs
        case 711: // mtfsf
        case 70:  // mtfsb0
        case 38:  // mtfsb1
        case 64:  // mcrfs
            compile_fpscr_access(emit, inst);
            return;
        default:
            break;
    }

    load_fpr(emit, 0, inst.rb);

    switch (xo_x) {
        case 12: // frsp - Round to Single Precision
            // FCVT S0, D0 then FCVT D0, S0 (round through single)
            // Simplified: just store as-is since NEON handles precision
            break;

        case 14: // fctiw - Convert to Integer Word (round)
        case 15: // fctiwz - Convert to Integer Word, Round toward Zero
            emit.FCVTZS_vec(0, 0, true);
            break;

        case 814: // fctid - Convert to Integer Doubleword
        case 815: // fctidz - Convert to Integer Doubleword, Round toward Zero
            emit.FCVTZS_vec(0, 0, true);
            break;

        case 846: // fcfid - Convert From Integer Doubleword
            emit.SCVTF_vec(0, 0, true);
            break;

        default:
            break;
    }

    store_fpr(emit, inst.rd, 0);
}

//=============================================================================
// RFI - Return From Interrupt
//=============================================================================

void JitCompiler::compile_rfi(ARM64Emitter& emit, const DecodedInst& inst) {
    // RFI: Restore MSR from SRR1, set PC from SRR0
    // In an emulator, this signals return from exception handler

    // Set interrupted flag to let dispatcher handle MSR restoration
    emit.MOV_imm(arm64::X0, 1);
    emit.STRB(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, interrupted));

    // Load SRR0 (saved PC) and set as new PC
    // SRR0 is stored in context - use a fixed offset or load from SPR array
    // For simplicity, just set running=false to return to dispatcher
    emit.STRB(arm64::XZR, arm64::CTX_REG, offsetof(ThreadContext, running));

    emit_block_epilogue(emit, current_block_inst_count_);
}

//=============================================================================
// FPSCR Access (mffs, mtfsf, mtfsb0, mtfsb1, mcrfs)
//=============================================================================

void JitCompiler::compile_fpscr_access(ARM64Emitter& emit, const DecodedInst& inst) {
    u32 xo_x = (inst.raw >> 1) & 0x3FF;

    switch (xo_x) {
        case 583: // mffs - Move From FPSCR
        {
            // Load FPSCR into u64 and store to FPR[rd] (as raw bits in f64)
            emit.LDR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_fpscr());
            // Store as 64-bit to FPR (FPSCR goes in low 32 bits)
            emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_fpr(inst.rd));
            break;
        }
        case 711: // mtfsf - Move To FPSCR Fields
        {
            // FM field mask is bits 7-14 (8-bit)
            u32 fm = (inst.raw >> 17) & 0xFF;
            // Load source FPR as raw u64
            emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_fpr(inst.rb));
            // Load current FPSCR
            emit.LDR_u32(arm64::X1, arm64::CTX_REG, ctx_offset_fpscr());

            // For each FM bit set, copy 4-bit nibble from FPR value to FPSCR
            for (int i = 0; i < 8; i++) {
                if (fm & (0x80 >> i)) {
                    u32 mask = 0xF << (28 - i * 4);
                    // Clear bits in FPSCR
                    emit.MOV_imm(arm64::X2, ~mask);
                    emit.AND(arm64::X1, arm64::X1, arm64::X2);
                    // Extract bits from source
                    emit.MOV_imm(arm64::X2, mask);
                    emit.AND(arm64::X3, arm64::X0, arm64::X2);
                    // Merge
                    emit.ORR(arm64::X1, arm64::X1, arm64::X3);
                }
            }

            // Store updated FPSCR
            emit.STR_u32(arm64::X1, arm64::CTX_REG, ctx_offset_fpscr());

            // If RN bits (bits 0-1) were modified, sync ARM64 rounding mode
            if (fm & 0x01) {
                emit_sync_rounding_mode(emit);
            }
            break;
        }
        case 70: // mtfsb0 - Set FPSCR bit to 0
        {
            int bit = (inst.raw >> 21) & 0x1F;
            emit.LDR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_fpscr());
            emit.MOV_imm(arm64::X1, ~(1u << (31 - bit)));
            emit.AND(arm64::X0, arm64::X0, arm64::X1);
            emit.STR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_fpscr());
            // Sync rounding mode if RN bits changed (bits 30-31 in PPC numbering = bits 0-1)
            if (bit >= 30) emit_sync_rounding_mode(emit);
            break;
        }
        case 38: // mtfsb1 - Set FPSCR bit to 1
        {
            int bit = (inst.raw >> 21) & 0x1F;
            emit.LDR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_fpscr());
            emit.MOV_imm(arm64::X1, 1u << (31 - bit));
            emit.ORR(arm64::X0, arm64::X0, arm64::X1);
            emit.STR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_fpscr());
            if (bit >= 30) emit_sync_rounding_mode(emit);
            break;
        }
        case 64: // mcrfs - Move to CR from FPSCR
        {
            int crfd = (inst.raw >> 23) & 0x7;
            int crfs = (inst.raw >> 18) & 0x7;
            // Extract 4-bit field from FPSCR
            emit.LDR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_fpscr());
            int shift = 28 - crfs * 4;
            emit.LSR_imm(arm64::X0, arm64::X0, shift);
            emit.AND_imm(arm64::X0, arm64::X0, 0xF);
            // Split into CR sub-fields: LT(3), GT(2), EQ(1), SO(0)
            emit.LSR_imm(arm64::X1, arm64::X0, 3);
            emit.AND_imm(arm64::X1, arm64::X1, 1);
            emit.STRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(crfd) + 0);
            emit.LSR_imm(arm64::X1, arm64::X0, 2);
            emit.AND_imm(arm64::X1, arm64::X1, 1);
            emit.STRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(crfd) + 1);
            emit.LSR_imm(arm64::X1, arm64::X0, 1);
            emit.AND_imm(arm64::X1, arm64::X1, 1);
            emit.STRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(crfd) + 2);
            emit.AND_imm(arm64::X1, arm64::X0, 1);
            emit.STRB(arm64::X1, arm64::CTX_REG, ctx_offset_cr(crfd) + 3);
            break;
        }
        default:
            emit.NOP();
            break;
    }
}

//=============================================================================
// FPSCR FPRF Update - classify FP result after arithmetic ops
//=============================================================================

void JitCompiler::emit_update_fprf(ARM64Emitter& emit, int vreg) {
    // FPRF is FPSCR bits 12-16 (in PPC bit numbering = bits 15-19 in standard)
    // FPRF = C | FPCC(FL, FG, FE, FU)
    //   C=class bit, FL=less-than, FG=greater-than, FE=equal, FU=unordered/NaN
    //
    // Fast path classification of double in NEON vreg:
    //   Compare with zero using scalar FCMP, then read NZCV flags
    //   ARM64 FCMP: N=less, Z=equal, C=greater-or-unordered, V=unordered

    // FCMP Dn, #0.0 encoding: 0x1E602008 | (vn << 5)
    emit.emit_raw(0x1E602008 | (vreg << 5));

    // Build FPRF from condition flags
    // Start with 0, set bits based on conditions
    emit.MOV_imm(arm64::X0, 0);

    // FU (unordered/NaN) = VS condition
    emit.CSET(arm64::X1, arm64_cond::VS);
    emit.ORR(arm64::X0, arm64::X0, arm64::X1);  // bit 0 = FU

    // FE (equal) = EQ condition
    emit.CSET(arm64::X1, arm64_cond::EQ);
    emit.LSL_imm(arm64::X1, arm64::X1, 1);
    emit.ORR(arm64::X0, arm64::X0, arm64::X1);  // bit 1 = FE

    // FG (greater than) = GT condition
    emit.CSET(arm64::X1, arm64_cond::GT);
    emit.LSL_imm(arm64::X1, arm64::X1, 2);
    emit.ORR(arm64::X0, arm64::X0, arm64::X1);  // bit 2 = FG

    // FL (less than) = MI condition
    emit.CSET(arm64::X1, arm64_cond::MI);
    emit.LSL_imm(arm64::X1, arm64::X1, 3);
    emit.ORR(arm64::X0, arm64::X0, arm64::X1);  // bit 3 = FL

    // C (class) bit 4 - set for negative zero, denormals, infinity
    // Simplified: set C=0 for normal results (most common case)

    // Write FPRF to FPSCR bits 12-16
    // Load current FPSCR
    emit.LDR_u32(arm64::X2, arm64::CTX_REG, ctx_offset_fpscr());
    // Clear FPRF field (bits 12-16 = mask 0x1F000)
    emit.MOV_imm(arm64::X3, ~0x1F000ULL);
    emit.AND(arm64::X2, arm64::X2, arm64::X3);
    // Shift FPRF into position and merge
    emit.LSL_imm(arm64::X0, arm64::X0, 12);
    emit.ORR(arm64::X2, arm64::X2, arm64::X0);
    // Store updated FPSCR
    emit.STR_u32(arm64::X2, arm64::CTX_REG, ctx_offset_fpscr());
}

//=============================================================================
// Rounding Mode Sync - map PPC FPSCR.RN to ARM64 FPCR.RMode
//=============================================================================

void JitCompiler::emit_sync_rounding_mode(ARM64Emitter& emit) {
    // PPC FPSCR RN (bits 0-1): 0=nearest, 1=toward zero, 2=toward +inf, 3=toward -inf
    // ARM64 FPCR RMode (bits 22-23): 0=nearest, 1=toward +inf, 2=toward -inf, 3=toward zero
    // Mapping: PPC 0→ARM 0, PPC 1→ARM 3, PPC 2→ARM 1, PPC 3→ARM 2

    // Load FPSCR RN bits
    emit.LDR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_fpscr());
    emit.AND_imm(arm64::X0, arm64::X0, 3);  // RN = bits 0-1

    // Map PPC RN to ARM64 RMode using a small lookup
    // Build lookup: [0]=0, [1]=3, [2]=1, [3]=2
    // Use conditional moves for compact code
    emit.MOV_imm(arm64::X1, 0);  // default: nearest

    // if RN==1, rmode=3 (toward zero)
    emit.CMP_imm(arm64::X0, 1);
    emit.MOV_imm(arm64::X2, 3);
    emit.CSEL(arm64::X1, arm64::X2, arm64::X1, arm64_cond::EQ);

    // if RN==2, rmode=1 (toward +inf)
    emit.CMP_imm(arm64::X0, 2);
    emit.MOV_imm(arm64::X2, 1);
    emit.CSEL(arm64::X1, arm64::X2, arm64::X1, arm64_cond::EQ);

    // if RN==3, rmode=2 (toward -inf)
    emit.CMP_imm(arm64::X0, 3);
    emit.MOV_imm(arm64::X2, 2);
    emit.CSEL(arm64::X1, arm64::X2, arm64::X1, arm64_cond::EQ);

    // Read current FPCR
    // ARM64 FPCR sysreg encoding: op0=3,op1=3,CRn=4,CRm=4,op2=0 = 0xDA20
    emit.MRS(arm64::X0, 0xDA20);

    // Clear RMode bits (22-23) and set new value
    emit.MOV_imm(arm64::X2, ~(3ULL << 22));
    emit.AND(arm64::X0, arm64::X0, arm64::X2);
    emit.LSL_imm(arm64::X1, arm64::X1, 22);
    emit.ORR(arm64::X0, arm64::X0, arm64::X1);

    // Write back FPCR
    emit.MSR(0xDA20, arm64::X0);
}

//=============================================================================
// System Instruction Compilation
//=============================================================================

void JitCompiler::compile_syscall(ARM64Emitter& emit, const DecodedInst& inst) {
    // Set interrupted flag to signal syscall to dispatcher
    emit.MOV_imm(arm64::X0, 1);
    emit.STRB(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, interrupted));
    
    // CRITICAL FIX: Advance PC past the syscall instruction (4 bytes)
    // Without this, the game loops forever on the same syscall!
    emit.LDR(arm64::X1, arm64::CTX_REG, offsetof(ThreadContext, pc));
    emit.ADD_imm(arm64::X1, arm64::X1, 4);
    emit.STR(arm64::X1, arm64::CTX_REG, offsetof(ThreadContext, pc));
    
    // Return from block to handle syscall
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
            // Xbox 360 runs in 32-bit mode, so CTR is effectively 32-bit.
            // Store only the lower 32 bits to ensure proper wrap-around.
            emit.STR_u32(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
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
        int cached = reg_alloc_.get_cached_arm_reg(ppc_reg);
        if (cached != RegisterAllocator::INVALID_REG) {
            // Use cached register (MOV = 1 cycle vs LDR = 3-4 cycles)
            if (arm_reg != cached) {
                emit.ORR(arm_reg, arm64::XZR, cached);
            }
        } else {
            emit.LDR(arm_reg, arm64::CTX_REG, ctx_offset_gpr(ppc_reg));
        }
    }
}

void JitCompiler::store_gpr(ARM64Emitter& emit, int ppc_reg, int arm_reg) {
    // Note: r0 CAN be written to as a destination register in most instructions.
    // The "r0 = 0" special case only applies when r0 is used as a BASE register
    // for load/store address calculation (rA field), not when it's a destination (rD).
    int cached = reg_alloc_.get_cached_arm_reg(ppc_reg);
    if (cached != RegisterAllocator::INVALID_REG) {
        // Update cached register, defer ThreadContext write to block epilogue
        if (arm_reg != cached) {
            emit.ORR(cached, arm64::XZR, arm_reg);
        }
        reg_alloc_.mark_dirty(ppc_reg);
    } else {
        emit.STR(arm_reg, arm64::CTX_REG, ctx_offset_gpr(ppc_reg));
    }
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
    // Works for physical (0x0-0x1FFFFFFF) and usermode virtual (0x80000000-0x9FFFFFFF)
    // 
    // IMPORTANT: Kernel addresses (>= 0xA0000000) should NOT use this function!
    // They should be routed through the MMIO/slow path instead.
    // For legacy callers that still call this directly, we clamp to valid range.
    if (!fastmem_enabled_) return;
    
    // Clamp addresses to 512MB range to avoid accessing unmapped memory
    // This is a safety check - kernel addresses should be caught earlier
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
    emit.STP(arm64::X23, arm64::X24, arm64::SP, -64);
    emit.SUB_imm(arm64::SP, arm64::SP, 64);

    // Set up context register (X19)
    emit.ORR(arm64::CTX_REG, arm64::XZR, arm64::X0);

    // Load cached PPC GPRs into X21-X24
    for (int i = 0; i < RegisterAllocator::MAX_CACHED_GPRS; i++) {
        int ppc_reg = reg_alloc_.cached_ppc_reg(i);
        if (ppc_reg > 0) {
            emit.LDR(RegisterAllocator::CACHE_REGS[i], arm64::CTX_REG, ctx_offset_gpr(ppc_reg));
        }
    }
}

void JitCompiler::emit_block_epilogue_for_link(ARM64Emitter& emit, u32 inst_count) {
    // Increment time base register by actual cycles executed
    // ~4 cycles per instruction to approximate Xbox 360's ~50MHz time base
    u32 cycles = inst_count * 4;
    emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_time_base());
    emit.ADD_imm(arm64::X0, arm64::X0, cycles);
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_time_base());

    // Flush dirty cached GPRs back to ThreadContext
    for (int i = 0; i < RegisterAllocator::MAX_CACHED_GPRS; i++) {
        int ppc_reg = reg_alloc_.cached_ppc_reg(i);
        if (ppc_reg > 0 && reg_alloc_.is_dirty(ppc_reg)) {
            emit.STR(RegisterAllocator::CACHE_REGS[i], arm64::CTX_REG, ctx_offset_gpr(ppc_reg));
        }
    }

    // Restore callee-saved registers
    emit.ADD_imm(arm64::SP, arm64::SP, 64);
    emit.LDP(arm64::X23, arm64::X24, arm64::SP, -64);
    emit.LDP(arm64::X21, arm64::X22, arm64::SP, -48);
    emit.LDP(arm64::X19, arm64::X20, arm64::SP, -32);
    emit.LDP(arm64::X29, arm64::X30, arm64::SP, -16);
    // No RET - caller emits B for block linking or RET for non-linkable exits
}

void JitCompiler::emit_block_epilogue(ARM64Emitter& emit, u32 inst_count) {
    emit_block_epilogue_for_link(emit, inst_count);
    emit.RET();
}

//=============================================================================
// Block Linking
//=============================================================================

void JitCompiler::try_link_block(CompiledBlock* block) {
    // Link this block's exits to already-compiled target blocks
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
                stats_.blocks_linked++;

#ifdef __aarch64__
                __builtin___clear_cache(
                    reinterpret_cast<char*>(patch_addr),
                    reinterpret_cast<char*>(patch_addr) + 4
                );
#endif
            }
        }
    }

    // Link other blocks' exits to this newly-compiled block
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
                stats_.blocks_linked++;

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
// Idle Loop Detection
//=============================================================================

bool JitCompiler::detect_idle_loop(GuestAddr addr, u32 inst_count) {
    // Idle loops are small blocks (2-8 instructions) that branch back to themselves
    // and only contain loads, compares, and NOPs (no stores or side effects).
    // Detecting these allows us to skip ahead and yield CPU time.
    if (inst_count < 2 || inst_count > 8) return false;

    // Check last instruction - must be a conditional branch back to block start
    u32 last_raw = memory_->read_u32(addr + (inst_count - 1) * 4);
    u32 last_opcode = last_raw >> 26;

    if (last_opcode != 16) return false;  // Must be bc (opcode 16)

    // Calculate branch target
    GuestAddr branch_pc = addr + (inst_count - 1) * 4;
    s32 bd = static_cast<s16>(last_raw & 0xFFFC);  // Sign-extend BD field
    GuestAddr target;
    if (last_raw & 2) {  // AA (absolute)
        target = bd & ~3;
    } else {
        target = branch_pc + bd;
    }

    if (target != addr) return false;  // Must loop back to start

    // Check that all instructions (except last branch) are side-effect-free:
    // Allow: loads (lwz, lbz, lhz, lha), compares (cmpi, cmpli, cmp, cmpl), NOPs
    for (u32 i = 0; i < inst_count - 1; i++) {
        u32 raw = memory_->read_u32(addr + i * 4);
        u32 opcode = raw >> 26;

        switch (opcode) {
            case 32: case 34: case 40: case 42:  // lwz, lbz, lhz, lha
                break;  // Safe: read-only
            case 11: case 10:  // cmpi, cmpli
                break;  // Safe: only sets CR
            case 24:  // ori - check if NOP (ori 0,0,0)
                if ((raw & 0x03FFFFFF) != 0) return false;
                break;
            case 31: {  // Extended ops
                u32 xo = (raw >> 1) & 0x3FF;
                switch (xo) {
                    case 0: case 32:    // cmp, cmpl
                    case 23: case 87:   // lwzx, lbzx
                    case 279: case 343: // lhzx, lhax
                        break;  // Safe
                    default:
                        return false;  // Unknown extended op - not safe
                }
                break;
            }
            default:
                return false;  // Unknown opcode - not safe
        }
    }

    return true;
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
    block->linked_entry_offset = 0;
    block->is_idle_loop = false;

    // Create temporary buffer for code generation
    u8 temp_buffer[TEMP_BUFFER_SIZE];
    ARM64Emitter emit(temp_buffer, TEMP_BUFFER_SIZE);

    ThreadContext ctx_template = {};

    // Pre-scan block to determine size and set up register allocation
    {
        GuestAddr scan_pc = addr;
        u32 pre_scan_count = 0;
        bool scan_ended = false;
        while (!scan_ended && pre_scan_count < MAX_BLOCK_INSTRUCTIONS) {
            u32 raw = memory_->read_u32(scan_pc);
            DecodedInst d = Decoder::decode(raw);
            d.raw = raw;
            pre_scan_count++;
            scan_pc += 4;
            if (is_block_ending(d)) scan_ended = true;
        }

        // Analyze GPR usage and map hot registers to X21-X24
        reg_alloc_.setup_block(addr, pre_scan_count, memory_);

        // Detect idle loops (small loops that just spin on a condition)
        block->is_idle_loop = detect_idle_loop(addr, pre_scan_count);
        if (block->is_idle_loop) {
            stats_.idle_loops_detected++;
            LOGI("Idle loop detected at %08llX (%u instructions)", (unsigned long long)addr, pre_scan_count);
        }
    }

    GuestAddr pc = addr;
    u32 inst_count = 0;
    bool block_ended = false;

    // Reset instruction count for time_base tracking
    current_block_inst_count_ = 0;

    // Emit block prologue
    emit_block_prologue(emit);

    // Record entry point past prologue for linked block entry
    block->linked_entry_offset = static_cast<u32>(emit.size());

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
