# Task: JIT Code Generation (PowerPC → ARM64)

## Priority: 🔴 CRITICAL (Performance)
## Estimated Time: 6-8 weeks
## Dependencies: None (interpreter provides fallback)

---

## Objective

Generate ARM64 machine code from PowerPC instructions for ~100x speedup over interpreter.

---

## What To Build

### Location
- `native/src/cpu/jit/arm64_emitter.cpp`
- `native/src/cpu/jit/jit_compiler.cpp`
- `native/src/cpu/jit/block_cache.cpp`

---

## Current State

The framework exists in `jit.h` and `jit_compiler.cpp` but doesn't generate actual ARM64 code.

---

## Specific Implementation

### 1. ARM64 Code Emitter

```cpp
class Arm64Emitter {
public:
    Arm64Emitter(u8* buffer, size_t size);
    
    // Get current position
    u8* code_ptr() const { return ptr_; }
    size_t code_size() const { return ptr_ - buffer_; }
    
    // Arithmetic
    void ADD(Reg rd, Reg rn, Reg rm);      // rd = rn + rm
    void ADD_imm(Reg rd, Reg rn, u32 imm); // rd = rn + imm
    void SUB(Reg rd, Reg rn, Reg rm);
    void MUL(Reg rd, Reg rn, Reg rm);
    void SDIV(Reg rd, Reg rn, Reg rm);
    void UDIV(Reg rd, Reg rn, Reg rm);
    void MADD(Reg rd, Reg rn, Reg rm, Reg ra); // rd = ra + rn*rm
    void SMULH(Reg rd, Reg rn, Reg rm);    // High 64 bits of 128-bit multiply
    
    // Logical
    void AND(Reg rd, Reg rn, Reg rm);
    void ORR(Reg rd, Reg rn, Reg rm);
    void EOR(Reg rd, Reg rn, Reg rm);
    void MVN(Reg rd, Reg rm);              // rd = ~rm
    
    // Shifts
    void LSL(Reg rd, Reg rn, Reg rm);      // rd = rn << rm
    void LSL_imm(Reg rd, Reg rn, u32 sh);
    void LSR(Reg rd, Reg rn, Reg rm);      // rd = rn >> rm (unsigned)
    void ASR(Reg rd, Reg rn, Reg rm);      // rd = rn >> rm (signed)
    
    // Memory
    void LDR(Reg rt, Reg rn, s32 offset);  // rt = [rn + offset]
    void LDRB(Reg rt, Reg rn, s32 offset);
    void LDRH(Reg rt, Reg rn, s32 offset);
    void LDRSW(Reg rt, Reg rn, s32 offset);
    void STR(Reg rt, Reg rn, s32 offset);  // [rn + offset] = rt
    void STRB(Reg rt, Reg rn, s32 offset);
    void STRH(Reg rt, Reg rn, s32 offset);
    
    // Branches
    void B(s32 offset);                     // Unconditional branch
    void B_cond(Condition cond, s32 offset);
    void BL(s32 offset);                    // Branch with link
    void BR(Reg rn);                        // Branch to register
    void BLR(Reg rn);                       // Branch with link to register
    void RET(Reg rn = X30);
    
    // Compare
    void CMP(Reg rn, Reg rm);
    void CMP_imm(Reg rn, u32 imm);
    void TST(Reg rn, Reg rm);
    
    // Move
    void MOV(Reg rd, Reg rm);
    void MOV_imm(Reg rd, u64 imm);
    void MOVZ(Reg rd, u16 imm, u8 shift);  // Move with zero
    void MOVK(Reg rd, u16 imm, u8 shift);  // Move keep
    
    // Floating point
    void FADD(FReg rd, FReg rn, FReg rm);
    void FSUB(FReg rd, FReg rn, FReg rm);
    void FMUL(FReg rd, FReg rn, FReg rm);
    void FDIV(FReg rd, FReg rn, FReg rm);
    void FCMP(FReg rn, FReg rm);
    void FCVTZS(Reg rd, FReg rn);          // Float to signed int
    void SCVTF(FReg rd, Reg rn);           // Signed int to float
    
    // NEON (for VMX128)
    void FADD_v4s(VReg rd, VReg rn, VReg rm);  // 4x float add
    void FMUL_v4s(VReg rd, VReg rn, VReg rm);
    void LD1(VReg vt, Reg rn);             // Load vector
    void ST1(VReg vt, Reg rn);             // Store vector
    
private:
    void emit32(u32 inst);
    
    u8* buffer_;
    u8* ptr_;
    size_t size_;
};

// ARM64 register names
enum Reg : u8 {
    X0, X1, X2, X3, X4, X5, X6, X7,
    X8, X9, X10, X11, X12, X13, X14, X15,
    X16, X17, X18, X19, X20, X21, X22, X23,
    X24, X25, X26, X27, X28, X29, X30, XZR,
    SP = 31,
};
```

### 2. Register Allocation

Map PowerPC GPRs to ARM64 registers:

```cpp
class RegisterAllocator {
public:
    // PPC register -> ARM64 register
    // r0-r13  -> X0-X13 (caller saved, use directly)
    // r14-r31 -> X19-X28 + spill (callee saved)
    // Special:
    //   X29 = frame pointer
    //   X30 = link register
    //   SP  = stack pointer
    
    Reg get_gpr(u8 ppc_reg);
    void spill_if_needed(u8 ppc_reg);
    void restore_if_needed(u8 ppc_reg);
    
    // Context pointer (ThreadContext* passed in X0)
    static constexpr Reg CTX = X20;
    
    // Memory base pointer (for fastmem)
    static constexpr Reg MEM_BASE = X21;
    
private:
    std::array<Reg, 32> mapping_;
    std::bitset<32> dirty_;
    std::bitset<32> in_register_;
};
```

### 3. Block Compilation

```cpp
class JitCompiler {
public:
    JitCompiler(Memory* memory);
    
    // Compile a basic block starting at pc
    CompiledBlock* compile_block(u64 guest_pc);
    
    // Execute compiled code
    void execute(ThreadContext& ctx);
    
private:
    void emit_prologue();
    void emit_epilogue();
    
    // Compile single instruction
    void compile_instruction(const DecodedInst& inst);
    
    // Specific instruction compilers
    void compile_add(const DecodedInst& inst);
    void compile_addi(const DecodedInst& inst);
    void compile_subf(const DecodedInst& inst);
    void compile_mullw(const DecodedInst& inst);
    void compile_divw(const DecodedInst& inst);
    void compile_and(const DecodedInst& inst);
    void compile_or(const DecodedInst& inst);
    void compile_xor(const DecodedInst& inst);
    void compile_slw(const DecodedInst& inst);
    void compile_srw(const DecodedInst& inst);
    void compile_lwz(const DecodedInst& inst);
    void compile_stw(const DecodedInst& inst);
    void compile_b(const DecodedInst& inst);
    void compile_bc(const DecodedInst& inst);
    
    // Memory access via fastmem
    void emit_load_u32(Reg dest, Reg addr_reg);
    void emit_store_u32(Reg src, Reg addr_reg);
    
    Arm64Emitter emit_;
    RegisterAllocator regs_;
    Memory* memory_;
    BlockCache* cache_;
};
```

### 4. Example: Compiling `add`

```cpp
void JitCompiler::compile_add(const DecodedInst& inst) {
    // add rD, rA, rB
    // rD = rA + rB
    
    Reg rd = regs_.get_gpr(inst.rd);
    Reg ra = regs_.get_gpr(inst.ra);
    Reg rb = regs_.get_gpr(inst.rb);
    
    emit_.ADD(rd, ra, rb);
    
    if (inst.rc) {
        // Update CR0 based on result
        emit_.CMP_imm(rd, 0);
        emit_update_cr0(rd);
    }
}

void JitCompiler::compile_lwz(const DecodedInst& inst) {
    // lwz rD, d(rA)
    // rD = mem[rA + d]
    
    Reg rd = regs_.get_gpr(inst.rd);
    Reg addr = X9;  // Temp register
    
    if (inst.ra == 0) {
        emit_.MOV_imm(addr, inst.simm);
    } else {
        Reg ra = regs_.get_gpr(inst.ra);
        emit_.ADD_imm(addr, ra, inst.simm);
    }
    
    // Fastmem: addr = MEM_BASE + guest_addr
    emit_.ADD(addr, MEM_BASE, addr);
    
    // Load with byte swap (big endian)
    emit_.LDR(rd, addr, 0);
    emit_.REV(rd, rd);  // Byte swap
}
```

### 5. Block Cache

```cpp
class BlockCache {
public:
    BlockCache(size_t code_cache_size = 64 * 1024 * 1024);
    
    CompiledBlock* get(u64 guest_pc);
    void insert(u64 guest_pc, CompiledBlock* block);
    void invalidate(u64 guest_pc);
    void invalidate_range(u64 start, u64 end);
    void clear();
    
    // Allocate executable memory for code
    u8* allocate_code(size_t size);
    
private:
    std::unordered_map<u64, CompiledBlock*> blocks_;
    
    // Executable memory region
    u8* code_cache_;
    u8* code_ptr_;
    size_t code_size_;
};

struct CompiledBlock {
    u64 guest_pc;
    u64 guest_end_pc;
    void* host_code;
    size_t code_size;
    u32 instruction_count;
};
```

### 6. Block Execution

```cpp
void JitCompiler::execute(ThreadContext& ctx) {
    while (ctx.running) {
        // Look up compiled block
        CompiledBlock* block = cache_->get(ctx.pc);
        if (!block) {
            block = compile_block(ctx.pc);
        }
        
        // Call compiled code
        // Signature: void block(ThreadContext* ctx, u8* mem_base)
        using BlockFn = void(*)(ThreadContext*, u8*);
        BlockFn fn = reinterpret_cast<BlockFn>(block->host_code);
        fn(&ctx, memory_->get_fastmem_base());
    }
}
```

---

## Priority Instructions to Compile

Start with these high-frequency instructions:

1. `add`, `addi`, `addis` - Addition
2. `subf`, `subfic` - Subtraction  
3. `mullw`, `mulld` - Multiply
4. `and`, `andi`, `or`, `ori`, `xor` - Logical
5. `slw`, `srw`, `sraw` - Shifts
6. `lwz`, `lbz`, `lhz` - Loads
7. `stw`, `stb`, `sth` - Stores
8. `b`, `bl`, `bc`, `blr` - Branches
9. `cmpw`, `cmplw` - Compares
10. `mfspr`, `mtspr` - Special registers

---

## Test Cases

```cpp
TEST(JitTest, CompileAdd) {
    JitCompiler jit(&memory);
    ThreadContext ctx;
    ctx.pc = 0x10000;
    ctx.gpr[3] = 100;
    ctx.gpr[4] = 50;
    
    // Write: add r5, r3, r4
    memory.write_u32(0x10000, 0x7CA32214);
    memory.write_u32(0x10004, 0x4E800020); // blr
    
    jit.execute(ctx);
    
    EXPECT_EQ(ctx.gpr[5], 150);
}

TEST(JitTest, CompileLwz) {
    JitCompiler jit(&memory);
    ThreadContext ctx;
    ctx.pc = 0x10000;
    ctx.gpr[3] = 0x20000;
    memory.write_u32(0x20008, 0xDEADBEEF);
    
    // lwz r4, 8(r3)
    memory.write_u32(0x10000, 0x80830008);
    
    jit.execute(ctx);
    
    EXPECT_EQ(ctx.gpr[4], 0xDEADBEEF);
}
```

---

## Do NOT Touch

- PowerPC decoder (`decoder.cpp`) - already working
- Interpreter (`interpreter*.cpp`) - fallback, keep working
- GPU code
- Audio code

---

## Success Criteria

1. ✅ ARM64 emitter generates correct opcodes
2. ✅ Block cache stores/retrieves compiled blocks
3. ✅ Simple programs run 10x+ faster than interpreter
4. ✅ At least 30 PowerPC instructions compiled
5. ✅ Fastmem loads/stores work correctly

---

*This task focuses only on JIT code generation. The interpreter remains as fallback for uncompiled instructions.*

