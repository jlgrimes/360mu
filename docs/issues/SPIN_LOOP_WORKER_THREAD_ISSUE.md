# Critical Issue: Spin Loop Waiting for Worker Thread Completion

## Status: OPEN - Root Cause Identified

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

## Potential Solutions

### Option A: Implement Real Worker Threads (Proper Fix)

1. Create worker threads with actual executable code
2. Implement a work queue system
3. Worker threads dequeue and process work items
4. Set completion flags when done

**Pros:** Correct emulation, will work for all games
**Cons:** Complex, need to understand Xbox 360 kernel work queue format

### Option B: Force Completion Flag (Targeted Hack)

When `KeSetEventBoostPriority` is called repeatedly:

1. Detect the pattern (same LR, same event)
2. Calculate completion flag address (event + 0xFC for this game)
3. Write non-zero to force game to progress

**Pros:** Quick fix for specific games
**Cons:** Game-specific, may break other things

### Option C: Pre-signal System Events

Before calling game entry point:

1. Signal all system initialization events
2. Mark system as "ready"
3. May satisfy games waiting for kernel init

**Pros:** Simpler than full worker threads
**Cons:** May not work if games expect actual work to be done

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

1. **Investigate what work the game expects** - The work queue structure needs reverse engineering
2. **Implement minimal worker thread code** - Even a simple "mark all work complete" loop might help
3. **Test completion flag hack more thoroughly** - Verify the 0xFC offset is correct
4. **Try other games** - See if they have the same pattern or different init requirements
