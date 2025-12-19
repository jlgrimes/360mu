# 360μ Threading Architecture

## Overview

This document describes the threading architecture for 360μ, an Xbox 360 emulator. The Xbox 360's Xenon processor has 3 cores with 2 hardware threads each (6 total), and games make heavy use of multi-threading with complex synchronization.

## Status: FIXED ✅

As of December 2024, the Call of Duty: Black Ops boot hang has been fixed. The game now advances past the worker thread initialization sequence.

## The Problem: Call of Duty Boot Hang (RESOLVED)

Call of Duty: Black Ops was hanging during initialization with a purple screen. Analysis revealed:

1. The game immediately enters a spin loop calling `KeSetEventBoostPriority`
2. It polls a completion flag at `r31 + 0x14C` waiting for it to change
3. The game expects **kernel worker threads** to process work and set the flag
4. Multiple bugs were preventing progress

### Root Causes Found and Fixed

**Bug 1: JIT syscall not advancing PC**

- After a `sc` (syscall) instruction, the JIT wasn't advancing PC
- This caused the game to loop forever on the same syscall instruction
- **Fix**: Added PC += 4 in `compile_syscall()`

**Bug 2: Context desynchronization**

- JIT used a local copy of context (`cpu_ctx`), while HLE modified `external_ctx`
- Syscall results weren't propagating back to JIT
- **Fix**: Sync `external_ctx = cpu_ctx` BEFORE syscall, `cpu_ctx = external_ctx` AFTER

**Bug 3: Address translation mismatch**

- JIT masked all addresses with `0x1FFFFFFF` (512MB physical)
- Memory class wasn't applying the same mask consistently
- **Fix**: Updated `Memory::translate_address()` to always mask with `0x1FFFFFFF`

**Bug 4: TLS for thread identification**

- In multi-threaded mode, `GetCurrentGuestThread()` needed thread-local storage
- Without TLS, syscall handlers couldn't identify which thread was calling
- **Fix**: Added `thread_local GuestThread* g_current_guest_thread`

## Architecture Comparison

### Previous Architecture (N:M Cooperative)

```
┌─────────────────────────────────────────────────────────┐
│                    ThreadScheduler                       │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐        │
│  │HostThread 0 │ │HostThread 1 │ │HostThread 2 │ ...    │
│  │  (std::thread)│  (std::thread)│  (std::thread)│        │
│  └──────┬──────┘ └──────┬──────┘ └──────┬──────┘        │
│         │               │               │                │
│         ▼               ▼               ▼                │
│  ┌──────────────────────────────────────────────┐       │
│  │           Ready Queue (cooperative)           │       │
│  │  [Guest1] [Guest2] [Guest3] [Guest4] ...     │       │
│  └──────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────┘

Problems:
- wait_for_object() returns immediately (no real blocking)
- signal_object() sets memory flag (no real wake)
- Worker threads have no code to execute
- Spin-waits consume CPU without progress
```

### New Architecture (1:1 Real Threading)

```
┌─────────────────────────────────────────────────────────┐
│                    Guest Threads                         │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐        │
│  │ GuestThread1│ │ GuestThread2│ │ GuestThread3│ ...    │
│  │ + HostThread│ │ + HostThread│ │ + HostThread│        │
│  │ + CondVar   │ │ + CondVar   │ │ + CondVar   │        │
│  └──────┬──────┘ └──────┬──────┘ └──────┬──────┘        │
│         │               │               │                │
│         ▼               ▼               ▼                │
│  ┌──────────────────────────────────────────────┐       │
│  │           Synchronization Objects             │       │
│  │  XEvent    XSemaphore    XMutex    XTimer    │       │
│  │  (real condition variables + wait lists)      │       │
│  └──────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────┘

Benefits:
- KeWaitForSingleObject actually blocks host thread
- KeSetEvent actually wakes waiting threads
- ExQueueWorkItem spawns real thread for work
- Proper multi-core parallelism
```

## How Xenia Does It (Reference Implementation)

Xenia uses the 1:1 model:

1. **XThread class**: Each guest thread is an `XThread` with its own `std::thread`
2. **XEvent class**: Real synchronization object with condition variable
3. **KeWaitForSingleObject**: Blocks host thread on condition variable
4. **KeSetEvent**: Signals condition variable, wakes waiters
5. **ExQueueWorkItem**: Creates NEW thread per work item (no thread pool)

Key code pattern from Xenia:

```cpp
// XEvent::Set() - signal the event
void XEvent::Set() {
  std::lock_guard<std::mutex> lock(mutex_);
  signal_state_ = true;
  if (manual_reset_) {
    cv_.notify_all();  // Wake all waiters
  } else {
    cv_.notify_one();  // Wake one waiter (auto-reset)
  }
}

// XThread::Wait() - block until signaled
uint32_t XThread::Wait(XObject* object, uint64_t timeout) {
  std::unique_lock<std::mutex> lock(object->mutex_);
  while (!object->signaled()) {
    if (timeout == 0) return STATUS_TIMEOUT;
    object->cv_.wait(lock);
  }
  return STATUS_SUCCESS;
}
```

## Implementation Plan

### Phase 1: GuestThread Enhancement

Each `GuestThread` gets:

- `std::thread host_thread` - dedicated host thread
- `std::mutex mutex` - for synchronization
- `std::condition_variable wait_cv` - for blocking waits
- `bool running` - thread lifecycle control

### Phase 2: Synchronization Objects

Create proper sync objects with real blocking:

- `XEvent` - manual/auto reset events with condition variables
- `XSemaphore` - counting semaphore
- `XMutex` - mutual exclusion
- Map guest addresses to these objects

### Phase 3: Kernel Functions

Implement real blocking/waking:

- `KeWaitForSingleObject` - block on condition variable
- `KeWaitForMultipleObjects` - wait on multiple objects
- `KeSetEvent` / `KeSetEventBoostPriority` - signal and wake
- `ExQueueWorkItem` - spawn new thread per work item

### Phase 4: Thread Lifecycle

- `ExCreateThread` - create new GuestThread with host thread
- Thread termination and cleanup
- Proper shutdown sequence

## Memory Model Considerations

The Xbox 360 uses a weakly-ordered memory model. With 1:1 threading:

- Each host thread directly accesses guest memory
- Memory barriers may be needed for cross-thread visibility
- JIT code runs directly on host threads

## Performance Considerations

1:1 threading has trade-offs:

- **Pro**: Real parallelism on multi-core hosts
- **Pro**: Proper blocking (no busy-wait)
- **Con**: Thread creation overhead
- **Con**: More host threads than CPU cores

Mitigations:

- Thread pooling for ExQueueWorkItem (optional)
- Affinity hints to OS scheduler
- Yield hints in spin-waits

## Files to Modify

1. `native/src/cpu/xenon/threading.h` - GuestThread structure
2. `native/src/cpu/xenon/threading.cpp` - ThreadScheduler implementation
3. `native/src/kernel/hle/xboxkrnl_threading.cpp` - HLE functions
4. `native/src/kernel/hle/xboxkrnl_extended.cpp` - Extended functions
5. `native/src/kernel/kernel.cpp` - Thread creation

## Critical Implementation Details

### Thread-Local Storage (TLS) for Thread Identification

In a true multi-threaded system, there is no single "current thread" - multiple threads run simultaneously. Xenia solves this with **thread-local storage**:

```cpp
// Each host thread has its own TLS slot
thread_local GuestThread* g_current_guest_thread = nullptr;

GuestThread* GetCurrentGuestThread() {
    return g_current_guest_thread;
}

// Set when 1:1 host thread starts
void SetCurrentGuestThread(GuestThread* thread) {
    g_current_guest_thread = thread;
}
```

When a syscall happens:

1. The host thread executing the syscall calls `GetCurrentGuestThread()`
2. Returns the `GuestThread*` associated with THIS host thread
3. Syscall handler uses the correct `ThreadContext`

**Do NOT** use a global "current thread ID" or "active thread" variable - this is a race condition in multi-threaded code.

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

If translation differs:

- JIT reads from physical `0x1003FC3C`
- HLE writes to virtual `0x7003FC3C` (different location!)
- Completion flags never seen by game code

The Xbox 360 address space has multiple mappings (0x00000000, 0x40000000, 0x80000000, etc.) that all map to the same 512MB of physical RAM.

## Testing

After implementation, verify:

1. Call of Duty: Black Ops boots past purple screen ✓
2. Multi-threaded games run correctly
3. No deadlocks or race conditions
4. Performance is acceptable on target hardware (Android)
