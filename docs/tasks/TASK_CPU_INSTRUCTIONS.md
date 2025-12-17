# Task: CPU Instruction Coverage

**STATUS: ✅ COMPLETED**

## Project Context

You are working on 360μ, an Xbox 360 emulator for Android. The Xbox 360 uses a PowerPC-based CPU (IBM Xenon) with VMX128 SIMD extensions.

## Your Assignment

Expand CPU instruction coverage to handle all instructions Black Ops uses.

## Current State

- Decoder at `native/src/cpu/xenon/decoder.cpp` ✅
- Interpreter at `native/src/cpu/xenon/interpreter.cpp` ✅
- Extended interpreter at `native/src/cpu/xenon/interpreter_extended.cpp` ✅
- VMX128 at `native/src/cpu/vmx128/vmx.cpp` ✅
- Comprehensive tests at `native/tests/cpu/test_interpreter.cpp` and `test_vmx128.cpp` ✅

## Implemented Instructions

## Instructions to Implement

### 1. 64-bit Integer Operations (`interpreter_extended.cpp`)

```cpp
// Opcode 31 extended:
case 9:   // mulhdu - Multiply High Doubleword Unsigned
case 73:  // mulhd - Multiply High Doubleword
case 233: // mulld - Multiply Low Doubleword (exists, verify)
case 457: // divdu - Divide Doubleword Unsigned
case 489: // divd - Divide Doubleword

// 64-bit shifts (opcode 31):
case 27:  // sld - Shift Left Doubleword
case 539: // srd - Shift Right Doubleword
case 794: // srad - Shift Right Algebraic Dword
case 826: // sradi - Shift Right Algebraic Dword Immediate

// 64-bit compare:
case 0:   // cmp (verify L=1 for 64-bit)
case 32:  // cmpl (verify L=1)
```

### 2. Rotate/Mask Operations (`interpreter_extended.cpp`)

```cpp
// Opcode 30 (64-bit rotate):
void exec_rotate64(ThreadContext& ctx, const DecodedInst& d) {
    switch (d.xo & 0xF) {
        case 0: // rldicl - Rotate Left Dword Imm then Clear Left
        case 1: // rldicr - Rotate Left Dword Imm then Clear Right
        case 2: // rldic  - Rotate Left Dword Imm then Clear
        case 3: // rldimi - Rotate Left Dword Imm then Mask Insert
        case 8: // rldcl  - Rotate Left Dword then Clear Left
        case 9: // rldcr  - Rotate Left Dword then Clear Right
    }
}
```

### 3. Load/Store with Update (`interpreter.cpp`)

```cpp
// Already have basic load/store, add:
case 31: // Extended load/store (by xo):
    // xo 23:  lwzx   - Load Word Zero Indexed
    // xo 55:  lwzux  - Load Word Zero with Update Indexed
    // xo 87:  lbzx   - Load Byte Zero Indexed
    // xo 119: lbzux
    // xo 279: lhzx
    // xo 311: lhzux
    // xo 343: lhax   - Load Halfword Algebraic Indexed
    // xo 375: lhaux
    // xo 21:  ldx    - Load Doubleword Indexed
    // xo 53:  ldux

    // Stores:
    // xo 151: stwx
    // xo 183: stwux
    // xo 215: stbx
    // xo 247: stbux
    // xo 407: sthx
    // xo 439: sthux
    // xo 149: stdx
    // xo 181: stdux
```

### 4. Atomic Operations (`interpreter_extended.cpp`)

```cpp
// Load-reserved / Store-conditional:
case 20:  // lwarx - Load Word and Reserve Indexed
    // Set reservation on address
    result = memory->read_u32(addr);
    reservation_addr = addr;
    reservation_valid = true;

case 150: // stwcx. - Store Word Conditional Indexed
    // Check reservation, store if valid
    if (reservation_valid && reservation_addr == addr) {
        memory->write_u32(addr, value);
        ctx.cr[0].eq = true;  // Success
        reservation_valid = false;
    } else {
        ctx.cr[0].eq = false; // Failed
    }

case 84:  // ldarx - Load Doubleword and Reserve
case 214: // stdcx. - Store Doubleword Conditional
```

### 5. Floating Point Complete (`interpreter.cpp`)

```cpp
// Opcode 63 (extended float):
void exec_float_complete(ThreadContext& ctx, const DecodedInst& d) {
    switch (d.xo) {
        case 12:  // frsp - Round to Single
        case 14:  // fctiw - Convert to Int Word
        case 15:  // fctiwz - Convert to Int Word with Round to Zero
        case 18:  // fdiv
        case 20:  // fsub
        case 21:  // fadd
        case 22:  // fsqrt
        case 24:  // fres - Reciprocal Estimate
        case 25:  // fmul
        case 26:  // frsqrte - Reciprocal Square Root Estimate
        case 28:  // fmsub
        case 29:  // fmadd
        case 30:  // fnmsub
        case 31:  // fnmadd
        case 32:  // fcmpo - Compare Ordered
        case 40:  // fneg
        case 72:  // fmr - Move Register
        case 136: // fnabs
        case 264: // fabs
        case 814: // fctid - Convert to Int Dword
        case 815: // fctidz
        case 846: // fcfid - Convert from Int Dword
    }
}
```

### 6. VMX128 Complete (`vmx.cpp`)

```cpp
// Xbox 360 specific VMX instructions:
void exec_vmx128(ThreadContext& ctx, const Vmx128Inst& d) {
    // Pack/Unpack 128-bit
    case VPack128:
    case VUnpack128:

    // Dot products
    case VDot3:  // 3-component dot
    case VDot4:  // 4-component dot

    // Cross product
    case VCross3:

    // Matrix ops (software)
    case VMtx44Mul:  // 4x4 matrix multiply
    case VMtxTrn:    // Transpose

    // Shuffle/swizzle
    case VShufD:     // Shuffle dwords
    case VPerm128:   // Full permute with control
}
```

## Testing Strategy

```cpp
// Add tests for each instruction:
TEST_F(InterpreterTest, Mulld) {
    ctx.gpr[3] = 0x100000000ULL;
    ctx.gpr[4] = 0x100000000ULL;
    execute_instruction(/*mulld r5, r3, r4*/);
    EXPECT_EQ(ctx.gpr[5], 0);  // Overflow, low 64 bits
}

TEST_F(InterpreterTest, Lwarx_Stwcx) {
    memory->write_u32(0x1000, 42);
    execute_instruction(/*lwarx r3, 0, r4*/);  // r4 = 0x1000
    EXPECT_EQ(ctx.gpr[3], 42);
    ctx.gpr[5] = 100;
    execute_instruction(/*stwcx. r5, 0, r4*/);
    EXPECT_TRUE(ctx.cr[0].eq);  // Should succeed
    EXPECT_EQ(memory->read_u32(0x1000), 100);
}
```

## Build & Test

```bash
cd native/build
cmake ..
make -j4
./x360mu_tests --gtest_filter=Interpreter*
./x360mu_tests --gtest_filter=VMX*
```

## Reference

- PowerPC ISA v2.06 (search online)
- Xenia CPU: https://github.com/xenia-project/xenia/tree/master/src/xenia/cpu/ppc
- `native/src/cpu/xenon/interpreter.cpp` for existing patterns

## Success Criteria

1. ✅ All integer/float/VMX instruction groups complete
2. ✅ Atomic operations (lwarx/stwcx/ldarx/stdcx) working
3. ✅ 64-bit operations correct (mulld, mulhd, mulhdu, divd, divdu, sld, srd, srad, sradi)
4. ✅ All rotate/mask instructions implemented (rldicl, rldicr, rldic, rldimi, rldcl, rldcr)
5. ✅ Test coverage for critical paths
6. ✅ VMX128 extensions: dot products, cross product, shuffle, matrix ops
