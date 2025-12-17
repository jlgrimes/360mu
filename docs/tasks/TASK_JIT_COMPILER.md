# Task: JIT Compiler Implementation

## Project Context
You are working on 360μ, an Xbox 360 emulator for Android. The CPU is a PowerPC-based IBM Xenon (3 cores, 6 threads). You need to implement a JIT compiler that translates PowerPC to ARM64.

## Your Assignment
Complete the JIT compiler that dynamically recompiles PowerPC code to ARM64 native code for high performance.

## Current State
- JIT header exists at `native/src/cpu/jit/jit.h` (500+ lines of interface)
- ARM64 emitter skeleton at `native/src/cpu/jit/arm64_emitter.cpp`
- JIT compiler skeleton at `native/src/cpu/jit/jit_compiler.cpp`
- Interpreter works (reference for instruction semantics)

## Files to Implement

### 1. `native/src/cpu/jit/arm64_emitter.cpp`
```cpp
// ARM64 code generation:
class Arm64Emitter {
    // Emit ARM64 instructions to buffer
    void emit_mov_imm(ArmReg dst, u64 imm);
    void emit_add(ArmReg dst, ArmReg lhs, ArmReg rhs);
    void emit_sub(ArmReg dst, ArmReg lhs, ArmReg rhs);
    void emit_mul(ArmReg dst, ArmReg lhs, ArmReg rhs);
    void emit_ldr(ArmReg dst, ArmReg base, s32 offset);
    void emit_str(ArmReg src, ArmReg base, s32 offset);
    void emit_branch(void* target);
    void emit_branch_cond(Condition cond, void* target);
    
    // NEON for VMX128
    void emit_neon_add_f32x4(NeonReg dst, NeonReg a, NeonReg b);
    void emit_neon_mul_f32x4(NeonReg dst, NeonReg a, NeonReg b);
    // etc...
};
```

### 2. `native/src/cpu/jit/jit_compiler.cpp`
```cpp
// Main JIT compiler:
class JitCompiler {
    // Compile a basic block of PPC code
    CompiledBlock* compile_block(GuestAddr start);
    
    // Instruction translation (PPC → ARM64)
    void translate_add(const DecodedInst& inst);
    void translate_lwz(const DecodedInst& inst);
    void translate_branch(const DecodedInst& inst);
    void translate_vmx(const DecodedInst& inst);
    
    // Register allocation
    ArmReg allocate_gpr(u32 ppc_reg);
    NeonReg allocate_vr(u32 ppc_vr);
    void spill_register(ArmReg reg);
};
```

### 3. `native/src/cpu/jit/block_cache.cpp`
```cpp
// JIT code cache management:
class BlockCache {
    // Find compiled block for address
    CompiledBlock* lookup(GuestAddr addr);
    
    // Store compiled block
    void insert(GuestAddr addr, CompiledBlock* block);
    
    // Invalidate on SMC (self-modifying code)
    void invalidate(GuestAddr start, u64 size);
};
```

## Key Translation Mappings

### Register Mapping (PPC → ARM64)
```
PPC GPR0-31  → ARM64 X0-X30 (X31=SP reserved)
PPC LR      → Store in memory, load for blr
PPC CTR     → Store in memory
PPC CR      → ARM64 NZCV flags (partial)
PPC XER     → Store in memory

PPC VR0-127 → ARM64 V0-V31 (spill rest to memory)
```

### Instruction Examples
```
PPC: add r3, r4, r5
ARM64: add x3, x4, x5

PPC: lwz r3, 0x10(r4)  
ARM64: ldr w3, [x4, #0x10]
       rev w3, w3  // byte swap for big-endian

PPC: vaddfp v3, v4, v5
ARM64: fadd v3.4s, v4.4s, v5.4s
```

## Build & Test
```bash
cd native/build
cmake .. -DX360MU_ENABLE_JIT=ON
make -j4

# Run JIT tests:
./x360mu_tests --gtest_filter=JIT*
```

## Reference
- Interpreter at `native/src/cpu/xenon/interpreter.cpp` (reference semantics)
- PowerPC ISA manual
- ARM64 instruction reference
- Xenia JIT: https://github.com/xenia-project/xenia/tree/master/src/xenia/cpu/backend

## Success Criteria
1. Can compile simple blocks (add, load, store, branch)
2. 10x+ speedup over interpreter on microbenchmarks
3. Block cache with invalidation works
4. VMX128 basic ops (vaddfp, vmulfp) working

