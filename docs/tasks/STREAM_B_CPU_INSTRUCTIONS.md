# Stream B: CPU Instruction Completion

**Priority**: HIGH  
**Estimated Time**: 5 hours  
**Dependencies**: None (can start immediately)  
**Blocks**: Game crashes on unimplemented instructions
**Status**: ✅ COMPLETED

## Implementation Summary

The following instructions have been implemented:

### Floating-Point Load/Store with Update (interpreter.cpp)

- `lfsu` (opcode 49) - Load Floating-Point Single with Update
- `lfdu` (opcode 51) - Load Floating-Point Double with Update
- `stfsu` (opcode 53) - Store Floating-Point Single with Update
- `stfdu` (opcode 55) - Store Floating-Point Double with Update

### Byte-Reverse Operations (interpreter_extended.cpp)

- `stwbrx` (XO31=662) - Store Word Byte-Reverse Indexed
- `sthbrx` (XO31=918) - Store Halfword Byte-Reverse Indexed
- `ldbrx` (XO31=532) - Load Doubleword Byte-Reverse Indexed
- `stdbrx` (XO31=660) - Store Doubleword Byte-Reverse Indexed

### Load Word Algebraic (interpreter_extended.cpp)

- `lwax` (XO31=341) - Load Word Algebraic Indexed
- `lwaux` (XO31=373) - Load Word Algebraic with Update Indexed

### String Operations (interpreter_extended.cpp)

- `lswi` (XO31=597) - Load String Word Immediate
- `lswx` (XO31=533) - Load String Word Indexed
- `stswi` (XO31=661) - Store String Word Immediate
- `stswx` (XO31=725) - Store String Word Indexed

### Bit Manipulation (interpreter_extended.cpp)

- `popcntb` (XO31=122) - Population Count Bytes
- `popcntw` (XO31=378) - Population Count Word
- `popcntd` (XO31=506) - Population Count Doubleword
- `cmpb` (XO31=508) - Compare Bytes

### Time Base (interpreter_extended.cpp)

- `mftb` (XO31=371) - Move From Time Base

### CR Logical Operations (interpreter.cpp)

- `crand` (XO19=257) - CR AND
- `cror` (XO19=449) - CR OR
- `crnand` (XO19=225) - CR NAND
- `crnor` (XO19=33) - CR NOR
- `crxor` (XO19=193) - CR XOR
- `creqv` (XO19=289) - CR Equivalent
- `crandc` (XO19=129) - CR AND with Complement
- `crorc` (XO19=417) - CR OR with Complement
- `mcrf` (XO19=0) - Move CR Field

### Decoder Updates (decoder.cpp)

- Added enum constants for all new XO31 opcodes
- Added decoder cases for new instruction types

---

## Overview

The PowerPC interpreter is missing several instructions that Black Ops uses. When the emulator encounters these, it either crashes or logs `?XX` where XX is the opcode number.

Known missing instructions from test runs:

- `?62` - This is `stfd` (Store Floating-Point Double)
- Various EXT31 extended opcodes

Your task is to implement these missing instructions.

## Files to Modify

- `native/src/cpu/xenon/interpreter.cpp` - Add instruction implementations
- `native/src/cpu/xenon/decoder.cpp` - May need decoder updates for some instructions

## Coordination Note

**Stream A also modifies `interpreter.cpp`** to add syscall handling. You are adding instruction cases, they are adding syscall handling. These are different switch cases and should not conflict, but coordinate when merging.

---

## Task B.1: Floating-Point Load/Store Instructions

### B.1.1: stfd - Store Floating-Point Double (Opcode 62)

**File**: `native/src/cpu/xenon/interpreter.cpp`

The `stfd` instruction stores a 64-bit floating-point value to memory.

**Instruction Format** (D-form):

```
stfd FRS, d(RA)
Opcode: 54 (0x36) for stfd
        62 for stfdu (with update)
```

Wait, let me check - opcode 62 might be `std` (store doubleword) or a floating-point variant. The test showed `?62`. Looking at PowerPC reference:

- Opcode 54 = `stfd` (Store Float Double)
- Opcode 55 = `stfdu` (Store Float Double with Update)
- Opcode 62 = `std` (Store Doubleword) - 64-bit integer store

**Implementation for opcode 62 (std)**:

```cpp
case 62: { // std/stdu - Store Doubleword
    u32 rs = (inst >> 21) & 0x1F;
    u32 ra = (inst >> 16) & 0x1F;
    s16 ds = inst & 0xFFFC;  // Note: bottom 2 bits encode variant
    u32 variant = inst & 0x3;

    GuestAddr ea;
    if (ra == 0) {
        ea = ds;
    } else {
        ea = ctx.gpr[ra] + ds;
    }

    memory_->write_u64(ea, ctx.gpr[rs]);

    // If variant == 1, this is stdu (update ra)
    if (variant == 1 && ra != 0) {
        ctx.gpr[ra] = ea;
    }

    ctx.pc += 4;
    break;
}
```

### B.1.2: lfd - Load Floating-Point Double (Opcode 50)

```cpp
case 50: { // lfd - Load Floating-Point Double
    u32 frd = (inst >> 21) & 0x1F;
    u32 ra = (inst >> 16) & 0x1F;
    s16 d = (s16)(inst & 0xFFFF);

    GuestAddr ea;
    if (ra == 0) {
        ea = d;
    } else {
        ea = ctx.gpr[ra] + d;
    }

    u64 value = memory_->read_u64(ea);
    memcpy(&ctx.fpr[frd], &value, sizeof(u64));

    ctx.pc += 4;
    break;
}
```

### B.1.3: stfd - Store Floating-Point Double (Opcode 54)

```cpp
case 54: { // stfd - Store Floating-Point Double
    u32 frs = (inst >> 21) & 0x1F;
    u32 ra = (inst >> 16) & 0x1F;
    s16 d = (s16)(inst & 0xFFFF);

    GuestAddr ea;
    if (ra == 0) {
        ea = d;
    } else {
        ea = ctx.gpr[ra] + d;
    }

    u64 value;
    memcpy(&value, &ctx.fpr[frs], sizeof(u64));
    memory_->write_u64(ea, value);

    ctx.pc += 4;
    break;
}
```

### B.1.4: lfs/stfs - Single-Precision Float (Opcodes 48, 52)

```cpp
case 48: { // lfs - Load Floating-Point Single
    u32 frd = (inst >> 21) & 0x1F;
    u32 ra = (inst >> 16) & 0x1F;
    s16 d = (s16)(inst & 0xFFFF);

    GuestAddr ea = (ra == 0) ? d : ctx.gpr[ra] + d;

    u32 value = memory_->read_u32(ea);
    float f;
    memcpy(&f, &value, sizeof(float));
    ctx.fpr[frd] = (double)f;  // FPRs are always 64-bit

    ctx.pc += 4;
    break;
}

case 52: { // stfs - Store Floating-Point Single
    u32 frs = (inst >> 21) & 0x1F;
    u32 ra = (inst >> 16) & 0x1F;
    s16 d = (s16)(inst & 0xFFFF);

    GuestAddr ea = (ra == 0) ? d : ctx.gpr[ra] + d;

    float f = (float)ctx.fpr[frs];
    u32 value;
    memcpy(&value, &f, sizeof(float));
    memory_->write_u32(ea, value);

    ctx.pc += 4;
    break;
}
```

---

## Task B.2: EXT31 Extended Opcodes

Opcode 31 is "extended" - the actual operation is determined by bits 1-10 (the XO field).

**File**: `native/src/cpu/xenon/interpreter.cpp`

Find the existing opcode 31 handler and add missing cases:

```cpp
case 31: {
    u32 xo = (inst >> 1) & 0x3FF;  // Extended opcode field
    u32 rs = (inst >> 21) & 0x1F;
    u32 ra = (inst >> 16) & 0x1F;
    u32 rb = (inst >> 11) & 0x1F;
    bool rc = inst & 1;  // Record bit

    switch (xo) {
        // === Register Moves ===

        case 444: { // or (mr is "or rX, rY, rY")
            ctx.gpr[ra] = ctx.gpr[rs] | ctx.gpr[rb];
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 124: { // nor
            ctx.gpr[ra] = ~(ctx.gpr[rs] | ctx.gpr[rb]);
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 28: { // and
            ctx.gpr[ra] = ctx.gpr[rs] & ctx.gpr[rb];
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 60: { // andc (and with complement)
            ctx.gpr[ra] = ctx.gpr[rs] & ~ctx.gpr[rb];
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 284: { // eqv (equivalence)
            ctx.gpr[ra] = ~(ctx.gpr[rs] ^ ctx.gpr[rb]);
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 316: { // xor
            ctx.gpr[ra] = ctx.gpr[rs] ^ ctx.gpr[rb];
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 476: { // nand
            ctx.gpr[ra] = ~(ctx.gpr[rs] & ctx.gpr[rb]);
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 412: { // orc (or with complement)
            ctx.gpr[ra] = ctx.gpr[rs] | ~ctx.gpr[rb];
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        // === Special Purpose Registers ===

        case 339: { // mfspr - Move From Special Purpose Register
            u32 spr = ((inst >> 16) & 0x1F) | ((inst >> 6) & 0x3E0);
            switch (spr) {
                case 1:   ctx.gpr[rs] = ctx.xer; break;    // XER
                case 8:   ctx.gpr[rs] = ctx.lr; break;     // LR
                case 9:   ctx.gpr[rs] = ctx.ctr; break;    // CTR
                case 268: ctx.gpr[rs] = get_timebase(); break; // TBL
                case 269: ctx.gpr[rs] = get_timebase() >> 32; break; // TBU
                default:
                    // Log unknown SPR
                    break;
            }
            break;
        }

        case 467: { // mtspr - Move To Special Purpose Register
            u32 spr = ((inst >> 16) & 0x1F) | ((inst >> 6) & 0x3E0);
            switch (spr) {
                case 1:   ctx.xer = ctx.gpr[rs]; break;    // XER
                case 8:   ctx.lr = ctx.gpr[rs]; break;     // LR
                case 9:   ctx.ctr = ctx.gpr[rs]; break;    // CTR
                default:
                    // Log unknown SPR
                    break;
            }
            break;
        }

        // === Shifts ===

        case 24: { // slw - Shift Left Word
            u32 sh = ctx.gpr[rb] & 0x3F;
            ctx.gpr[ra] = (sh >= 32) ? 0 : (u32)(ctx.gpr[rs] << sh);
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 536: { // srw - Shift Right Word
            u32 sh = ctx.gpr[rb] & 0x3F;
            ctx.gpr[ra] = (sh >= 32) ? 0 : (u32)((u32)ctx.gpr[rs] >> sh);
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 792: { // sraw - Shift Right Algebraic Word
            u32 sh = ctx.gpr[rb] & 0x3F;
            s32 val = (s32)ctx.gpr[rs];
            if (sh >= 32) {
                ctx.gpr[ra] = (val < 0) ? -1 : 0;
                // Set CA if negative and any 1 bits shifted out
                ctx.xer = (ctx.xer & ~0x20000000) | ((val < 0) ? 0x20000000 : 0);
            } else {
                ctx.gpr[ra] = val >> sh;
                // Set CA if negative and any 1 bits shifted out
                u32 mask = (1 << sh) - 1;
                bool ca = (val < 0) && ((val & mask) != 0);
                ctx.xer = (ctx.xer & ~0x20000000) | (ca ? 0x20000000 : 0);
            }
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        // === Byte/Halfword Operations ===

        case 954: { // extsb - Extend Sign Byte
            ctx.gpr[ra] = (s64)(s8)ctx.gpr[rs];
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 922: { // extsh - Extend Sign Halfword
            ctx.gpr[ra] = (s64)(s16)ctx.gpr[rs];
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        case 986: { // extsw - Extend Sign Word
            ctx.gpr[ra] = (s64)(s32)ctx.gpr[rs];
            if (rc) update_cr0(ctx, ctx.gpr[ra]);
            break;
        }

        // === Compare ===

        case 0: { // cmp - Compare
            u32 bf = (inst >> 23) & 0x7;
            s64 a = (s64)ctx.gpr[ra];
            s64 b = (s64)ctx.gpr[rb];
            u32 c;
            if (a < b) c = 0x8;
            else if (a > b) c = 0x4;
            else c = 0x2;
            c |= (ctx.xer >> 31) & 1;  // SO bit
            ctx.cr = (ctx.cr & ~(0xF << (28 - bf * 4))) | (c << (28 - bf * 4));
            break;
        }

        case 32: { // cmpl - Compare Logical (unsigned)
            u32 bf = (inst >> 23) & 0x7;
            u64 a = ctx.gpr[ra];
            u64 b = ctx.gpr[rb];
            u32 c;
            if (a < b) c = 0x8;
            else if (a > b) c = 0x4;
            else c = 0x2;
            c |= (ctx.xer >> 31) & 1;
            ctx.cr = (ctx.cr & ~(0xF << (28 - bf * 4))) | (c << (28 - bf * 4));
            break;
        }

        // === Load/Store with Byte Reversal ===

        case 534: { // lwbrx - Load Word Byte-Reverse Indexed
            GuestAddr ea = (ra == 0) ? ctx.gpr[rb] : ctx.gpr[ra] + ctx.gpr[rb];
            u32 val = memory_->read_u32(ea);
            ctx.gpr[rs] = __builtin_bswap32(val);
            break;
        }

        case 662: { // stwbrx - Store Word Byte-Reverse Indexed
            GuestAddr ea = (ra == 0) ? ctx.gpr[rb] : ctx.gpr[ra] + ctx.gpr[rb];
            memory_->write_u32(ea, __builtin_bswap32((u32)ctx.gpr[rs]));
            break;
        }

        default:
            // Log unhandled XO
            printf("Unhandled EXT31 XO=%d at PC=0x%08X\n", xo, ctx.pc);
            break;
    }

    ctx.pc += 4;
    break;
}
```

---

## Task B.3: Helper Functions

Add these helper functions if they don't exist:

```cpp
// Update CR0 based on result
void update_cr0(ThreadContext& ctx, u64 result) {
    u32 c;
    if ((s64)result < 0) c = 0x8;       // LT
    else if ((s64)result > 0) c = 0x4;  // GT
    else c = 0x2;                        // EQ
    c |= (ctx.xer >> 31) & 1;           // SO
    ctx.cr = (ctx.cr & 0x0FFFFFFF) | (c << 28);
}

// Get timebase counter (for timing)
u64 get_timebase() {
    // Return cycle count or wall time
    return __builtin_readcyclecounter();  // Or use std::chrono
}
```

---

## Testing

```bash
cd /Users/jaredgrimes/code/360mu/native/build
./test_execute /path/to/default.xex 1000
```

**Success criteria**:

- No more `?62` or `?31` output
- Instructions execute without "unhandled" errors
- More instructions complete before any crash

## Reference

- PowerPC Architecture Book - Available online
- `native/src/cpu/xenon/decoder.cpp` - See how instructions are decoded
- `native/src/cpu/jit/jit_compiler.cpp` - JIT has some instruction implementations you can reference

## Notes

- The `ThreadContext` struct defines registers: `gpr[32]`, `fpr[32]`, `cr`, `lr`, `ctr`, `xer`, `pc`
- Xbox 360 uses big-endian PowerPC; memory reads may need byte swapping
- Some instructions have a "record" bit (Rc) that updates CR0 - handle this
- Use `s64`/`u64` for 64-bit operations, `s32`/`u32` for 32-bit
