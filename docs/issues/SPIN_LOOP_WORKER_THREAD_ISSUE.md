# Critical Issue: Spin Loop Waiting for Worker Thread Completion

## Status: SOLUTION IDENTIFIED - Implementing 1:1 Threading Model

**Solution**: Migrate from N:M cooperative scheduling to 1:1 threading model (like Xenia).
See: `/docs/architecture/THREADING_MODEL.md`

## Summary

Call of Duty: Black Ops (and likely other games) gets stuck in a spin loop during early initialization, causing a purple screen. The game is waiting for worker threads to complete work, but our emulated worker threads have no executable code.

---

## Symptoms

- Purple screen (GPU test clear color)
- High CPU usage (game spinning in loop)
- Only syscall being made is `KeSetEventBoostPriority` (ordinal 2168)
- GPU never receives ring buffer configuration or rendering commands

---

## Root Cause Analysis

### The Spin Loop

The game is stuck at PC around `0x824D2BC0-0x824D2BE8` in a loop that:

1. Loads a completion flag from `r31 + 0x14C` (stack address `0x9FFEFD3C`)
2. Checks if it's non-zero
3. If zero, calls `KeSetEventBoostPriority` to try to wake workers
4. Loops back and checks again

**Disassembled spin loop:**

```
0x824D2BC0: 2B0B0000  cmplwi r11, 0           ; Check if completion flag is zero
0x824D2BC4: 409AFD8C  bne 0x824D2950          ; If non-zero, continue (exit loop)
0x824D2BC8: 3CA06000  lis r5, 0x6000          ; Otherwise, set up for syscall
0x824D2BCC: 7F67DB78  mr r7, r27
0x824D2BD0: 38C00004  li r6, 4
0x824D2BD4: 60A52000  ori r5, r5, 0x2000
0x824D2BD8: 389F0144  addi r4, r31, 0x144     ; Boost parameter
0x824D2BDC: 387F0050  addi r3, r31, 0x50      ; Event address (0x9FFEFC40)
0x824D2BE0: 4813F941  bl KeSetEventBoostPriority
0x824D2BE4: 2C030000  cmpwi r3, 0             ; Check return value
0x824D2BE8: 4180FD68  blt 0x824D2950          ; If error, branch
0x824D2BEC: 817F014C  lwz r11, 0x14C(r31)     ; RELOAD completion flag
0x824D2BF4: 2B0B0000  cmplwi r11, 0           ; Check again...
```

### Key Memory Addresses

| Address      | Description                                     |
| ------------ | ----------------------------------------------- |
| `0x9FFEFBF0` | r31 (stack frame base)                          |
| `0x9FFEFC40` | Event object (r31 + 0x50)                       |
| `0x9FFEFD34` | Boost parameter (r31 + 0x144)                   |
| `0x9FFEFD3C` | **Completion flag** (r31 + 0x14C) - **THE KEY** |

### Why It's Stuck

The game expects:

1. Main thread queues work items
2. Worker threads dequeue and process work
3. Worker threads write non-zero to completion flag (`r31 + 0x14C`)
4. Main thread sees flag and continues

**But in our emulator:**

- Worker threads are created with `entry_point = 0` (no code!)
- They're set to `ThreadState::Waiting` immediately
- They can never execute any code
- Completion flag is never set
- Game loops forever

### Evidence from Logs

```
Created system worker thread 2 (DPC processor) - starting in Waiting state
Created system worker thread 3 (DPC processor) - starting in Waiting state
Created system worker thread 4 (DPC processor) - starting in Waiting state
```

All worker threads have `entry=0x00000000` - they have no code to run!

---

## What Xbox 360 Does Differently

On real Xbox 360 hardware:

1. Kernel boots and creates system worker threads with real code
2. Worker threads run in a loop: `dequeue_work() -> process() -> signal_completion()`
3. Various system services are initialized
4. System events are signaled when subsystems are ready
5. **THEN** game entry point is called

Our emulator calls the game entry point immediately without proper kernel initialization.

---

## Attempted Fixes

### 1. Thread Tracking Fix ✅

Fixed `ThreadScheduler::run()` to properly assign main thread to `hw_threads_[0]`, allowing `get_current_thread()` to work.

### 2. Wait Object Fix ✅

Fixed `wait_for_object()` to not busy-loop returning success.

### 3. Completion Flag Hack ❌ (Partial)

Added code to write to `event + 0xFC` (completion flag offset) after 50 syscalls. Needs more testing - offset may not be consistent.

---

## Solution: 1:1 Threading Model (Xenia's Approach)

After researching how Xenia (the reference Xbox 360 emulator) handles this, the proper solution is clear:

### The Problem with N:M Cooperative Scheduling

Our previous architecture:

- N host threads share M guest threads via cooperative scheduling
- `wait_for_object()` returns `STATUS_TIMEOUT` immediately (no real blocking)
- `signal_object()` writes memory flags (no real thread wake)
- Worker threads have `entry=0` (no code to run)

### Xenia's 1:1 Model

Xenia uses a 1:1 thread mapping:

- Each `GuestThread` has its own `std::thread`
- `KeWaitForSingleObject` actually blocks the host thread using `std::condition_variable`
- `KeSetEvent` signals the condition variable, waking blocked threads
- `ExQueueWorkItem` creates a NEW thread for each work item

### Implementation Plan

1. **Enhance GuestThread**: Add `std::thread`, `std::mutex`, `std::condition_variable`
2. **Real Blocking**: `KeWaitForSingleObject` blocks on condition variable
3. **Real Wake**: `KeSetEvent` signals condition variable
4. **Work Items**: `ExQueueWorkItem` spawns new thread per item

See `/docs/architecture/THREADING_MODEL.md` for full details.

### Why This Fixes Call of Duty

The game's pattern:

1. Game calls `KeSetEventBoostPriority` to wake workers
2. Workers should be BLOCKED on `KeWaitForSingleObject`
3. Workers wake up, process work, set completion flag
4. Game's spin-poll sees flag and continues

With 1:1 threading:

- Worker threads can actually block and wake
- Real synchronization replaces memory flag polling
- Proper multi-threaded execution

---

## Files Involved

- `native/src/cpu/xenon/threading.cpp` - Thread scheduler
- `native/src/kernel/kernel.cpp` - Guest thread creation
- `native/src/kernel/hle/xboxkrnl_extended.cpp` - KeSetEventBoostPriority implementation
- `native/src/cpu/xenon/cpu.cpp` - Syscall dispatch

---

## Debugging Commands

```bash
# Check for spin loop
adb logcat -d | grep "KeSetEventBoostPriority"

# Get code dump around spin loop
adb logcat -d | grep -A 50 "SPIN LOOP CODE DUMP"

# Check what threads exist
adb logcat -d | grep "Created thread"

# Check for any syscalls other than 2168
adb logcat -d | grep "dispatch_syscall" | grep -v "ordinal=2168"
```

---

## Next Steps

1. ✅ **Document the architecture gap** - See `/docs/architecture/THREADING_MODEL.md`
2. 🔄 **Implement 1:1 threading** - Each GuestThread gets own host thread
3. 🔄 **Implement real blocking** - KeWaitForSingleObject uses condition variables
4. 🔄 **Implement real wake** - KeSetEvent signals condition variables
5. 🔄 **Test with Call of Duty** - Verify it boots past purple screen
