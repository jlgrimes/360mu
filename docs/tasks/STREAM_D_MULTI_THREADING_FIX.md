# Stream D: Proper Multi-Threaded Guest Execution

## Priority: CRITICAL

## Status: Completed

## Blocking Issue: Purple screen / No GPU initialization

---

## Problem Summary

The emulator shows a **purple screen** because the game never reaches GPU initialization. The game is stuck in initialization spin loops waiting for kernel system threads to process work - but our DPC (Deferred Procedure Call) processing system exists but is **never called**.

### Root Cause

Two parallel threading systems exist that aren't connected:

1. **ThreadScheduler** (`native/src/cpu/xenon/threading.cpp`) - Used by the main emulator loop
2. **XKernel/XScheduler** (`native/src/kernel/xkernel.cpp`) - Has DPC/timer/APC processing but is **never invoked**

The critical function `XKernel::run_for()` which calls `process_dpcs()` is defined but **never called** from the main emulator loop.

### Evidence from Debugging

- Only 2-3 syscalls ever get called: `KeSetEventBoostPriority` (2168), `ObReferenceObjectByHandle` (2144)
- `ExCreateThread` (ordinal 14) is **never called** - the game expects pre-existing system threads
- GPU ring buffer remains at `rb_base:0` throughout all runs
- Game spins at various PCs (0x82550920, etc.) waiting for events that never get signaled by worker threads

---

## Architecture Diagram

```
CURRENT BROKEN FLOW:
====================
Emulator Loop
    |
    v
ThreadScheduler.run() --> Execute Guest Threads

KeInsertQueueDpc() --> DPC Queue --> [NEVER PROCESSED]


REQUIRED FIXED FLOW:
====================
Emulator Loop
    |
    +---> ThreadScheduler.run() --> Execute Guest Threads
    |
    +---> XKernel.run_for()
              |
              +---> process_dpcs() --> Execute DPC Routines --> Signal Completion
              |
              +---> process_timers()
              |
              +---> process_apcs()
```

---

## Implementation Tasks

### Task 1: Integrate XKernel Processing into Main Loop

**File:** `native/src/core/emulator.cpp`
**Location:** Around line 434 in the main emulation loop

**Current Code:**

```cpp
while (!frame_complete && !emu_thread_->paused && !emu_thread_->should_stop) {
    scheduler_->run(cpu::CLOCK_SPEED / 60 / 100);

    gpu_->process_commands();
    // ...
}
```

**Required Change:**

```cpp
#include "../kernel/xkernel.h"  // Add at top of file

while (!frame_complete && !emu_thread_->paused && !emu_thread_->should_stop) {
    scheduler_->run(cpu::CLOCK_SPEED / 60 / 100);

    // Process kernel work items (DPCs, timers, APCs)
    XKernel::instance().run_for(cpu::CLOCK_SPEED / 60 / 100);

    gpu_->process_commands();
    // ...
}
```

---

### Task 2: Implement Actual DPC Execution

**File:** `native/src/kernel/xobject.cpp`
**Location:** `KernelState::process_dpcs()` function (around line 221)

**Current Code (does nothing):**

```cpp
void KernelState::process_dpcs() {
    std::vector<DpcEntry> to_process;
    {
        std::lock_guard<std::mutex> lock(dpc_mutex_);
        to_process.swap(dpc_queue_);
    }

    for (const auto& dpc : to_process) {
        // In a full implementation, we'd call the DPC routine
        // For now, we just log it
        LOGD("Processing DPC: routine=0x%08X, context=0x%08X",
             dpc.routine, dpc.context);

        // TODO: Actually execute the DPC routine
        // This requires calling back into the CPU emulator
    }
}
```

**Required Change:**

```cpp
void KernelState::process_dpcs() {
    std::vector<DpcEntry> to_process;
    {
        std::lock_guard<std::mutex> lock(dpc_mutex_);
        to_process.swap(dpc_queue_);
    }

    if (to_process.empty()) return;

    LOGI("Processing %zu DPCs", to_process.size());

    for (const auto& dpc : to_process) {
        if (dpc.routine == 0) continue;

        LOGD("Executing DPC: routine=0x%08X, context=0x%08X",
             dpc.routine, dpc.context);

        // Execute DPC by running the guest routine
        // DPC routines run at DISPATCH_LEVEL, synchronously
        // Need to get CPU instance and execute
        if (cpu_) {
            ThreadContext ctx;
            ctx.reset();
            ctx.pc = dpc.routine;
            ctx.gpr[3] = dpc.context;   // Context in r3
            ctx.gpr[4] = dpc.arg1;      // SystemArgument1 in r4
            ctx.gpr[5] = dpc.arg2;      // SystemArgument2 in r5
            ctx.lr = 0;                  // Return terminates
            ctx.running = true;
            ctx.memory = memory_;

            // Execute for limited cycles (DPCs should be quick)
            cpu_->execute_block(ctx, 10000);
        }
    }
}
```

**Note:** You may need to:

1. Add `Cpu* cpu_` member to `KernelState` class
2. Set it during initialization
3. Include appropriate headers

---

### Task 3: Store DPC Arguments Properly

**File:** `native/src/kernel/hle/xboxkrnl_extended.cpp`
**Location:** `HLE_KeInsertQueueDpc` function (around line 1072)

The DpcEntry struct and queue insertion need to capture all arguments:

**Check the DpcEntry struct in `xboxkrnl_extended.cpp` (around line 118):**

```cpp
struct DpcEntry {
    GuestAddr routine;
    GuestAddr context;
    GuestAddr arg1;
    GuestAddr arg2;
};
```

**Ensure the queue insertion captures all fields (around line 1083):**

```cpp
g_ext_hle.dpc_queue.push_back({
    memory->read_u32(dpc + 12),  // DeferredRoutine (offset 0x0C)
    memory->read_u32(dpc + 16),  // DeferredContext (offset 0x10)
    arg1,                         // SystemArgument1
    arg2                          // SystemArgument2
});
```

**Also update the DpcEntry in `xobject.h` to match:**

```cpp
struct DpcEntry {
    GuestAddr routine;
    GuestAddr context;
    GuestAddr arg1;
    GuestAddr arg2;
};
```

---

### Task 4: Trigger DPC Processing on Event Signal

**File:** `native/src/kernel/hle/xboxkrnl_extended.cpp`
**Location:** Inside the `KeSetEventBoostPriority` handler (ordinal 2168)

**Add after setting the event signal state:**

```cpp
// Set event to signaled state
memory->write_u32(event + 4, 1);

// Immediately process any pending DPCs
// This simulates a system thread responding to the event
KernelState::instance().process_dpcs();

// Or if using XKernel:
// XKernel::instance().process_dpcs();
```

---

### Task 5: Fix System Thread Creation (Optional Enhancement)

**File:** `native/src/kernel/kernel.cpp`
**Location:** `Kernel::create_system_guest_threads()` function

Instead of creating threads with idle loops, make them proper DPC processors:

```cpp
void Kernel::create_system_guest_threads() {
    if (!scheduler_) return;

    // Create system worker threads that process DPCs
    for (int i = 0; i < 3; i++) {
        GuestThread* worker = scheduler_->create_thread(
            0,           // No initial entry point
            i,           // Worker ID as param
            64 * 1024,   // 64KB stack
            0            // Not suspended
        );

        if (worker) {
            worker->is_system_thread = true;
            worker->state = ThreadState::Waiting;  // Start in waiting state

            LOGI("Created system worker thread %u (DPC processor)", worker->thread_id);
        }
    }
}
```

---

### Task 6: Clean Up Debug Hacks

After the proper DPC flow is working, remove these temporary hacks:

**In `kernel.cpp`:**

- Remove `start_system_worker()` and `stop_system_worker()`
- Remove the system worker thread that monitors event addresses

**In `xboxkrnl_extended.cpp`:**

- Remove the aggressive event range signaling in `KeSetEventBoostPriority`
- Remove the `broad_init_done` static variable and related code

**In `jit_compiler.cpp`:**

- Remove spin loop detection/breaking code (the yield hack)

---

## Files to Modify

| File                                          | Priority | Changes                                  |
| --------------------------------------------- | -------- | ---------------------------------------- |
| `native/src/core/emulator.cpp`                | HIGH     | Add XKernel::run_for() to main loop      |
| `native/src/kernel/xobject.cpp`               | HIGH     | Implement actual DPC execution           |
| `native/src/kernel/xobject.h`                 | HIGH     | Update DpcEntry struct, add cpu\_ member |
| `native/src/kernel/hle/xboxkrnl_extended.cpp` | MEDIUM   | Trigger DPC processing on event signal   |
| `native/src/kernel/kernel.cpp`                | LOW      | Fix system thread creation, remove hacks |
| `native/src/cpu/jit/jit_compiler.cpp`         | LOW      | Remove spin loop hacks                   |

---

## Testing

After implementation:

1. Build and deploy to device
2. Load a game
3. Check logs for:
   - `"Processing X DPCs"` messages
   - `"Executing DPC"` messages
   - New syscalls beyond 2168/2144
   - `rb_base` becoming non-zero (GPU initialized)
4. Visual confirmation: Screen should change from purple to game content

---

## Expected Outcome

Once DPCs are properly executed:

1. DPC routines queued by the game will run
2. These routines will signal completion events
3. Main thread will see completion and proceed past initialization
4. Game will reach GPU initialization code
5. GPU ring buffer will be set up (`rb_base` non-zero)
6. Actual game rendering will begin

---

## Reference: Xbox 360 DPC Structure

```
KDPC Structure (0x28 bytes):
Offset  Size  Field
0x00    1     Type (19 = DpcObject)
0x01    1     Importance
0x02    1     Number (processor)
0x03    1     Padding
0x04    4     DpcListEntry.Flink
0x08    4     DpcListEntry.Blink
0x0C    4     DeferredRoutine
0x10    4     DeferredContext
0x14    4     SystemArgument1
0x18    4     SystemArgument2
0x1C    4     DpcData
```

The DPC routine signature:

```cpp
void DpcRoutine(
    PKDPC Dpc,           // r3 - pointer to DPC object
    PVOID DeferredContext, // r4 - from DeferredContext field
    PVOID SystemArgument1, // r5 - passed to KeInsertQueueDpc
    PVOID SystemArgument2  // r6 - passed to KeInsertQueueDpc
);
```
