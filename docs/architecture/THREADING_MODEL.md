# 360μ Threading Architecture

## Overview

This document describes the threading architecture for 360μ, an Xbox 360 emulator. The Xbox 360's Xenon processor has 3 cores with 2 hardware threads each (6 total), and games make heavy use of multi-threading with complex synchronization.

## The Problem: Call of Duty Boot Hang

Call of Duty: Black Ops hangs during initialization with a purple screen. Analysis revealed:

1. The game immediately enters a spin loop calling `KeSetEventBoostPriority`
2. It polls a completion flag at `r31 + 0x14C` waiting for it to change
3. The game expects **kernel worker threads** to process work and set the flag
4. Our worker threads had no code to execute (`entry = 0`)

### Root Cause Analysis

The game uses a custom work queue pattern (NOT `ExQueueWorkItem`):

```
1. Game creates work request structure on stack
2. Event object at structure + 0x50
3. Completion flag at structure + 0x14C
4. Game calls KeSetEventBoostPriority(event) to "wake workers"
5. Workers should process work and set completion flag
6. Game spin-polls completion flag until non-zero
```

The fundamental issue: our architecture didn't support **real thread blocking and waking**.

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

## Testing

After implementation, verify:

1. Call of Duty: Black Ops boots past purple screen
2. Multi-threaded games run correctly
3. No deadlocks or race conditions
4. Performance is acceptable on target hardware (Android)
