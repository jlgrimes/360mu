/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * PowerPC Xenon CPU emulation
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <array>
#include <string>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace x360mu {

class Memory;
class JitCompiler;

/**
 * CPU configuration
 */
struct CpuConfig {
    bool enable_jit = true;
    u64 jit_cache_size = 128 * MB;
    bool enable_tracing = false;
};

/**
 * PowerPC Condition Register (CR) field
 */
struct CRField {
    bool lt : 1;  // Less than
    bool gt : 1;  // Greater than
    bool eq : 1;  // Equal
    bool so : 1;  // Summary overflow
    
    u8 to_byte() const {
        return (lt << 3) | (gt << 2) | (eq << 1) | so;
    }
    
    void from_byte(u8 val) {
        lt = (val >> 3) & 1;
        gt = (val >> 2) & 1;
        eq = (val >> 1) & 1;
        so = val & 1;
    }
};

/**
 * PowerPC XER (Fixed-Point Exception Register)
 */
struct XER {
    bool so : 1;      // Summary overflow
    bool ov : 1;      // Overflow
    bool ca : 1;      // Carry
    u8 byte_count;    // For string instructions
    
    u32 to_u32() const {
        return (so << 31) | (ov << 30) | (ca << 29) | (byte_count & 0x7F);
    }
    
    void from_u32(u32 val) {
        so = (val >> 31) & 1;
        ov = (val >> 30) & 1;
        ca = (val >> 29) & 1;
        byte_count = val & 0x7F;
    }
};

/**
 * VMX128 vector register (128 bits)
 */
union VectorReg {
    u8  u8x16[16];
    u16 u16x8[8];
    u32 u32x4[4];
    u64 u64x2[2];
    f32 f32x4[4];
    f64 f64x2[2];
    
    // ARM64 NEON compatible
    #ifdef __aarch64__
    __attribute__((aligned(16))) uint8x16_t neon;
    #endif
};

/**
 * Thread context (one per hardware thread)
 */
struct ThreadContext {
    // General Purpose Registers (32 x 64-bit)
    std::array<u64, cpu::NUM_GPRS> gpr;
    
    // Floating Point Registers (32 x 64-bit)
    std::array<f64, cpu::NUM_FPRS> fpr;
    
    // Vector Registers (128 x 128-bit for VMX128)
    std::array<VectorReg, cpu::NUM_VMX_REGS> vr;
    
    // Special Purpose Registers
    u64 lr;     // Link Register
    u64 ctr;    // Count Register
    XER xer;    // Fixed-Point Exception Register
    
    // Condition Register (8 x 4-bit fields)
    std::array<CRField, 8> cr;
    
    // FPSCR (Floating-Point Status and Control Register)
    u32 fpscr;
    
    // VSCR (Vector Status and Control Register)
    u32 vscr;
    
    // Program Counter
    u64 pc;
    
    // MSR (Machine State Register)
    u64 msr;
    
    // Thread ID (0-5)
    u32 thread_id;
    
    // Execution state
    bool running;
    bool interrupted;
    
    void reset() {
        gpr.fill(0);
        fpr.fill(0.0);
        for (auto& v : vr) {
            v.u64x2[0] = v.u64x2[1] = 0;
        }
        lr = 0;
        ctr = 0;
        xer = {};
        for (auto& field : cr) {
            field = {};
        }
        fpscr = 0;
        vscr = 0;
        pc = 0;
        msr = 0;
        running = false;
        interrupted = false;
    }
};

/**
 * Decoded PowerPC instruction
 */
struct DecodedInst {
    u32 raw;            // Raw instruction word
    u8 opcode;          // Primary opcode (bits 0-5)
    u16 xo;             // Extended opcode (needs >8 bits for values like divd=489)
    u8 rd, rs, ra, rb;  // Register operands
    u8 rc;              // Record bit
    s16 simm;           // Signed immediate
    u16 uimm;           // Unsigned immediate
    s32 li;             // Branch offset
    u8 bo, bi;          // Branch operands
    u8 sh, mb, me;      // Rotate operands
    u8 crfd, crfs;      // CR field operands
    
    // Instruction type for dispatch
    enum class Type {
        Unknown,
        // Integer
        Add, AddCarrying, AddExtended,
        Sub, SubCarrying, SubExtended,
        Mul, MulHigh, Div,
        And, Or, Xor, Nand, Nor,
        Shift, Rotate,
        Compare, CompareLI,
        // Load/Store
        Load, Store, LoadUpdate, StoreUpdate,
        LoadMultiple, StoreMultiple,
        // Branch
        Branch, BranchConditional, BranchLink,
        // CR ops
        CRLogical, MTcrf, MFcr,
        // SPR ops
        MTspr, MFspr,
        // Float
        FAdd, FSub, FMul, FDiv, FMadd, FNeg, FAbs,
        FCompare, FConvert,
        // Vector (VMX128)
        VAdd, VSub, VMul, VDiv,
        VPerm, VMerge, VSplat,
        VCompare, VLogical,
        // System
        SC, RFI, ISYNC,
        TW, TD,  // Trap
        // Memory barrier
        SYNC, LWSYNC, EIEIO,
        // Cache
        DCBF, DCBST, DCBT, DCBZ, ICBI,
    } type = Type::Unknown;
};

/**
 * CPU instruction decoder
 */
class Decoder {
public:
    /**
     * Decode a single instruction
     */
    static DecodedInst decode(u32 instruction);
    
    /**
     * Get instruction mnemonic for debugging
     */
    static const char* get_mnemonic(const DecodedInst& inst);
    
    /**
     * Disassemble instruction to string
     */
    static std::string disassemble(u32 addr, u32 instruction);
};

/**
 * CPU interpreter (fallback when JIT unavailable or for single-stepping)
 */
class Interpreter {
public:
    Interpreter(Memory* memory);
    
    /**
     * Execute a single instruction
     * Returns cycles consumed
     */
    u32 execute_one(ThreadContext& ctx);
    
    /**
     * Execute until cycle count reached
     */
    void execute(ThreadContext& ctx, u64 cycles);
    
private:
    Memory* memory_;
    
    // Instruction handlers
    void exec_integer(ThreadContext& ctx, const DecodedInst& inst);
    void exec_integer_ext31(ThreadContext& ctx, const DecodedInst& inst);
    void exec_load_store(ThreadContext& ctx, const DecodedInst& inst);
    void exec_load_store_ds(ThreadContext& ctx, const DecodedInst& inst);
    void exec_branch(ThreadContext& ctx, const DecodedInst& inst);
    void exec_float(ThreadContext& ctx, const DecodedInst& inst);
    void exec_float_complete(ThreadContext& ctx, const DecodedInst& inst);
    void exec_rotate64(ThreadContext& ctx, const DecodedInst& inst);
    void exec_vector(ThreadContext& ctx, const DecodedInst& inst);
    void exec_system(ThreadContext& ctx, const DecodedInst& inst);
    
    // Memory access helpers
    u8 read_u8(ThreadContext& ctx, GuestAddr addr);
    u16 read_u16(ThreadContext& ctx, GuestAddr addr);
    u32 read_u32(ThreadContext& ctx, GuestAddr addr);
    u64 read_u64(ThreadContext& ctx, GuestAddr addr);
    
    void write_u8(ThreadContext& ctx, GuestAddr addr, u8 value);
    void write_u16(ThreadContext& ctx, GuestAddr addr, u16 value);
    void write_u32(ThreadContext& ctx, GuestAddr addr, u32 value);
    void write_u64(ThreadContext& ctx, GuestAddr addr, u64 value);
    
    // CR update helpers
    void update_cr0(ThreadContext& ctx, s64 result);
    void update_cr1(ThreadContext& ctx);
};

/**
 * Main CPU class
 * Manages all 6 hardware threads across 3 cores
 */
class Cpu {
public:
    Cpu();
    ~Cpu();
    
    /**
     * Initialize CPU subsystem
     */
    Status initialize(Memory* memory, const CpuConfig& config);
    
    /**
     * Shutdown and release resources
     */
    void shutdown();
    
    /**
     * Reset CPU state
     */
    void reset();
    
    /**
     * Execute cycles across all active threads
     */
    void execute(u64 cycles);
    
    /**
     * Start/stop individual threads
     */
    Status start_thread(u32 thread_id, GuestAddr entry_point, GuestAddr stack);
    void stop_thread(u32 thread_id);
    
    /**
     * Interrupt handling
     */
    void raise_interrupt(u32 thread_id, u32 interrupt);
    void clear_interrupt(u32 thread_id, u32 interrupt);
    
    /**
     * Get thread context (for debugging)
     */
    ThreadContext& get_context(u32 thread_id);
    const ThreadContext& get_context(u32 thread_id) const;
    
    /**
     * Check if any thread is running
     */
    bool any_running() const;
    
private:
    Memory* memory_ = nullptr;
    CpuConfig config_;
    
    // Thread contexts
    std::array<ThreadContext, cpu::NUM_THREADS> contexts_;
    
    // Execution engines
    std::unique_ptr<Interpreter> interpreter_;
    
#ifdef X360MU_JIT_ENABLED
    std::unique_ptr<JitCompiler> jit_;
#endif
    
    // Execute single thread for given cycles
    void execute_thread(u32 thread_id, u64 cycles);
};

} // namespace x360mu

