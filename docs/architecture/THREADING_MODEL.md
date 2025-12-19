# 360μ Threading Architecture

## Overview

This document describes the threading architecture for 360μ, an Xbox 360 emulator. The Xbox 360's Xenon processor has 3 cores with 2 hardware threads each (6 total), and games make heavy use of multi-threading with complex synchronization.

## Status: ✅ IMPLEMENTED

As of December 2024, the 1:1 threading model is implemented and working. Call of Duty: Black Ops successfully boots past the initial worker thread initialization sequence.

---

## Architecture: 1:1 Real Threading

Each guest thread gets its own dedicated host thread. This enables real OS-level blocking and waking, which is essential for correct Xbox 360 emulation.

```
┌─────────────────────────────────────────────────────────────┐
│                    Guest Threads                             │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐            │
│  │ GuestThread1│ │ GuestThread2│ │ GuestThread3│ ...        │
│  │ + HostThread│ │ + HostThread│ │ + HostThread│            │
│  │ + Mutex     │ │ + Mutex     │ │ + Mutex     │            │
│  │ + CondVar   │ │ + CondVar   │ │ + CondVar   │            │
│  └──────┬──────┘ └──────┬──────┘ └──────┬──────┘            │
│         │               │               │                    │
│         ▼               ▼               ▼                    │
│  ┌──────────────────────────────────────────────┐           │
│  │           Synchronization Objects             │           │
│  │  XEvent    XSemaphore    XMutex    XTimer    │           │
│  │  (real condition variables + wait lists)      │           │
│  └──────────────────────────────────────────────┘           │
└─────────────────────────────────────────────────────────────┘

Benefits:
- KeWaitForSingleObject actually blocks host thread
- KeSetEvent actually wakes waiting threads
- Proper multi-core parallelism
- No busy-wait polling
```

---

## Key Implementation Details

### Thread-Local Storage (TLS)

In a true multi-threaded system, multiple threads run simultaneously. We use thread-local storage to identify the current guest thread:

```cpp
// Each host thread has its own TLS slot
thread_local GuestThread* g_current_guest_thread = nullptr;

GuestThread* GetCurrentGuestThread() {
    return g_current_guest_thread;
}

void SetCurrentGuestThread(GuestThread* thread) {
    g_current_guest_thread = thread;
}
```

When a host thread starts executing a guest thread, it calls `SetCurrentGuestThread()`. Syscall handlers then call `GetCurrentGuestThread()` to get the correct context.

### Address Translation Consistency

The JIT and HLE code MUST translate addresses identically:

```cpp
// JIT: masks ALL addresses to 29 bits (512MB physical)
emit.AND_imm(addr_reg, addr_reg, 0x1FFFFFFFULL);

// Memory: MUST do the same thing!
GuestAddr Memory::translate_address(GuestAddr addr) {
    return addr & 0x1FFFFFFF;  // Match JIT behavior exactly
}
```

The Xbox 360 address space has multiple virtual mappings (0x00000000, 0x40000000, 0x80000000, etc.) that all map to the same 512MB of physical RAM.

### Context Synchronization

The JIT uses a local copy of the thread context for performance. When a syscall occurs, we must sync:

```cpp
// In Cpu::execute_with_context()
if (cpu_ctx.interrupted) {
    // BEFORE syscall: copy JIT's working context to thread context
    external_ctx = cpu_ctx;
    dispatch_syscall(cpu_ctx);
    // AFTER syscall: copy modified context back to JIT
    cpu_ctx = external_ctx;
}
```

### Syscall PC Advancement

After a `sc` (syscall) instruction, the JIT must advance PC past the instruction:

```cpp
void JitCompiler::compile_syscall(ARM64Emitter& emit, const DecodedInst& inst) {
    // Set interrupted flag
    emit.MOV_imm(arm64::X0, 1);
    emit.STRB(arm64::X0, arm64::CTX_REG, offsetof(ThreadContext, interrupted));

    // CRITICAL: Advance PC past syscall (4 bytes)
    emit.LDR(arm64::X1, arm64::CTX_REG, offsetof(ThreadContext, pc));
    emit.ADD_imm(arm64::X1, arm64::X1, 4);
    emit.STR(arm64::X1, arm64::CTX_REG, offsetof(ThreadContext, pc));

    // Return from block
    emit_block_epilogue(emit, current_block_inst_count_);
}
```

---

## Bugs Fixed (December 2024)

### Bug 1: JIT syscall not advancing PC

- After `sc` instruction, JIT wasn't advancing PC
- Game looped forever on the same syscall
- **Fix**: Added `PC += 4` in `compile_syscall()`

### Bug 2: Context desynchronization

- JIT used local `cpu_ctx`, HLE modified `external_ctx`
- Syscall results weren't propagating back
- **Fix**: Sync contexts before AND after syscall

### Bug 3: Address translation mismatch

- JIT masked with `0x1FFFFFFF`, Memory class didn't always
- HLE writes went to wrong physical addresses
- **Fix**: `translate_address()` always masks

### Bug 4: TLS for thread identification

- `GetCurrentGuestThread()` returned wrong thread
- Syscall handlers used wrong context
- **Fix**: Added `thread_local` TLS

---

## Files

| File                                  | Description                               |
| ------------------------------------- | ----------------------------------------- |
| `native/src/cpu/xenon/threading.h`    | GuestThread structure, TLS declarations   |
| `native/src/cpu/xenon/threading.cpp`  | ThreadScheduler, 1:1 thread creation, TLS |
| `native/src/cpu/xenon/cpu.cpp`        | Context sync in `execute_with_context()`  |
| `native/src/cpu/jit/jit_compiler.cpp` | PC advancement in `compile_syscall()`     |
| `native/src/memory/memory.cpp`        | Consistent address translation            |

---

## Performance Considerations

1:1 threading has trade-offs:

| Pro                                  | Con                              |
| ------------------------------------ | -------------------------------- |
| Real parallelism on multi-core hosts | Thread creation overhead         |
| Proper blocking (no busy-wait)       | More host threads than CPU cores |
| Correct synchronization semantics    | Context switch overhead          |

Modern Android devices (8+ cores) handle this well. The Snapdragon 8 Gen 2+ has enough cores for the Xbox 360's 6 hardware threads plus system overhead.

---

## Reference: Xenia's Approach

Our implementation is inspired by Xenia:

1. **XThread class**: Each guest thread has its own `std::thread`
2. **XEvent class**: Real synchronization with condition variables
3. **KeWaitForSingleObject**: Blocks host thread on condition variable
4. **KeSetEvent**: Signals condition variable, wakes waiters

---

## Testing

Verified with Call of Duty: Black Ops:

- ✅ Boots past XEX loading
- ✅ Creates multiple guest threads
- ✅ Syscalls execute correctly
- ✅ VBlank interrupts fire
- ✅ No infinite syscall loops
