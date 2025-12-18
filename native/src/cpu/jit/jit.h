/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * JIT Compiler - PowerPC to ARM64 Dynamic Recompiler
 * 
 * This is the heart of the emulator's performance. It translates PowerPC
 * instructions to native ARM64 code at runtime for near-native execution speed.
 */

#pragma once

#include "x360mu/types.h"
#include "../xenon/cpu.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <bitset>
#include <array>

#ifdef __aarch64__
#include <sys/mman.h>
#endif

namespace x360mu {

class Memory;

/**
 * ARM64 register allocation
 */
namespace arm64 {
    // Caller-saved registers (can use freely)
    constexpr int X0 = 0;   // Return value, temp
    constexpr int X1 = 1;   // Temp
    constexpr int X2 = 2;   // Temp
    constexpr int X3 = 3;   // Temp
    constexpr int X4 = 4;   // Temp
    constexpr int X5 = 5;   // Temp
    constexpr int X6 = 6;   // Temp
    constexpr int X7 = 7;   // Temp
    constexpr int X8 = 8;   // Indirect result location
    constexpr int X9 = 9;   // Temp
    constexpr int X10 = 10; // Temp
    constexpr int X11 = 11; // Temp
    constexpr int X12 = 12; // Temp
    constexpr int X13 = 13; // Temp
    constexpr int X14 = 14; // Temp
    constexpr int X15 = 15; // Temp
    constexpr int X16 = 16; // IP0 - scratch
    constexpr int X17 = 17; // IP1 - scratch
    constexpr int X18 = 18; // Platform register (avoid)
    
    // Callee-saved registers (must preserve)
    constexpr int X19 = 19; // PPC context pointer
    constexpr int X20 = 20; // Memory base pointer (fastmem)
    constexpr int X21 = 21; // PPC GPR cache 0
    constexpr int X22 = 22; // PPC GPR cache 1
    constexpr int X23 = 23; // PPC GPR cache 2
    constexpr int X24 = 24; // PPC GPR cache 3
    constexpr int X25 = 25; // PPC LR cache
    constexpr int X26 = 26; // PPC CTR cache
    constexpr int X27 = 27; // JIT compiler pointer
    constexpr int X28 = 28; // Cycle counter
    constexpr int X29 = 29; // Frame pointer
    constexpr int X30 = 30; // Link register
    constexpr int SP = 31;  // Stack pointer / Zero register
    constexpr int XZR = 31; // Zero register (same encoding as SP)
    
    // NEON registers for FPU and VMX128 emulation
    constexpr int V0 = 0;
    constexpr int V1 = 1;
    constexpr int V2 = 2;
    constexpr int V3 = 3;
    constexpr int V4 = 4;
    constexpr int V5 = 5;
    constexpr int V6 = 6;
    constexpr int V7 = 7;
    // V8-V15 are callee-saved (lower 64 bits only)
    // V16-V31 are caller-saved
    
    // Context register - always points to ThreadContext
    constexpr int CTX_REG = X19;
    // Memory base register for fastmem
    constexpr int MEM_BASE = X20;
    // JIT compiler pointer
    constexpr int JIT_REG = X27;
    // Cycle counter
    constexpr int CYCLES_REG = X28;
}

/**
 * Register allocator for PPC to ARM64 mapping
 */
class RegisterAllocator {
public:
    RegisterAllocator();
    
    // Get ARM64 register for PPC GPR
    // Returns a register (may need to load from context if not cached)
    int get_gpr(int ppc_reg);
    
    // Mark a PPC GPR as dirty (needs writeback)
    void mark_dirty(int ppc_reg);
    
    // Flush all dirty registers to context
    void flush_all(class ARM64Emitter& emit);
    
    // Flush specific register
    void flush_gpr(class ARM64Emitter& emit, int ppc_reg);
    
    // Allocate a temporary ARM64 register
    int alloc_temp();
    
    // Free a temporary register
    void free_temp(int arm_reg);
    
    // Reset allocator state
    void reset();
    
    // Check if a PPC register is currently cached in an ARM64 register
    bool is_cached(int ppc_reg) const;
    
    // Direct register mapping (simple approach for initial implementation)
    // PPC GPR -> ARM64 register or -1 if not mapped
    static constexpr int INVALID_REG = -1;
    
private:
    // Which PPC GPRs are cached in ARM64 registers
    std::array<int, 32> ppc_to_arm_;  // -1 if not cached
    
    // Which ARM64 registers hold PPC GPRs
    std::array<int, 32> arm_to_ppc_;  // -1 if not holding a PPC reg
    
    // Which cached registers are dirty
    std::bitset<32> dirty_;
    
    // Available temp registers
    std::bitset<18> temp_available_;  // X0-X17 availability
};

/**
 * Compiled code block
 */
struct CompiledBlock {
    GuestAddr start_addr;           // PPC start address
    GuestAddr end_addr;             // PPC end address (exclusive)
    u32 size;                       // Number of PPC instructions
    void* code;                     // Pointer to compiled ARM64 code
    u32 code_size;                  // Size of ARM64 code in bytes
    u64 hash;                       // Hash of original PPC code for SMC detection
    u32 execution_count;            // For hot block tracking
    std::vector<GuestAddr> exits;   // Block exit addresses
    
    // Linking info for direct jumps
    struct Link {
        GuestAddr target;           // PPC target address
        u32 patch_offset;           // Offset in ARM64 code to patch
        bool linked;                // Has been linked?
        bool is_conditional;        // Is this a conditional branch?
    };
    std::vector<Link> links;
    
    // Block cache management (used by BlockCache)
    CompiledBlock* hash_next = nullptr;  // Hash chain
    CompiledBlock* hash_prev = nullptr;
    CompiledBlock* lru_next = nullptr;   // LRU list
    CompiledBlock* lru_prev = nullptr;
    
    // Check if this block contains the given address
    bool contains(GuestAddr addr) const {
        return addr >= start_addr && addr < end_addr;
    }
};

/**
 * ARM64 code emitter
 */
class ARM64Emitter {
public:
    ARM64Emitter(u8* buffer, size_t capacity);
    
    // Current write position
    u8* current() { return current_; }
    size_t size() const { return current_ - buffer_; }
    
    // Data processing - immediate
    void ADD_imm(int rd, int rn, u32 imm12, bool shift = false);
    void ADDS_imm(int rd, int rn, u32 imm12, bool shift = false);
    void SUB_imm(int rd, int rn, u32 imm12, bool shift = false);
    void SUBS_imm(int rd, int rn, u32 imm12, bool shift = false);
    void CMP_imm(int rn, u32 imm12);
    void CMN_imm(int rn, u32 imm12);
    void MOV_imm(int rd, u64 imm);
    void MOVZ(int rd, u16 imm, int shift = 0);
    void MOVK(int rd, u16 imm, int shift = 0);
    void MOVN(int rd, u16 imm, int shift = 0);
    
    // Data processing - register
    void ADD(int rd, int rn, int rm, int shift = 0, int amount = 0);
    void ADDS(int rd, int rn, int rm);
    void SUB(int rd, int rn, int rm, int shift = 0, int amount = 0);
    void SUBS(int rd, int rn, int rm);
    void ADC(int rd, int rn, int rm);
    void ADCS(int rd, int rn, int rm);
    void SBC(int rd, int rn, int rm);
    void SBCS(int rd, int rn, int rm);
    void NEG(int rd, int rm);
    void CMP(int rn, int rm);
    void CMN(int rn, int rm);
    
    // Logical
    void AND(int rd, int rn, int rm);
    void ANDS(int rd, int rn, int rm);
    void ORR(int rd, int rn, int rm);
    void ORN(int rd, int rn, int rm);
    void EOR(int rd, int rn, int rm);
    void EON(int rd, int rn, int rm);
    void BIC(int rd, int rn, int rm);
    void BICS(int rd, int rn, int rm);
    void TST(int rn, int rm);
    void AND_imm(int rd, int rn, u64 imm);
    void ORR_imm(int rd, int rn, u64 imm);
    void EOR_imm(int rd, int rn, u64 imm);
    
    // Shifts
    void LSL(int rd, int rn, int rm);
    void LSL_imm(int rd, int rn, int shift);
    void LSR(int rd, int rn, int rm);
    void LSR_imm(int rd, int rn, int shift);
    void ASR(int rd, int rn, int rm);
    void ASR_imm(int rd, int rn, int shift);
    void ROR(int rd, int rn, int rm);
    void ROR_imm(int rd, int rn, int shift);
    
    // Multiply
    void MUL(int rd, int rn, int rm);
    void MADD(int rd, int rn, int rm, int ra);
    void MSUB(int rd, int rn, int rm, int ra);
    void SMULL(int rd, int rn, int rm);
    void UMULL(int rd, int rn, int rm);
    void SMULH(int rd, int rn, int rm);
    void UMULH(int rd, int rn, int rm);
    
    // Divide
    void SDIV(int rd, int rn, int rm);
    void UDIV(int rd, int rn, int rm);
    
    // Bit manipulation
    void CLZ(int rd, int rn);
    void CLS(int rd, int rn);
    void RBIT(int rd, int rn);
    void REV(int rd, int rn);
    void REV16(int rd, int rn);
    void REV32(int rd, int rn);
    
    // Extension
    void SXTB(int rd, int rn);
    void SXTH(int rd, int rn);
    void SXTW(int rd, int rn);
    void UXTB(int rd, int rn);
    void UXTH(int rd, int rn);
    void UXTW(int rd, int rn);
    
    // Conditional select
    void CSEL(int rd, int rn, int rm, int cond);
    void CSINC(int rd, int rn, int rm, int cond);
    void CSINV(int rd, int rn, int rm, int cond);
    void CSNEG(int rd, int rn, int rm, int cond);
    void CSET(int rd, int cond);
    void CSETM(int rd, int cond);
    
    // Load/Store
    void LDR(int rt, int rn, s32 offset = 0);
    void LDRB(int rt, int rn, s32 offset = 0);
    void LDRH(int rt, int rn, s32 offset = 0);
    void LDRSB(int rt, int rn, s32 offset = 0);
    void LDRSH(int rt, int rn, s32 offset = 0);
    void LDRSW(int rt, int rn, s32 offset = 0);
    void LDR_reg(int rt, int rn, int rm, int extend = 0, bool shift = false);
    void LDP(int rt1, int rt2, int rn, s32 offset = 0);
    
    void STR(int rt, int rn, s32 offset = 0);
    void STRB(int rt, int rn, s32 offset = 0);
    void STRH(int rt, int rn, s32 offset = 0);
    void STR_reg(int rt, int rn, int rm, int extend = 0, bool shift = false);
    void STP(int rt1, int rt2, int rn, s32 offset = 0);
    
    // Load/Store pre/post-index
    void LDR_pre(int rt, int rn, s32 offset);
    void LDR_post(int rt, int rn, s32 offset);
    void STR_pre(int rt, int rn, s32 offset);
    void STR_post(int rt, int rn, s32 offset);
    
    // Branch
    void B(s32 offset);
    void B_cond(int cond, s32 offset);
    void BL(s32 offset);
    void BR(int rn);
    void BLR(int rn);
    void RET(int rn = arm64::X30);
    void CBZ(int rt, s32 offset);
    void CBNZ(int rt, s32 offset);
    void TBZ(int rt, int bit, s32 offset);
    void TBNZ(int rt, int bit, s32 offset);
    
    // System
    void NOP();
    void BRK(u16 imm = 0);
    void DMB(int option = 15);
    void DSB(int option = 15);
    void ISB();
    void MRS(int rt, u32 sysreg);
    void MSR(u32 sysreg, int rt);
    
    // NEON - basic
    void FADD_vec(int vd, int vn, int vm, bool is_double = false);
    void FSUB_vec(int vd, int vn, int vm, bool is_double = false);
    void FMUL_vec(int vd, int vn, int vm, bool is_double = false);
    void FDIV_vec(int vd, int vn, int vm, bool is_double = false);
    void FMADD_vec(int vd, int vn, int vm, int va, bool is_double = false);
    void FNEG_vec(int vd, int vn, bool is_double = false);
    void FABS_vec(int vd, int vn, bool is_double = false);
    void FCMP_vec(int vd, int vn, int vm, bool is_double = false);
    
    // NEON - load/store
    void LDR_vec(int vt, int rn, s32 offset = 0);
    void STR_vec(int vt, int rn, s32 offset = 0);
    void LD1(int vt, int rn, int lane = -1);
    void ST1(int vt, int rn, int lane = -1);
    
    // NEON - permute
    void DUP_element(int vd, int vn, int index);
    void DUP_general(int vd, int rn);
    void INS_element(int vd, int index, int vn, int src_index);
    void INS_general(int vd, int index, int rn);
    void EXT(int vd, int vn, int vm, int index);
    void TRN1(int vd, int vn, int vm);
    void TRN2(int vd, int vn, int vm);
    void ZIP1(int vd, int vn, int vm);
    void ZIP2(int vd, int vn, int vm);
    void UZP1(int vd, int vn, int vm);
    void UZP2(int vd, int vn, int vm);
    
    // NEON - integer vector
    void ADD_vec(int vd, int vn, int vm, int size = 2);
    void SUB_vec(int vd, int vn, int vm, int size = 2);
    void AND_vec(int vd, int vn, int vm);
    void ORR_vec(int vd, int vn, int vm);
    void EOR_vec(int vd, int vn, int vm);
    void BIC_vec(int vd, int vn, int vm);
    void NOT_vec(int vd, int vn);
    
    // NEON - comparison
    void CMEQ_vec(int vd, int vn, int vm, int size = 2);
    void CMGT_vec(int vd, int vn, int vm, int size = 2);
    void CMGE_vec(int vd, int vn, int vm, int size = 2);
    
    // NEON - min/max
    void FMAX_vec(int vd, int vn, int vm, bool is_double = false);
    void FMIN_vec(int vd, int vn, int vm, bool is_double = false);
    
    // NEON - reciprocal/sqrt
    void FRECPE_vec(int vd, int vn, bool is_double = false);
    void FRSQRTE_vec(int vd, int vn, bool is_double = false);
    void FSQRT_vec(int vd, int vn, bool is_double = false);
    
    // NEON - convert
    void FCVTZS_vec(int vd, int vn, bool is_double = false);
    void FCVTZU_vec(int vd, int vn, bool is_double = false);
    void SCVTF_vec(int vd, int vn, bool is_double = false);
    void UCVTF_vec(int vd, int vn, bool is_double = false);
    
    // 32-bit mode operations
    void ADD_32(int rd, int rn, int rm);
    void SUB_32(int rd, int rn, int rm);
    void MUL_32(int rd, int rn, int rm);
    void SDIV_32(int rd, int rn, int rm);
    void UDIV_32(int rd, int rn, int rm);
    void LSL_32(int rd, int rn, int rm);
    void LSR_32(int rd, int rn, int rm);
    void ASR_32(int rd, int rn, int rm);
    void ROR_32(int rd, int rn, int rm);
    
    // Address manipulation
    void ADR(int rd, s32 offset);
    void ADRP(int rd, s64 offset);
    
    // Patching
    void patch_branch(u32* patch_site, void* target);
    void patch_imm(u32* patch_site, u64 imm);
    
    // Labels and fixups
    u32* label_here() { return reinterpret_cast<u32*>(current_); }
    void bind_label(u32* label, u32* target);
    
private:
    u8* buffer_;
    u8* current_;
    size_t capacity_;
    
    void emit32(u32 value);
    
    // Encoding helpers
    u32 encode_logical_imm(u64 imm, bool is_64bit);
    bool is_valid_logical_imm(u64 imm, bool is_64bit);
};

/**
 * JIT Compiler
 */
class JitCompiler {
public:
    JitCompiler();
    ~JitCompiler();
    
    /**
     * Initialize the JIT compiler
     */
    Status initialize(Memory* memory, u64 cache_size);
    
    /**
     * Shutdown and release resources
     */
    void shutdown();
    
    /**
     * Execute a thread starting from its current PC
     * Runs until cycles exhausted or interrupted
     * @param ctx Thread context to execute
     * @param cycles Maximum cycles to execute
     * @return Number of cycles actually executed
     */
    u64 execute(ThreadContext& ctx, u64 cycles);
    
    /**
     * Invalidate code at address (called when game writes to code)
     */
    void invalidate(GuestAddr addr, u32 size);
    
    /**
     * Get cache statistics
     */
    struct Stats {
        u64 blocks_compiled;
        u64 code_bytes_used;
        u64 cache_hits;
        u64 cache_misses;
        u64 instructions_executed;
        u64 interpreter_fallbacks;
    };
    Stats get_stats() const { return stats_; }
    
    /**
     * Flush entire cache
     */
    void flush_cache();
    
    /**
     * Look up block for dispatch (called from JIT code)
     */
    void* lookup_block_for_dispatch(GuestAddr pc);
    
    /**
     * Get memory pointer for fastmem (called from JIT code)
     */
    u8* get_memory_base() const;
    
private:
    // Compile without locking (lock must be held)
    CompiledBlock* compile_block_unlocked(GuestAddr addr);
    Memory* memory_ = nullptr;
    
    // Code cache - executable memory region
    u8* code_cache_ = nullptr;
    u8* code_write_ptr_ = nullptr;
    size_t cache_size_ = 0;
    
    // Block lookup (PPC address -> compiled block)
    std::unordered_map<GuestAddr, CompiledBlock*> block_map_;
    std::mutex block_map_mutex_;
    
    // Fastmem base pointer (points to guest memory region)
    u8* fastmem_base_ = nullptr;
    bool fastmem_enabled_ = false;
    
    // Statistics
    Stats stats_ = {};
    
    // Register allocator
    RegisterAllocator reg_alloc_;
    
    // Current instruction count during block compilation (for time_base tracking)
    u32 current_block_inst_count_ = 0;
    
    // Compile a single block
    CompiledBlock* compile_block(GuestAddr addr);
    
    // Block compilation
    void compile_instruction(ARM64Emitter& emit, ThreadContext& ctx_template, 
                             const DecodedInst& inst, GuestAddr pc);
    
    // Check if instruction ends the block
    bool is_block_ending(const DecodedInst& inst) const;
    
    // Integer instruction compilation
    void compile_add(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_sub(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_mul(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_div(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_logical(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_shift(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_rotate(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_compare(ARM64Emitter& emit, const DecodedInst& inst);
    
    // Load/Store compilation
    void compile_load(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_store(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_load_multiple(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_store_multiple(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_atomic_load(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_atomic_store(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_dcbz(ARM64Emitter& emit, const DecodedInst& inst);
    
    // Additional instruction compilation
    void compile_extsb(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_extsh(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_extsw(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_cntlzw(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_cntlzd(ARM64Emitter& emit, const DecodedInst& inst);
    
    // Branch compilation
    void compile_branch(ARM64Emitter& emit, const DecodedInst& inst, GuestAddr pc, 
                       CompiledBlock* block);
    void compile_branch_conditional(ARM64Emitter& emit, const DecodedInst& inst, 
                                   GuestAddr pc, CompiledBlock* block);
    void compile_branch_to_lr(ARM64Emitter& emit, const DecodedInst& inst, CompiledBlock* block);
    void compile_branch_to_ctr(ARM64Emitter& emit, const DecodedInst& inst, CompiledBlock* block);
    
    // Float compilation
    void compile_float(ARM64Emitter& emit, const DecodedInst& inst);
    
    // Vector (VMX128) compilation - uses NEON
    void compile_vector(ARM64Emitter& emit, const DecodedInst& inst);
    
    // System instruction compilation
    void compile_system(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_syscall(ARM64Emitter& emit, const DecodedInst& inst);
    
    // CR operations
    void compile_cr_update(ARM64Emitter& emit, int field, int result_reg);
    void compile_cr_logical(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_mtcrf(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_mfcr(ARM64Emitter& emit, const DecodedInst& inst);
    
    // SPR operations  
    void compile_mfspr(ARM64Emitter& emit, const DecodedInst& inst);
    void compile_mtspr(ARM64Emitter& emit, const DecodedInst& inst);
    
    // Helper: Load PPC GPR into ARM64 register
    void load_gpr(ARM64Emitter& emit, int arm_reg, int ppc_reg);
    
    // Helper: Store ARM64 register to PPC GPR
    void store_gpr(ARM64Emitter& emit, int ppc_reg, int arm_reg);
    
    // Helper: Load PPC FPR into NEON register
    void load_fpr(ARM64Emitter& emit, int neon_reg, int ppc_reg);
    
    // Helper: Store NEON register to PPC FPR
    void store_fpr(ARM64Emitter& emit, int ppc_reg, int neon_reg);
    
    // Helper: Load PPC VR into NEON register
    void load_vr(ARM64Emitter& emit, int neon_reg, int ppc_reg);
    
    // Helper: Store NEON register to PPC VR
    void store_vr(ARM64Emitter& emit, int ppc_reg, int neon_reg);
    
    // Helper: Calculate effective address
    void calc_ea(ARM64Emitter& emit, int dest_reg, int ra, s16 offset);
    void calc_ea_indexed(ARM64Emitter& emit, int dest_reg, int ra, int rb);
    
    // Helper: Translate virtual address to physical and add fastmem base
    void emit_translate_address(ARM64Emitter& emit, int addr_reg);
    
    // Helper: Memory byte swap (big-endian to little-endian)
    void byteswap32(ARM64Emitter& emit, int reg);
    void byteswap16(ARM64Emitter& emit, int reg);
    void byteswap64(ARM64Emitter& emit, int reg);
    
    // Block prologue/epilogue
    void emit_block_prologue(ARM64Emitter& emit);
    void emit_block_epilogue(ARM64Emitter& emit, u32 inst_count);
    
    // Block linking
    void try_link_block(CompiledBlock* block);
    void unlink_block(CompiledBlock* block);
    
    // Dispatcher
    using DispatcherFunc = void(*)(ThreadContext* ctx, void* jit);
    DispatcherFunc dispatcher_ = nullptr;
    void generate_dispatcher();
    
    // Exit stub (return to dispatcher)
    void* exit_stub_ = nullptr;
    void generate_exit_stub();
    
    // Helper functions called from JIT code
    static void helper_syscall(ThreadContext* ctx, JitCompiler* jit);
    static void helper_read_u8(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u8* result);
    static void helper_read_u16(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u16* result);
    static void helper_read_u32(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u32* result);
    static void helper_read_u64(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u64* result);
    static void helper_write_u8(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u8 value);
    static void helper_write_u16(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u16 value);
    static void helper_write_u32(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u32 value);
    static void helper_write_u64(ThreadContext* ctx, JitCompiler* jit, GuestAddr addr, u64 value);
    
    // Context offset helpers
    static constexpr size_t ctx_offset_gpr(int reg) {
        return offsetof(ThreadContext, gpr) + reg * sizeof(u64);
    }
    static constexpr size_t ctx_offset_fpr(int reg) {
        return offsetof(ThreadContext, fpr) + reg * sizeof(f64);
    }
    static constexpr size_t ctx_offset_vr(int reg) {
        return offsetof(ThreadContext, vr) + reg * sizeof(VectorReg);
    }
    static constexpr size_t ctx_offset_lr() {
        return offsetof(ThreadContext, lr);
    }
    static constexpr size_t ctx_offset_ctr() {
        return offsetof(ThreadContext, ctr);
    }
    static constexpr size_t ctx_offset_pc() {
        return offsetof(ThreadContext, pc);
    }
    static constexpr size_t ctx_offset_cr(int field) {
        return offsetof(ThreadContext, cr) + field * sizeof(CRField);
    }
    static constexpr size_t ctx_offset_xer() {
        return offsetof(ThreadContext, xer);
    }
    static constexpr size_t ctx_offset_time_base() {
        return offsetof(ThreadContext, time_base);
    }
};

// ARM64 condition codes
namespace arm64_cond {
    constexpr int EQ = 0;   // Equal
    constexpr int NE = 1;   // Not equal
    constexpr int CS = 2;   // Carry set / unsigned higher or same
    constexpr int CC = 3;   // Carry clear / unsigned lower
    constexpr int MI = 4;   // Minus / negative
    constexpr int PL = 5;   // Plus / positive or zero
    constexpr int VS = 6;   // Overflow
    constexpr int VC = 7;   // No overflow
    constexpr int HI = 8;   // Unsigned higher
    constexpr int LS = 9;   // Unsigned lower or same
    constexpr int GE = 10;  // Signed greater than or equal
    constexpr int LT = 11;  // Signed less than
    constexpr int GT = 12;  // Signed greater than
    constexpr int LE = 13;  // Signed less than or equal
    constexpr int AL = 14;  // Always
    constexpr int NV = 15;  // Never (used as unconditional)
}

} // namespace x360mu

