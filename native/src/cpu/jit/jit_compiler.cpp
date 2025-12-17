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

// Size of temporary code buffer
constexpr size_t TEMP_BUFFER_SIZE = 64 * 1024;

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
        LOGE("Failed to allocate JIT code cache (%llu bytes)", cache_size);
        return Status::ErrorOutOfMemory;
    }
#else
    // Non-ARM64 fallback (for testing on x86)
    code_cache_ = new u8[cache_size];
#endif
    
    code_write_ptr_ = code_cache_;
    
    // Generate dispatcher and exit stub
    generate_dispatcher();
    generate_exit_stub();
    
    LOGI("JIT initialized with %lluMB cache", cache_size / (1024 * 1024));
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

void JitCompiler::execute(ThreadContext& ctx, u64 cycles) {
#ifdef __aarch64__
    // Run the dispatcher which will execute compiled code
    if (dispatcher_) {
        dispatcher_(&ctx, this);
    }
#else
    // Fallback to interpreter on non-ARM64 platforms
    LOGE("JIT only supported on ARM64");
#endif
}

void JitCompiler::invalidate(GuestAddr addr, u32 size) {
    std::lock_guard<std::mutex> lock(block_map_mutex_);
    
    // Find and remove any blocks that overlap with the modified region
    GuestAddr end_addr = addr + size;
    
    for (auto it = block_map_.begin(); it != block_map_.end();) {
        CompiledBlock* block = it->second;
        GuestAddr block_end = block->start_addr + block->size * 4;
        
        if (block->start_addr < end_addr && block_end > addr) {
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
    
    // Reset code write pointer
    code_write_ptr_ = code_cache_ + 4096; // Leave room for dispatcher
    stats_ = {};
}

CompiledBlock* JitCompiler::compile_block(GuestAddr addr) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(block_map_mutex_);
        auto it = block_map_.find(addr);
        if (it != block_map_.end()) {
            stats_.cache_hits++;
            return it->second;
        }
    }
    
    stats_.cache_misses++;
    
    // Allocate new block
    CompiledBlock* block = new CompiledBlock();
    block->start_addr = addr;
    block->code = code_write_ptr_;
    block->execution_count = 0;
    
    // Create temporary buffer for code generation
    u8 temp_buffer[TEMP_BUFFER_SIZE];
    ARM64Emitter emit(temp_buffer, TEMP_BUFFER_SIZE);
    
    // Template context for compilation
    ThreadContext ctx_template = {};
    
    // Prologue: We expect X19 = context pointer, X20 = memory base
    // Save state to context before each block can be optimized later
    
    GuestAddr pc = addr;
    u32 inst_count = 0;
    bool block_ended = false;
    
    while (!block_ended && inst_count < MAX_BLOCK_INSTRUCTIONS) {
        // Fetch instruction from PPC memory (big-endian)
        u32 ppc_inst = memory_->read_u32(pc);
        
        // Decode
        DecodedInst decoded = Decoder::decode(ppc_inst);
        
        // Compile instruction
        compile_instruction(emit, ctx_template, decoded, pc);
        
        inst_count++;
        pc += 4;
        
        // Check if this instruction ends the block
        switch (decoded.type) {
            case DecodedInst::Type::Branch:
            case DecodedInst::Type::BranchConditional:
            case DecodedInst::Type::BranchLink:
            case DecodedInst::Type::SC:
            case DecodedInst::Type::RFI:
                block_ended = true;
                break;
            default:
                break;
        }
    }
    
    // If block didn't end with a branch, add fallthrough
    if (!block_ended) {
        // Update PC
        emit.MOV_imm(arm64::X0, pc);
        emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
        
        // Jump to exit stub
        s64 exit_offset = reinterpret_cast<u8*>(exit_stub_) - emit.current();
        emit.B(exit_offset);
    }
    
    block->size = inst_count;
    block->code_size = emit.size();
    
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
    
    // Add to cache
    {
        std::lock_guard<std::mutex> lock(block_map_mutex_);
        block_map_[addr] = block;
    }
    
    stats_.blocks_compiled++;
    stats_.code_bytes_used = code_write_ptr_ - code_cache_;
    
    // Try to link this block to others
    try_link_block(block);
    
    return block;
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
            
        default:
            // Fallback: call interpreter for this instruction
            // Store PC
            emit.MOV_imm(arm64::X0, pc);
            emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
            
            // Could call interpreter here, but for now just NOP
            emit.NOP();
            break;
    }
    
    stats_.instructions_executed++;
}

//=============================================================================
// Integer Instruction Compilation
//=============================================================================

void JitCompiler::compile_add(ARM64Emitter& emit, const DecodedInst& inst) {
    // Load operands
    if (inst.opcode == 14) { // addi
        // addi rD, rA, SIMM
        if (inst.ra == 0) {
            // li rD, SIMM
            emit.MOV_imm(arm64::X0, static_cast<u64>(static_cast<s64>(inst.simm)));
        } else {
            load_gpr(emit, arm64::X0, inst.ra);
            if (inst.simm >= 0) {
                emit.ADD_imm(arm64::X0, arm64::X0, inst.simm);
            } else {
                emit.SUB_imm(arm64::X0, arm64::X0, -inst.simm);
            }
        }
        store_gpr(emit, inst.rd, arm64::X0);
    }
    else if (inst.opcode == 15) { // addis
        // addis rD, rA, SIMM
        if (inst.ra == 0) {
            emit.MOV_imm(arm64::X0, static_cast<u64>(static_cast<s64>(inst.simm) << 16));
        } else {
            load_gpr(emit, arm64::X0, inst.ra);
            s32 imm = static_cast<s32>(inst.simm) << 16;
            if (imm >= 0 && imm < 4096) {
                emit.ADD_imm(arm64::X0, arm64::X0, imm);
            } else {
                emit.MOV_imm(arm64::X1, imm);
                emit.ADD(arm64::X0, arm64::X0, arm64::X1);
            }
        }
        store_gpr(emit, inst.rd, arm64::X0);
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
                // Store carry to XER.CA
                emit.CSET(arm64::X2, arm64_cond::CS);
                // ... store to XER
                break;
            case 138: // adde
                // Load XER.CA
                // emit.LDR(...);
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
        emit.SUB(arm64::X0, arm64::X0, arm64::X1);
        store_gpr(emit, inst.rd, arm64::X0);
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
            case 235: // mullw (32-bit)
                emit.SXTW(arm64::X0, arm64::X0);
                emit.SXTW(arm64::X1, arm64::X1);
                emit.MUL(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 233: // mulld (64-bit)
                emit.MUL(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 75: // mulhw
                emit.SXTW(arm64::X0, arm64::X0);
                emit.SXTW(arm64::X1, arm64::X1);
                emit.SMULH(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 11: // mulhwu
                emit.UXTW(arm64::X0, arm64::X0);
                emit.UXTW(arm64::X1, arm64::X1);
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
    
    // Check for division by zero
    emit.CBNZ(arm64::X1, 8); // Skip if not zero
    emit.MOV_imm(arm64::X0, 0);
    emit.B(20); // Skip division
    
    switch (inst.xo) {
        case 491: // divw
            emit.SXTW(arm64::X0, arm64::X0);
            emit.SXTW(arm64::X1, arm64::X1);
            emit.SDIV(arm64::X0, arm64::X0, arm64::X1);
            break;
        case 459: // divwu
            emit.UXTW(arm64::X0, arm64::X0);
            emit.UXTW(arm64::X1, arm64::X1);
            emit.UDIV(arm64::X0, arm64::X0, arm64::X1);
            break;
    }
    
    store_gpr(emit, inst.rd, arm64::X0);
}

void JitCompiler::compile_logical(ARM64Emitter& emit, const DecodedInst& inst) {
    if (inst.opcode == 24) { // ori
        load_gpr(emit, arm64::X0, inst.rs);
        emit.ORR_imm(arm64::X0, arm64::X0, inst.uimm);
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
        emit.EOR_imm(arm64::X0, arm64::X0, inst.uimm);
        store_gpr(emit, inst.ra, arm64::X0);
    }
    else if (inst.opcode == 28) { // andi.
        load_gpr(emit, arm64::X0, inst.rs);
        emit.AND_imm(arm64::X0, arm64::X0, inst.uimm);
        store_gpr(emit, inst.ra, arm64::X0);
        compile_cr_update(emit, 0, arm64::X0);
    }
    else if (inst.opcode == 31) {
        load_gpr(emit, arm64::X0, inst.ra);
        load_gpr(emit, arm64::X1, inst.rb);
        
        switch (inst.xo) {
            case 28: // and
                emit.AND(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 60: // andc
                emit.BIC(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 444: // or
                emit.ORR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 412: // orc
                emit.ORN(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 316: // xor
                emit.EOR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 124: // nor
                emit.ORR(arm64::X0, arm64::X0, arm64::X1);
                emit.MOV_imm(arm64::X1, ~0ULL);
                emit.EOR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 476: // nand
                emit.AND(arm64::X0, arm64::X0, arm64::X1);
                emit.MOV_imm(arm64::X1, ~0ULL);
                emit.EOR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 26: // cntlzw
                load_gpr(emit, arm64::X0, inst.rs);
                emit.UXTW(arm64::X0, arm64::X0);
                emit.CLZ(arm64::X0, arm64::X0);
                emit.SUB_imm(arm64::X0, arm64::X0, 32); // Adjust for 64-bit CLZ
                break;
            case 922: // extsh
                load_gpr(emit, arm64::X0, inst.rs);
                emit.SXTH(arm64::X0, arm64::X0);
                break;
            case 954: // extsb
                load_gpr(emit, arm64::X0, inst.rs);
                emit.SXTB(arm64::X0, arm64::X0);
                break;
            case 986: // extsw
                load_gpr(emit, arm64::X0, inst.rs);
                emit.SXTW(arm64::X0, arm64::X0);
                break;
        }
        
        store_gpr(emit, inst.ra, arm64::X0);
        
        if (inst.rc) {
            compile_cr_update(emit, 0, arm64::X0);
        }
    }
}

void JitCompiler::compile_shift(ARM64Emitter& emit, const DecodedInst& inst) {
    load_gpr(emit, arm64::X0, inst.rs);
    
    if (inst.opcode == 31) {
        switch (inst.xo) {
            case 24: // slw
                load_gpr(emit, arm64::X1, inst.rb);
                emit.AND_imm(arm64::X1, arm64::X1, 0x3F);
                emit.LSL(arm64::X0, arm64::X0, arm64::X1);
                emit.UXTW(arm64::X0, arm64::X0);
                break;
            case 536: // srw
                load_gpr(emit, arm64::X1, inst.rb);
                emit.UXTW(arm64::X0, arm64::X0);
                emit.AND_imm(arm64::X1, arm64::X1, 0x3F);
                emit.LSR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 792: // sraw
                load_gpr(emit, arm64::X1, inst.rb);
                emit.SXTW(arm64::X0, arm64::X0);
                emit.AND_imm(arm64::X1, arm64::X1, 0x3F);
                emit.ASR(arm64::X0, arm64::X0, arm64::X1);
                break;
            case 824: // srawi
                emit.SXTW(arm64::X0, arm64::X0);
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
    if (inst.opcode == 21) { // rlwinm
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
        
        emit.AND_imm(arm64::X0, arm64::X0, mask);
        store_gpr(emit, inst.ra, arm64::X0);
        
        if (inst.rc) {
            compile_cr_update(emit, 0, arm64::X0);
        }
    }
}

void JitCompiler::compile_compare(ARM64Emitter& emit, const DecodedInst& inst) {
    int crfd = inst.crfd;
    
    if (inst.opcode == 11) { // cmpi
        load_gpr(emit, arm64::X0, inst.ra);
        emit.MOV_imm(arm64::X1, static_cast<u64>(static_cast<s64>(inst.simm)));
        emit.CMP(arm64::X0, arm64::X1);
    }
    else if (inst.opcode == 10) { // cmpli
        load_gpr(emit, arm64::X0, inst.ra);
        emit.CMP_imm(arm64::X0, inst.uimm);
    }
    else if (inst.opcode == 31) {
        load_gpr(emit, arm64::X0, inst.ra);
        load_gpr(emit, arm64::X1, inst.rb);
        emit.CMP(arm64::X0, arm64::X1);
    }
    
    // Set CR field based on comparison
    // LT = N, GT = !N && !Z, EQ = Z, SO = copy from XER
    size_t cr_offset = ctx_offset_cr(crfd);
    
    // Store LT (negative flag)
    emit.CSET(arm64::X2, arm64_cond::MI);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset);
    
    // Store GT (!N && !Z)
    emit.CSET(arm64::X2, arm64_cond::GT);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 1);
    
    // Store EQ
    emit.CSET(arm64::X2, arm64_cond::EQ);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 2);
    
    // SO = XER.SO (just set to 0 for now)
    emit.STRB(31, arm64::CTX_REG, cr_offset + 3); // Store 0
}

//=============================================================================
// Load/Store Compilation
//=============================================================================

void JitCompiler::compile_load(ARM64Emitter& emit, const DecodedInst& inst) {
    // Calculate effective address
    if (inst.opcode != 31) {
        calc_ea(emit, arm64::X0, inst.ra, inst.simm);
    } else {
        calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    }
    
    // Add memory base
    emit.ADD(arm64::X0, arm64::X0, arm64::MEM_BASE);
    
    // Load based on opcode
    switch (inst.opcode) {
        case 32: // lwz
        case 33: // lwzu
            emit.LDR(arm64::X1, arm64::X0);
            emit.UXTW(arm64::X1, arm64::X1);
            byteswap32(emit, arm64::X1);
            break;
        case 34: // lbz
        case 35: // lbzu
            emit.LDRB(arm64::X1, arm64::X0);
            break;
        case 40: // lhz
        case 41: // lhzu
            emit.LDRH(arm64::X1, arm64::X0);
            byteswap16(emit, arm64::X1);
            break;
        case 42: // lha
        case 43: // lhau
            emit.LDRSH(arm64::X1, arm64::X0);
            byteswap16(emit, arm64::X1);
            emit.SXTH(arm64::X1, arm64::X1);
            break;
    }
    
    store_gpr(emit, inst.rd, arm64::X1);
    
    // Update RA for update forms
    if (inst.opcode == 33 || inst.opcode == 35 || inst.opcode == 41 || inst.opcode == 43) {
        emit.SUB(arm64::X0, arm64::X0, arm64::MEM_BASE); // Get guest address
        store_gpr(emit, inst.ra, arm64::X0);
    }
}

void JitCompiler::compile_store(ARM64Emitter& emit, const DecodedInst& inst) {
    // Calculate effective address
    if (inst.opcode != 31) {
        calc_ea(emit, arm64::X0, inst.ra, inst.simm);
    } else {
        calc_ea_indexed(emit, arm64::X0, inst.ra, inst.rb);
    }
    
    // Load value to store
    load_gpr(emit, arm64::X1, inst.rs);
    
    // Add memory base
    emit.ADD(arm64::X0, arm64::X0, arm64::MEM_BASE);
    
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
    }
    
    // Update RA for update forms
    if (inst.opcode == 37 || inst.opcode == 39 || inst.opcode == 45) {
        emit.SUB(arm64::X0, arm64::X0, arm64::MEM_BASE);
        store_gpr(emit, inst.ra, arm64::X0);
    }
}

void JitCompiler::compile_load_multiple(ARM64Emitter& emit, const DecodedInst& inst) {
    calc_ea(emit, arm64::X0, inst.ra, inst.simm);
    emit.ADD(arm64::X0, arm64::X0, arm64::MEM_BASE);
    
    for (u32 r = inst.rd; r < 32; r++) {
        emit.LDR(arm64::X1, arm64::X0, (r - inst.rd) * 4);
        byteswap32(emit, arm64::X1);
        store_gpr(emit, r, arm64::X1);
    }
}

void JitCompiler::compile_store_multiple(ARM64Emitter& emit, const DecodedInst& inst) {
    calc_ea(emit, arm64::X0, inst.ra, inst.simm);
    emit.ADD(arm64::X0, arm64::X0, arm64::MEM_BASE);
    
    for (u32 r = inst.rs; r < 32; r++) {
        load_gpr(emit, arm64::X1, r);
        byteswap32(emit, arm64::X1);
        emit.STR(arm64::X1, arm64::X0, (r - inst.rs) * 4);
    }
}

//=============================================================================
// Branch Compilation
//=============================================================================

void JitCompiler::compile_branch(ARM64Emitter& emit, const DecodedInst& inst, 
                                  GuestAddr pc, CompiledBlock* block) {
    GuestAddr target;
    
    if (inst.raw & 2) { // Absolute
        target = inst.li;
    } else {
        target = pc + inst.li;
    }
    
    // Link if LK=1
    if (inst.raw & 1) {
        emit.MOV_imm(arm64::X0, pc + 4);
        emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_lr());
    }
    
    // Update PC and exit
    emit.MOV_imm(arm64::X0, target);
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
    
    // Jump to exit stub (will be patched for direct linking)
    s64 exit_offset = reinterpret_cast<u8*>(exit_stub_) - emit.current();
    emit.B(exit_offset);
}

void JitCompiler::compile_branch_conditional(ARM64Emitter& emit, const DecodedInst& inst,
                                             GuestAddr pc, CompiledBlock* block) {
    u8 bo = inst.bo;
    u8 bi = inst.bi;
    
    // Calculate taken/not-taken targets
    GuestAddr target_taken;
    GuestAddr target_not_taken = pc + 4;
    
    if (inst.opcode == 16) { // bc
        if (inst.raw & 2) { // AA
            target_taken = inst.simm;
        } else {
            target_taken = pc + inst.simm;
        }
    } else if (inst.xo == 16) { // bclr
        // Target is LR
        emit.LDR(arm64::X2, arm64::CTX_REG, ctx_offset_lr());
    } else if (inst.xo == 528) { // bcctr
        // Target is CTR
        emit.LDR(arm64::X2, arm64::CTX_REG, ctx_offset_ctr());
    }
    
    // Handle BO field
    bool decrement_ctr = !(bo & 0x04);
    bool test_ctr = decrement_ctr;
    bool test_cond = !(bo & 0x10);
    
    u32* skip_label = nullptr;
    
    if (decrement_ctr) {
        // CTR = CTR - 1
        emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
        emit.SUB_imm(arm64::X0, arm64::X0, 1);
        emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_ctr());
        
        // Test CTR
        if (bo & 0x02) { // CTR == 0
            emit.CBNZ(arm64::X0, 0); // Will patch
            skip_label = emit.label_here() - 1;
        } else { // CTR != 0
            emit.CBZ(arm64::X0, 0);
            skip_label = emit.label_here() - 1;
        }
    }
    
    if (test_cond) {
        // Load CR bit
        int cr_field = bi / 4;
        int cr_bit = bi % 4;
        
        emit.LDRB(arm64::X0, arm64::CTX_REG, ctx_offset_cr(cr_field) + cr_bit);
        
        if (bo & 0x08) { // Test for 1
            emit.CBZ(arm64::X0, 0);
        } else { // Test for 0
            emit.CBNZ(arm64::X0, 0);
        }
        
        // Patch previous skip if needed
        if (skip_label) {
            u32* current = emit.label_here();
            // Calculate offset and patch
        }
        skip_label = emit.label_here() - 1;
    }
    
    // Link if LK=1
    if (inst.raw & 1) {
        emit.MOV_imm(arm64::X0, pc + 4);
        emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_lr());
    }
    
    // Branch taken: update PC
    if (inst.opcode == 16) {
        emit.MOV_imm(arm64::X0, target_taken);
    } else {
        // X2 already has target from LR/CTR
        emit.AND_imm(arm64::X0, arm64::X2, ~3ULL);
    }
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
    
    // Exit
    s64 exit_offset = reinterpret_cast<u8*>(exit_stub_) - emit.current();
    emit.B(exit_offset);
    
    // Not taken: patch skip and continue
    if (skip_label) {
        u32* current = emit.label_here();
        s64 offset = (reinterpret_cast<u8*>(current) - reinterpret_cast<u8*>(skip_label));
        // Would patch skip_label here
    }
    
    // Update PC for not-taken
    emit.MOV_imm(arm64::X0, target_not_taken);
    emit.STR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
}

//=============================================================================
// Float Compilation
//=============================================================================

void JitCompiler::compile_float(ARM64Emitter& emit, const DecodedInst& inst) {
    // Load FPR operands into NEON registers
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
    
    // VMX128 to NEON mapping
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
        default:
            // Fallback - NOP
            emit.NOP();
            break;
    }
    
    store_vr(emit, inst.rd, 0);
}

//=============================================================================
// System Instruction Compilation
//=============================================================================

void JitCompiler::compile_syscall(ARM64Emitter& emit, const DecodedInst& inst) {
    // Set interrupted flag
    emit.MOV_imm(arm64::X0, 1);
    emit.STRB(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, interrupted));
    
    // Exit to dispatcher
    s64 exit_offset = reinterpret_cast<u8*>(exit_stub_) - emit.current();
    emit.B(exit_offset);
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
        default:
            // Ignore other SPRs for now
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
        case 268: // TBL
        case 269: // TBU
            // Return time base - simplified
            emit.MRS(arm64::X0, 0x5F01); // CNTVCT_EL0 (virtual counter)
            break;
        default:
            emit.MOV_imm(arm64::X0, 0);
            break;
    }
    
    store_gpr(emit, inst.rd, arm64::X0);
}

void JitCompiler::compile_cr_logical(ARM64Emitter& emit, const DecodedInst& inst) {
    // CR logical operations - implement as needed
    emit.NOP();
}

//=============================================================================
// CR Update
//=============================================================================

void JitCompiler::compile_cr_update(ARM64Emitter& emit, int field, int result_reg) {
    size_t cr_offset = ctx_offset_cr(field);
    
    // Compare result with 0 to set flags
    emit.CMP_imm(result_reg, 0);
    
    // LT = result < 0 (N flag)
    emit.CSET(arm64::X2, arm64_cond::MI);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset);
    
    // GT = result > 0 (!N && !Z)
    emit.CSET(arm64::X2, arm64_cond::GT);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 1);
    
    // EQ = result == 0 (Z flag)
    emit.CSET(arm64::X2, arm64_cond::EQ);
    emit.STRB(arm64::X2, arm64::CTX_REG, cr_offset + 2);
    
    // SO = XER.SO (keep existing)
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
    if (ppc_reg != 0) {
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
            if (offset > 0) {
                emit.ADD_imm(dest_reg, dest_reg, offset);
            } else {
                emit.SUB_imm(dest_reg, dest_reg, -offset);
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

void JitCompiler::byteswap32(ARM64Emitter& emit, int reg) {
    // Reverse bytes in 32-bit value
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
// Block Linking
//=============================================================================

void JitCompiler::try_link_block(CompiledBlock* block) {
    // Try to link this block's exits to already-compiled blocks
    // And link other blocks' exits to this block
    
    // This is an optimization - skip for now
}

void JitCompiler::unlink_block(CompiledBlock* block) {
    // Remove links to/from this block
}

//=============================================================================
// Dispatcher
//=============================================================================

void JitCompiler::generate_dispatcher() {
#ifdef __aarch64__
    ARM64Emitter emit(code_cache_, 4096);
    
    // Dispatcher entry point
    // X0 = ThreadContext*
    // X1 = JitCompiler*
    
    // Save callee-saved registers
    emit.STP(arm64::X29, arm64::X30, arm64::SP, -16);
    emit.STP(arm64::X19, arm64::X20, arm64::SP, -32);
    emit.STP(arm64::X21, arm64::X22, arm64::SP, -48);
    emit.STP(arm64::X23, arm64::X24, arm64::SP, -64);
    emit.STP(arm64::X25, arm64::X26, arm64::SP, -80);
    emit.STP(arm64::X27, arm64::X28, arm64::SP, -96);
    emit.SUB_imm(arm64::SP, arm64::SP, 96);
    
    // Set up context register
    emit.ORR(arm64::CTX_REG, arm64::X0, arm64::X0); // MOV X19, X0
    
    // Set up memory base (will be filled in during execute)
    // emit.MOV_imm(arm64::MEM_BASE, ...);
    
    // Main dispatch loop
    u32* loop_start = emit.label_here();
    
    // Check if still running
    emit.LDRB(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, running));
    emit.CBZ(arm64::X0, 0); // Exit if not running - will patch
    u32* exit_patch = emit.label_here() - 1;
    
    // Check if interrupted
    emit.LDRB(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, interrupted));
    emit.CBNZ(arm64::X0, 0); // Exit if interrupted - will patch
    u32* int_patch = emit.label_here() - 1;
    
    // Load PC
    emit.LDR(arm64::X0, arm64::CTX_REG, ctx_offset_pc());
    
    // Look up compiled block (simplified - would call lookup function)
    // For now, just exit to interpreter
    emit.B(0);
    u32* interp_patch = emit.label_here() - 1;
    
    // Exit point
    u32* exit_point = emit.label_here();
    
    // Restore callee-saved registers
    emit.ADD_imm(arm64::SP, arm64::SP, 96);
    emit.LDP(arm64::X27, arm64::X28, arm64::SP, -96);
    emit.LDP(arm64::X25, arm64::X26, arm64::SP, -80);
    emit.LDP(arm64::X23, arm64::X24, arm64::SP, -64);
    emit.LDP(arm64::X21, arm64::X22, arm64::SP, -48);
    emit.LDP(arm64::X19, arm64::X20, arm64::SP, -32);
    emit.LDP(arm64::X29, arm64::X30, arm64::SP, -16);
    emit.RET();
    
    // Patch branches
    // ...
    
    dispatcher_ = reinterpret_cast<DispatcherFunc>(code_cache_);
    
    __builtin___clear_cache(
        reinterpret_cast<char*>(code_cache_),
        reinterpret_cast<char*>(code_cache_) + emit.size()
    );
    
    code_write_ptr_ = code_cache_ + emit.size();
    // Align
    code_write_ptr_ = reinterpret_cast<u8*>(
        (reinterpret_cast<uintptr_t>(code_write_ptr_) + 15) & ~15
    );
#endif
}

void JitCompiler::generate_exit_stub() {
#ifdef __aarch64__
    exit_stub_ = code_write_ptr_;
    
    ARM64Emitter emit(code_write_ptr_, 256);
    
    // Exit stub - return to dispatcher
    // PC should already be updated
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

} // namespace x360mu

