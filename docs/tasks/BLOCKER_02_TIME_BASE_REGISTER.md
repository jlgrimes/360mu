# Task: Implement Time Base Register

## Priority: HIGH

## Problem

The `mftb` (Move From Time Base) instruction in `native/src/cpu/xenon/interpreter.cpp` returns 0, causing games to hang in timing loops.

Current broken code at line 854:

```cpp
ctx.gpr[d.rd] = 0; // TODO: implement time base
```

Games use the time base register for:

- Frame timing and delays
- Animation systems
- Physics calculations
- Random number seeding

Returning 0 causes infinite loops when games wait for time to pass.

## Solution

Implement a monotonically increasing counter that approximates the Xbox 360's time base frequency (~50 MHz).

## Implementation Steps

### Step 1: Add time_base field to ThreadContext

In `native/src/cpu/xenon/cpu.h`, add to the ThreadContext struct (around line 50-80):

```cpp
struct ThreadContext {
    // ... existing fields like gpr, fpr, cr, etc. ...

    // Time base register - increments with executed instructions
    // Xbox 360 time base runs at ~50MHz (bus clock / 8)
    u64 time_base = 0;

    // ... rest of existing fields ...
};
```

### Step 2: Fix mftb instruction in interpreter

In `native/src/cpu/xenon/interpreter.cpp`, find the `mftb` handler (around line 850-860) and replace:

```cpp
case 371: {  // mftb - Move From Time Base
    // The time base is a 64-bit counter
    // TBR field determines which half to read:
    // TBR=268 (0x10C) = TBL (lower 32 bits)
    // TBR=269 (0x10D) = TBU (upper 32 bits)
    u32 tbr = ((inst >> 16) & 0x1F) | ((inst >> 6) & 0x3E0);

    // Increment time base by approximate cycles for this block
    // This gives rough timing even in interpreter mode
    ctx.time_base += 32;  // ~32 cycles per instruction average

    if (tbr == 268 || tbr == 284) {
        // TBL - Time Base Lower
        ctx.gpr[d.rd] = static_cast<u32>(ctx.time_base);
    } else if (tbr == 269 || tbr == 285) {
        // TBU - Time Base Upper
        ctx.gpr[d.rd] = static_cast<u32>(ctx.time_base >> 32);
    } else {
        // Full 64-bit read (some games use this)
        ctx.gpr[d.rd] = ctx.time_base;
    }
    break;
}
```

### Step 3: Add time base increment to interpreter execute loop

In the main interpreter execute loop in `native/src/cpu/xenon/interpreter.cpp` (the `execute` or `step` method), add time base increment:

Find the instruction execution loop and add after each instruction executes:

```cpp
// After executing each instruction
ctx.time_base += cycles_for_instruction;  // Or just use a constant like 4
```

If there's a `cycles` counter being tracked, use that. Otherwise, a constant increment per instruction is fine.

### Step 4: Add time base increment to JIT compiler

In `native/src/cpu/jit/jit_compiler.cpp`, the JIT needs to increment time_base too.

Find where the JIT emits code for block execution (likely in `compile_block` or `execute`), and add ARM64 code to increment the time_base:

```cpp
// In the JIT block prologue or epilogue, add:
// Load time_base address
// Add instruction count * cycles
// Store back

// Example ARM64 emission (adjust to your emitter API):
void JitCompiler::emit_time_base_update(u32 instruction_count) {
    // Assuming emit_ is your ARM64 emitter and ctx_reg holds context pointer

    // Calculate offset of time_base in ThreadContext
    constexpr size_t time_base_offset = offsetof(ThreadContext, time_base);

    // ldr x0, [ctx_reg, #time_base_offset]
    emit_.ldr(X0, ctx_reg_, time_base_offset);

    // add x0, x0, #(instruction_count * 4)
    emit_.add(X0, X0, instruction_count * 4);

    // str x0, [ctx_reg, #time_base_offset]
    emit_.str(X0, ctx_reg_, time_base_offset);
}
```

Call this at the end of each compiled block with the block's instruction count.

### Step 5: Handle mftbu and mftbl variants

Some games use the split reads. Make sure both are handled. In the interpreter, the TBR field encoding is:

- TBR = 268 (0x10C): TBL (Time Base Lower)
- TBR = 269 (0x10D): TBU (Time Base Upper)

Also handle the alternate encodings 284/285 which some assemblers use.

## Testing

Add a unit test in `native/tests/cpu/test_interpreter.cpp`:

```cpp
TEST(Interpreter, TimeBase) {
    // Setup
    Memory mem;
    mem.initialize();
    Interpreter interp(&mem);
    ThreadContext ctx;
    ctx.reset();

    // Execute mftb instruction
    // mftb r3 = 0x7C6C42E6
    u32 mftb_inst = 0x7C6C42E6;
    u64 tb1 = ctx.time_base;

    // Execute some instructions
    for (int i = 0; i < 100; i++) {
        interp.step(ctx);
    }

    u64 tb2 = ctx.time_base;

    // Time base should have increased
    EXPECT_GT(tb2, tb1);
    EXPECT_GT(tb2 - tb1, 0);
}
```

## Files to Modify

- `native/src/cpu/xenon/cpu.h` - Add time_base field to ThreadContext
- `native/src/cpu/xenon/interpreter.cpp` - Implement mftb, increment time_base
- `native/src/cpu/jit/jit_compiler.cpp` - Add time_base tracking to JIT
- `native/tests/cpu/test_interpreter.cpp` - Add unit test (optional)

## Dependencies

None - this task is independent of other blockers.

## Notes

- The Xbox 360's actual time base runs at ~50 MHz
- We approximate this by incrementing based on instruction count
- Exact cycle-accurate timing isn't required, just monotonic increase
- The JIT implementation is optional but recommended for performance
