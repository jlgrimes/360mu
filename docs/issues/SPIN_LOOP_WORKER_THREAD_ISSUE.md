# Critical Issue: Spin Loop Waiting for Worker Thread Completion

## Status: ✅ RESOLVED (December 2024)

The original spin loop issue has been **fixed**. The game now progresses past the worker thread initialization sequence.

---

## Summary

Call of Duty: Black Ops (and likely other games) was getting stuck in a spin loop during early initialization, causing a purple screen. This issue has been resolved through multiple bug fixes in the threading and JIT systems.

---

## Root Causes Found and Fixed

### Bug 1: JIT Syscall Not Advancing PC ✅

- **Problem**: After a `sc` (syscall) instruction, the JIT wasn't advancing PC
- **Symptom**: Game looped forever on the same syscall instruction
- **Fix**: Added `PC += 4` in `JitCompiler::compile_syscall()`
- **File**: `native/src/cpu/jit/jit_compiler.cpp`

### Bug 2: Context Desynchronization ✅

- **Problem**: JIT used a local copy of context (`cpu_ctx`), while HLE modified `external_ctx`
- **Symptom**: Syscall results weren't propagating back to JIT
- **Fix**: Sync contexts before AND after syscall dispatch
- **File**: `native/src/cpu/xenon/cpu.cpp`

```cpp
// BEFORE syscall: sync JIT's working context to thread context
external_ctx = cpu_ctx;
dispatch_syscall(cpu_ctx);
// AFTER syscall: sync modified thread context back to JIT
cpu_ctx = external_ctx;
```

### Bug 3: Address Translation Mismatch ✅

- **Problem**: JIT masked addresses with `0x1FFFFFFF`, but Memory class didn't always apply the same mask
- **Symptom**: HLE writes went to different physical addresses than JIT reads
- **Fix**: Updated `Memory::translate_address()` to always mask with `0x1FFFFFFF`
- **File**: `native/src/memory/memory.cpp`

### Bug 4: TLS for Thread Identification ✅

- **Problem**: In multi-threaded mode, `GetCurrentGuestThread()` needed thread-local storage
- **Symptom**: Syscall handlers couldn't identify which thread was calling
- **Fix**: Added `thread_local GuestThread* g_current_guest_thread`
- **File**: `native/src/cpu/xenon/threading.cpp`

---

## Original Symptoms (Historical)

- Purple screen (GPU test clear color)
- High CPU usage (game spinning in loop)
- Only syscall being made was `KeSetEventBoostPriority` (ordinal 2168)
- GPU never received ring buffer configuration or rendering commands

---

## Architecture Change: 1:1 Threading Model

As part of fixing this issue, the threading model was redesigned:

**Before**: N:M cooperative scheduling (broken)

- Host threads shared guest threads via queue
- `wait_for_object()` returned immediately (no real blocking)
- Worker threads had `entry=0` (no code to run)

**After**: 1:1 real threading (working)

- Each guest thread has its own host `std::thread`
- Real OS-level blocking via `std::condition_variable`
- Thread-local storage for proper thread identification

See: [/docs/architecture/THREADING_MODEL.md](../architecture/THREADING_MODEL.md)

---

## Current State

The original spin loop is fixed. The game now:

1. ✅ Boots past XEX loading
2. ✅ Executes syscalls correctly
3. ✅ Creates and manages multiple threads
4. ✅ Processes VBlank interrupts
5. ✅ Presents GPU frames (purple test color)

**New Blocker**: The game is now stuck in a secondary polling loop at PC `0x825FB308`, waiting for something else. This is a separate issue - the original worker thread spin loop is resolved.

---

## Files Modified

| File                                  | Change                                      |
| ------------------------------------- | ------------------------------------------- |
| `native/src/cpu/jit/jit_compiler.cpp` | Added PC advancement in `compile_syscall()` |
| `native/src/cpu/xenon/cpu.cpp`        | Added context sync before/after syscall     |
| `native/src/memory/memory.cpp`        | Fixed `translate_address()` to always mask  |
| `native/src/cpu/xenon/threading.cpp`  | Added TLS for thread identification         |
| `native/src/cpu/xenon/threading.h`    | Added TLS accessor functions                |

---

## Testing

The fix has been verified with Call of Duty: Black Ops:

```
✅ VBlank #60 (1 second elapsed)
✅ GPU::present() called (frame 1-20+)
✅ No UNIMPLEMENTED syscall errors
✅ No infinite syscall loop at KeSetEventBoostPriority
```

---

## Historical Context

This issue was the primary blocker for weeks. Key insights came from:

1. Researching Xenia's 1:1 threading model
2. Tracing exactly where the game was polling
3. Understanding the relationship between JIT context and HLE context
4. Debugging address translation discrepancies

The journey from "purple screen" to "boots past initialization" involved understanding the fundamental architecture differences between N:M cooperative scheduling and 1:1 real threading.
