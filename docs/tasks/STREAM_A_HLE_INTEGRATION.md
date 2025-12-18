# Stream A: HLE/Syscall Integration

**Priority**: CRITICAL PATH  
**Estimated Time**: 5 hours  
**Dependencies**: None (can start immediately)  
**Blocks**: Game cannot call ANY kernel functions until complete

## Overview

The emulator has 150+ HLE (High-Level Emulation) functions already implemented in `xboxkrnl.cpp` and `xam.cpp`, but they are never called because:

1. The interpreter doesn't handle the `sc` (syscall) instruction
2. The CPU execute loop doesn't check for syscalls
3. Import thunks don't contain syscall instructions

Your task is to wire these together so games can call kernel functions.

## Files to Modify

- `native/src/cpu/xenon/interpreter.cpp` - Add syscall instruction handling
- `native/src/cpu/xenon/cpu.cpp` - Add syscall dispatch after execution
- `native/src/cpu/xenon/cpu.h` - Add kernel pointer and dispatch method
- `native/src/kernel/kernel.cpp` - Install import thunks at load time
- `native/src/core/emulator.cpp` - Connect kernel to CPU

## Current Architecture

```
CPU Execute → Interpreter → Crash at Import Thunk (no syscall handling)
```

## Target Architecture

```
CPU Execute → Interpreter → Syscall Instruction
                               ↓
                          Set Interrupted Flag
                               ↓
                          CPU checks flag
                               ↓
                          Kernel::handle_syscall()
                               ↓
                          HLE Function executes
                               ↓
                          Resume execution
```

---

## Task A.1: Add Syscall to Interpreter

**File**: `native/src/cpu/xenon/interpreter.cpp`

The PowerPC `sc` (syscall) instruction is opcode 17. Add handling for it:

```cpp
// In execute_one() switch statement, find where instruction types are handled
// Add a case for syscall:

case DecodedInst::Type::Syscall:
    // Signal syscall to the CPU dispatch loop
    ctx.interrupted = true;
    ctx.pc += 4;
    return 1;
```

**Verification**: After this change, the interpreter should recognize `sc` instructions instead of treating them as unknown.

---

## Task A.2: Add Syscall Dispatch to CPU

**File**: `native/src/cpu/xenon/cpu.cpp`

In the `execute_thread()` method, after the interpreter returns, check if a syscall occurred:

```cpp
void Cpu::execute_thread(u32 thread_id, u64 cycles) {
    ThreadContext& ctx = contexts_[thread_id];

    // ... existing interpreter execution code ...

    interpreter_->execute(ctx, cycles);

    // ADD THIS: Check for syscall after execution
    if (ctx.interrupted) {
        ctx.interrupted = false;
        dispatch_syscall(ctx);
    }
}

// ADD THIS: New method to dispatch syscalls
void Cpu::dispatch_syscall(ThreadContext& ctx) {
    // r0 contains: (module_id << 16) | ordinal
    // This encoding is set up by the import thunks (Task A.4)
    u32 ordinal = ctx.gpr[0] & 0xFFFF;
    u32 module = (ctx.gpr[0] >> 16) & 0xFF;

    if (kernel_) {
        kernel_->handle_syscall(ordinal, module);
    }
}
```

---

## Task A.3: Connect Kernel to CPU

**File**: `native/src/cpu/xenon/cpu.h`

Add a kernel pointer so the CPU can dispatch syscalls:

```cpp
// Forward declaration at top of file
class Kernel;

class Cpu {
private:
    // ADD: Kernel pointer for syscall dispatch
    Kernel* kernel_ = nullptr;

public:
    // ADD: Setter for kernel
    void set_kernel(Kernel* kernel) { kernel_ = kernel; }

    // ADD: Syscall dispatch method declaration
    void dispatch_syscall(ThreadContext& ctx);

    // ... existing declarations ...
};
```

**File**: `native/src/core/emulator.cpp`

In the `initialize()` method, connect the kernel to the CPU:

```cpp
bool Emulator::initialize() {
    // ... existing initialization ...

    // ADD: Connect kernel to CPU for syscall dispatch
    cpu_->set_kernel(kernel_.get());

    // ... rest of initialization ...
}
```

---

## Task A.4: Install Import Thunks

**File**: `native/src/kernel/kernel.cpp`

When loading a XEX, the import table contains addresses where the game expects kernel functions to be. We need to write small code stubs (thunks) at these addresses that:

1. Load the function identifier into r0
2. Execute a syscall instruction
3. Return to the caller

```cpp
void Kernel::install_import_thunks(const XexModule& module) {
    u32 module_id = 0;

    for (const auto& lib : module.imports) {
        // Determine module ID based on library name
        if (lib.name == "xboxkrnl.exe") {
            module_id = 0;
        } else if (lib.name == "xam.xex") {
            module_id = 1;
        } else {
            module_id = 2; // Unknown/other
        }

        for (size_t i = 0; i < lib.imports.size(); i++) {
            u32 thunk_addr = lib.imports[i].thunk_address;
            u32 ordinal = lib.imports[i].ordinal;

            // Encode: (module_id << 16) | ordinal
            u32 encoded = (module_id << 16) | ordinal;

            // Write: li r0, encoded (load immediate)
            // For values > 16 bits, may need lis + ori sequence
            if (encoded <= 0x7FFF) {
                // li r0, encoded (addi r0, 0, encoded)
                u32 li_inst = 0x38000000 | (encoded & 0xFFFF);
                memory_->write_u32(thunk_addr, li_inst);
            } else {
                // lis r0, high16
                u32 lis_inst = 0x3C000000 | ((encoded >> 16) & 0xFFFF);
                memory_->write_u32(thunk_addr, lis_inst);
                // ori r0, r0, low16
                u32 ori_inst = 0x60000000 | (encoded & 0xFFFF);
                memory_->write_u32(thunk_addr + 4, ori_inst);
                thunk_addr += 4;
            }

            // Write: sc (syscall instruction)
            memory_->write_u32(thunk_addr + 4, 0x44000002);

            // Write: blr (branch to link register - return)
            memory_->write_u32(thunk_addr + 8, 0x4E800020);
        }
    }
}
```

**Call this method** after loading the XEX in `Kernel::load_xex()` or wherever appropriate.

---

## Testing

Use the existing test tool:

```bash
cd /Users/jaredgrimes/code/360mu/native/build
./test_syscalls /path/to/blackops.iso/default.xex 10000
```

**Success criteria**:

- Game should execute more than 200 instructions
- Should see HLE function calls being logged
- Should not crash at import thunks

## Reference Files

- `native/src/kernel/hle/xboxkrnl.cpp` - See existing HLE implementations
- `native/src/kernel/hle/xam.cpp` - See XAM HLE implementations
- `native/src/kernel/kernel.h` - See `handle_syscall()` signature
- `native/src/cpu/xenon/cpu.h` - See `ThreadContext` structure

## Notes

- The `ThreadContext.interrupted` flag may already exist; check the header
- The encoding scheme (module_id << 16 | ordinal) is arbitrary but must match between thunk installation and dispatch
- Black Ops imports 522 functions, mainly from `xboxkrnl.exe`
