# Multi-Threading Implementation Plan for 360μ

## Executive Summary

**Question:** Do other emulators use multi-threading for Xbox 360 emulation?

**Answer:** Yes, absolutely. Xenia (the reference Xbox 360 emulator) uses true multi-threaded execution, mapping guest threads to host threads. This is the **correct and necessary approach** for Xbox 360 emulation because:

1. Xbox 360 games are **heavily multi-threaded** (3 cores × 2 threads = 6 hardware threads)
2. Games rely on **precise timing between threads** for synchronization
3. Single-threaded emulation causes **infinite loops** when games wait for other threads

**Is it possible on Android?** Yes, and other emulators prove it:

- **Dolphin** (GameCube/Wii): Uses dual-core mode on Android
- **PPSSPP** (PSP): Multi-threaded OpenGL/Vulkan backends
- **Citra** (3DS): Dual ARM processor emulation

Modern Android devices (Snapdragon 8xx series) have 8 cores and can easily handle 4-6 guest threads.

---

## Current State Analysis

### What We Have

The threading infrastructure exists but is **disabled/incomplete**:

```cpp
// threading.cpp line 61
// TODO: Implement true multi-threaded execution
```

**Existing Components:**

- `ThreadScheduler` class with full priority queue system
- `GuestThread` structures with proper state management
- Hardware thread abstraction (6 threads mapped)
- Synchronization primitives (events, semaphores, mutants)

**What's Missing:**

1. Host thread spawning (commented out)
2. CPU context per-thread execution
3. Integration with emulation loop
4. Proper blocking in wait functions

### Why Games Are Stuck

Call of Duty: Black Ops calls `KeSetEventBoostPriority` in a tight loop because:

1. Main thread (thread 0) starts and waits for initialization
2. Initialization should happen on worker threads (threads 1-5)
3. Worker threads are **never scheduled** (single-threaded execution)
4. Main thread loops forever waiting for events that never get signaled

---

## Implementation Approaches

### Approach A: True Multi-Threaded (Recommended)

**How Xenia Does It:**

- Each guest hardware thread maps to a host thread
- JIT code runs natively on host threads
- Synchronization primitives use host OS primitives
- Thread affinity hints respected where possible

**Advantages:**

- Most accurate emulation
- Best performance on multi-core devices
- Games work as designed

**Disadvantages:**

- Complex synchronization
- Potential race conditions
- More debugging difficulty

### Approach B: Cooperative Multi-Threading (Fiber-Based)

**Alternative Approach:**

- Single host thread switches between guest contexts
- Use fibers/coroutines for context switching
- Time-slice based scheduling

**Advantages:**

- Simpler synchronization
- Deterministic execution
- Easier debugging

**Disadvantages:**

- Cannot use multiple cores efficiently
- May not satisfy timing-dependent games
- Performance limited to single core

### Approach C: Hybrid

**Best of Both Worlds:**

- Use 2-4 host threads instead of 6
- Group related guest threads on same host thread
- Critical threads get dedicated host threads

**This is what Dolphin does** - "Dual Core" mode uses 2 threads (CPU + GPU).

---

## Recommended Implementation: Hybrid Approach

Given Android hardware constraints and the need for stability, I recommend:

**Phase 1: Dual-Thread Mode**

- Thread 0: Main game thread + threads 1, 2
- Thread 1: GPU/render thread + threads 3, 4, 5

**Phase 2: Quad-Thread Mode** (optional)

- More granular distribution based on game requirements

---

## Detailed Implementation Plan

### Step 1: Enable ThreadScheduler Integration

**File:** `native/src/core/emulator.cpp`

Replace single-threaded execution:

```cpp
// Current (broken):
cpu_->execute(cycles);

// New:
scheduler_->run(cycles);
```

**Required Changes:**

1. Create `ThreadScheduler` instance in `Emulator`
2. Pass scheduler to kernel for thread creation
3. Route `ExCreateThread` through scheduler

### Step 2: Implement Host Thread Pool

**File:** `native/src/cpu/xenon/threading.cpp`

```cpp
Status ThreadScheduler::initialize(..., u32 num_host_threads) {
    // ...existing code...

    // Start host threads for multi-threaded execution
    for (u32 i = 0; i < num_host_threads && i < 6; i++) {
        hw_threads_[i].running = true;
        hw_threads_[i].stop_flag = false;
        hw_threads_[i].host_thread = std::thread(
            &ThreadScheduler::hw_thread_main, this, i
        );

        // Set thread affinity on Android
        #ifdef __ANDROID__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i % std::thread::hardware_concurrency(), &cpuset);
        pthread_setaffinity_np(
            hw_threads_[i].host_thread.native_handle(),
            sizeof(cpu_set_t), &cpuset
        );
        #endif
    }

    return Status::Ok;
}
```

### Step 3: Implement Hardware Thread Main Loop

**File:** `native/src/cpu/xenon/threading.cpp`

```cpp
void ThreadScheduler::hw_thread_main(u32 hw_thread_id) {
    auto& hwt = hw_threads_[hw_thread_id];
    u32 affinity_bit = 1u << hw_thread_id;

    LOGI("Hardware thread %u started", hw_thread_id);

    while (!hwt.stop_flag) {
        // Wait for work
        {
            std::unique_lock<std::mutex> lock(hwt.wake_mutex);
            hwt.wake_cv.wait(lock, [&]() {
                return hwt.stop_flag || hwt.current_thread != nullptr;
            });
        }

        if (hwt.stop_flag) break;

        GuestThread* thread = hwt.current_thread;
        if (!thread) continue;

        // Execute guest thread
        thread->state = ThreadState::Running;
        thread->context.running = true;

        // Run for time slice
        u64 cycles_remaining = TIME_SLICE;
        while (cycles_remaining > 0 && thread->state == ThreadState::Running) {
            u64 executed = execute_guest_code(thread, cycles_remaining);
            cycles_remaining -= executed;
            thread->execution_time += executed;

            // Check for syscalls, interrupts, etc.
            if (thread->context.interrupted) {
                handle_interrupt(thread);
            }
        }

        thread->context.running = false;

        // Reschedule
        schedule_thread(hw_thread_id);
    }

    LOGI("Hardware thread %u stopped", hw_thread_id);
}
```

### Step 4: Implement Blocking Wait

**File:** `native/src/kernel/hle/xboxkrnl.cpp`

```cpp
static void HLE_KeWaitForSingleObject(Cpu* cpu, Memory* memory,
                                       u64* args, u64* result) {
    GuestAddr object = static_cast<GuestAddr>(args[0]);
    GuestAddr timeout_ptr = static_cast<GuestAddr>(args[4]);

    // Get current thread from scheduler
    GuestThread* thread = g_scheduler->get_current_thread(
        cpu->get_context(0).thread_id % 6
    );

    // Check if already signaled
    u32 signal_state = memory->read_u32(object + 4);
    if (signal_state != 0) {
        // Already signaled - consume and return
        memory->write_u32(object + 4, signal_state - 1);
        *result = STATUS_WAIT_0;
        return;
    }

    // Calculate timeout
    u64 timeout_ns = UINT64_MAX;  // Infinite
    if (timeout_ptr) {
        s64 timeout = static_cast<s64>(memory->read_u64(timeout_ptr));
        if (timeout == 0) {
            *result = STATUS_TIMEOUT;
            return;
        }
        if (timeout < 0) {
            timeout_ns = static_cast<u64>(-timeout) * 100;  // 100ns units
        }
    }

    // Block the thread
    *result = g_scheduler->wait_for_object(thread, object, timeout_ns);
}
```

### Step 5: Implement Wait Queue Management

**File:** `native/src/cpu/xenon/threading.cpp`

```cpp
u32 ThreadScheduler::wait_for_object(GuestThread* thread,
                                      GuestAddr object, u64 timeout_ns) {
    if (!thread) return STATUS_INVALID_PARAMETER;

    std::unique_lock<std::mutex> lock(wait_mutex_);

    // Add to wait queue for this object
    thread->state = ThreadState::Waiting;
    thread->wait_object = object;
    thread->wait_timeout = current_time_ + timeout_ns;

    // Get or create sync object tracking
    auto& waiters = wait_queues_[object];
    waiters.push_back(thread);

    stats_.waiting_thread_count++;
    stats_.ready_thread_count--;

    // Signal scheduler to pick another thread
    u32 hw_thread = thread->context.thread_id % 6;
    hw_threads_[hw_thread].current_thread = nullptr;
    hw_threads_[hw_thread].wake_cv.notify_one();

    // The thread will be woken when:
    // 1. Object is signaled (via wake_waiting_threads)
    // 2. Timeout expires (via timer check in scheduler)

    return STATUS_PENDING;  // Actual result set when woken
}

void ThreadScheduler::wake_waiting_threads(GuestAddr object) {
    std::lock_guard<std::mutex> lock(wait_mutex_);

    auto it = wait_queues_.find(object);
    if (it == wait_queues_.end()) return;

    for (GuestThread* thread : it->second) {
        if (thread->state == ThreadState::Waiting) {
            thread->state = ThreadState::Ready;
            thread->wait_result = STATUS_WAIT_0;
            enqueue_thread(thread);

            stats_.waiting_thread_count--;

            // Wake a hardware thread to run it
            for (auto& hwt : hw_threads_) {
                if (!hwt.current_thread) {
                    hwt.wake_cv.notify_one();
                    break;
                }
            }
        }
    }

    it->second.clear();
}
```

### Step 6: Connect Event Signaling

**File:** `native/src/kernel/hle/xboxkrnl.cpp`

```cpp
static void HLE_KeSetEvent(Cpu* cpu, Memory* memory,
                           u64* args, u64* result) {
    GuestAddr event = static_cast<GuestAddr>(args[0]);

    // Set signal state
    s32 prev_state = static_cast<s32>(memory->read_u32(event + 4));
    memory->write_u32(event + 4, 1);

    // Wake any threads waiting on this event
    if (g_scheduler) {
        g_scheduler->wake_waiting_threads(event);
    }

    *result = static_cast<u64>(prev_state);
}
```

---

## Android-Specific Considerations

### Thread Affinity

```cpp
#ifdef __ANDROID__
#include <sched.h>
#include <pthread.h>

void set_thread_affinity(std::thread& thread, int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(thread.native_handle(),
                          sizeof(cpu_set_t), &cpuset);
}
#endif
```

### big.LITTLE Optimization

Modern Android SoCs have big (performance) and LITTLE (efficiency) cores:

```cpp
void assign_thread_to_core(u32 guest_thread_id, std::thread& host_thread) {
    // Guest thread 0 (main) -> Big core 0
    // Guest threads 1-2 (workers) -> Big cores 1-2
    // Guest threads 3-5 (background) -> LITTLE cores

    int core;
    if (guest_thread_id < 3) {
        core = guest_thread_id;  // Big cores (typically 0-3)
    } else {
        core = 4 + (guest_thread_id - 3);  // LITTLE cores (typically 4-7)
    }

    set_thread_affinity(host_thread, core);
}
```

### Power Management

```cpp
// Request sustained performance mode
#ifdef __ANDROID__
#include <android/thermal.h>

void request_sustained_performance() {
    // Use Android Thermal API to request sustained performance
    // This prevents thermal throttling during intensive emulation
}
#endif
```

---

## Testing Strategy

### Unit Tests

```cpp
TEST(ThreadScheduler, CreateAndRunThread) {
    ThreadScheduler scheduler;
    Memory memory;
    scheduler.initialize(&memory, nullptr, 2);

    GuestThread* thread = scheduler.create_thread(
        0x82000000,  // entry
        0,           // param
        64 * 1024,   // stack
        0            // flags
    );

    ASSERT_NE(thread, nullptr);
    ASSERT_EQ(thread->state, ThreadState::Ready);

    scheduler.run(1000000);

    // Thread should have executed
    ASSERT_GT(thread->execution_time, 0);
}

TEST(ThreadScheduler, WaitAndSignal) {
    // Test that waiting thread wakes when event is signaled
}

TEST(ThreadScheduler, PriorityScheduling) {
    // Test that high priority threads run before low priority
}
```

### Integration Tests

1. **Simple multi-threaded XEX**: Create test XEX that spawns threads and signals events
2. **Synchronization stress test**: Many threads with complex synchronization
3. **Real game test**: Black Ops boot sequence

---

## Performance Targets

| Metric              | Target    | Rationale               |
| ------------------- | --------- | ----------------------- |
| Context switch time | < 1μs     | Native threads are fast |
| Threads per second  | > 100,000 | Scheduler efficiency    |
| CPU utilization     | > 80%     | Effective parallelism   |
| Frame time variance | < 5ms     | Smooth gameplay         |

---

## Risk Mitigation

### Race Conditions

- Use ThreadSanitizer during development
- Extensive stress testing
- Clear ownership rules for shared state

### Deadlocks

- Implement timeout on all waits
- Detect and break deadlock cycles
- Logging for debugging

### Performance Regression

- Benchmark before/after
- Profile on target Android devices
- Fallback to single-threaded mode if needed

---

## Implementation Timeline

| Phase                    | Duration | Deliverable                           |
| ------------------------ | -------- | ------------------------------------- |
| 1. Scheduler Integration | 2-3 days | ThreadScheduler connected to emulator |
| 2. Host Thread Pool      | 2-3 days | Multi-threaded execution working      |
| 3. Blocking Waits        | 2-3 days | Proper synchronization                |
| 4. Testing & Debugging   | 3-5 days | Stable multi-threaded execution       |
| 5. Android Optimization  | 2-3 days | Thread affinity, power management     |

**Total: 2-3 weeks**

---

## Conclusion

Multi-threaded emulation is **necessary** for proper Xbox 360 emulation and **feasible** on modern Android hardware. The infrastructure already exists in 360μ - it just needs to be enabled and integrated.

The hybrid approach (2-4 host threads) provides the best balance of accuracy, performance, and stability for Android devices.

**Next Steps:**

1. Start with Step 1 (Enable ThreadScheduler Integration)
2. Test on simple multi-threaded scenarios
3. Gradually enable more host threads as stability improves
