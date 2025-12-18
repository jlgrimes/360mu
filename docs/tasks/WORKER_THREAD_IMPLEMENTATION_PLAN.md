# Worker Thread Implementation Plan

## Priority: CRITICAL

## Status: NOT STARTED

## Blocking Issue: Purple screen / Game stuck in spin loop

---

## Executive Summary

The Xbox 360 kernel is based on Windows NT. Games use `ExQueueWorkItem` to queue work to system worker threads. Our emulator creates worker threads with `entry_point=0`, meaning they can never execute guest code. This plan implements real worker thread functionality.

---

## Background

### How Xbox 360 Worker Threads Work

```
┌─────────────────┐     ExQueueWorkItem()     ┌─────────────────┐
│   Game Thread   │ ──────────────────────────▶│   Work Queue    │
│                 │                            │  (LIST_ENTRY)   │
│  Polls for      │                            └────────┬────────┘
│  completion     │                                     │
│  flag           │                                     ▼
│                 │                            ┌─────────────────┐
│                 │                            │  Worker Thread  │
│                 │                            │                 │
│                 │◀─── sets completion ───────│  Dequeues item  │
│                 │     flag                   │  Calls routine  │
└─────────────────┘                            └─────────────────┘
```

### Key Structures (Windows NT / Xbox 360)

```c
// Work queue item - queued by ExQueueWorkItem
typedef struct _WORK_QUEUE_ITEM {
    LIST_ENTRY             List;           // Offset 0x00: Linked list pointers
    PWORKER_THREAD_ROUTINE WorkerRoutine;  // Offset 0x08: Guest function to call
    PVOID                  Parameter;      // Offset 0x0C: Context for routine
} WORK_QUEUE_ITEM;  // Size: 0x10 (16 bytes)

// Worker routine signature
typedef VOID (*PWORKER_THREAD_ROUTINE)(PVOID Parameter);

// Queue types
typedef enum _WORK_QUEUE_TYPE {
    CriticalWorkQueue = 0,    // High priority
    DelayedWorkQueue = 1,     // Normal priority
    HyperCriticalWorkQueue = 2,
    MaximumWorkQueue = 3
} WORK_QUEUE_TYPE;
```

### Xbox 360 Kernel Functions Involved

| Ordinal | Function             | Description                        |
| ------- | -------------------- | ---------------------------------- |
| 60      | ExInitializeWorkItem | Initialize a WORK_QUEUE_ITEM       |
| 61      | ExQueueWorkItem      | Queue work item to worker thread   |
| 62      | ExQueueWorkItemEx    | Extended version with more options |

---

## Implementation Tasks

### Task 1: Define Work Queue Structures

**File:** `native/src/kernel/work_queue.h` (NEW FILE)

```cpp
#pragma once

#include "../types.h"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace x360mu {

// Mirrors Xbox 360 WORK_QUEUE_ITEM structure
struct WorkQueueItem {
    GuestAddr list_flink;      // Offset 0x00
    GuestAddr list_blink;      // Offset 0x04
    GuestAddr worker_routine;  // Offset 0x08 - Guest function pointer
    GuestAddr parameter;       // Offset 0x0C - Context parameter

    // Host-side tracking
    GuestAddr item_address;    // Address of this item in guest memory
};

enum class WorkQueueType : u32 {
    Critical = 0,
    Delayed = 1,
    HyperCritical = 2,
    Maximum = 3
};

class WorkQueue {
public:
    void enqueue(const WorkQueueItem& item);
    bool dequeue(WorkQueueItem& item, u32 timeout_ms = INFINITE);
    void shutdown();
    bool is_empty() const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<WorkQueueItem> items_;
    std::atomic<bool> shutdown_{false};
};

// Global work queues (one per queue type)
class WorkQueueManager {
public:
    static WorkQueueManager& instance();

    void enqueue(WorkQueueType type, const WorkQueueItem& item);
    bool dequeue(WorkQueueType type, WorkQueueItem& item, u32 timeout_ms);
    void shutdown_all();

private:
    WorkQueue queues_[static_cast<size_t>(WorkQueueType::Maximum)];
};

} // namespace x360mu
```

---

### Task 2: Implement Work Queue

**File:** `native/src/kernel/work_queue.cpp` (NEW FILE)

```cpp
#include "work_queue.h"
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-workq"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) printf("[WorkQ] " __VA_ARGS__); printf("\n")
#define LOGW(...) printf("[WorkQ WARN] " __VA_ARGS__); printf("\n")
#endif

namespace x360mu {

void WorkQueue::enqueue(const WorkQueueItem& item) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(item);
        LOGI("Enqueued work item: routine=0x%08X, param=0x%08X, queue_size=%zu",
             (u32)item.worker_routine, (u32)item.parameter, items_.size());
    }
    cv_.notify_one();
}

bool WorkQueue::dequeue(WorkQueueItem& item, u32 timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (timeout_ms == INFINITE) {
        cv_.wait(lock, [this] { return !items_.empty() || shutdown_; });
    } else {
        auto result = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                   [this] { return !items_.empty() || shutdown_; });
        if (!result) return false;  // Timeout
    }

    if (shutdown_ || items_.empty()) return false;

    item = items_.front();
    items_.pop_front();

    LOGI("Dequeued work item: routine=0x%08X, param=0x%08X, remaining=%zu",
         (u32)item.worker_routine, (u32)item.parameter, items_.size());

    return true;
}

void WorkQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

bool WorkQueue::is_empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.empty();
}

size_t WorkQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

// WorkQueueManager singleton
WorkQueueManager& WorkQueueManager::instance() {
    static WorkQueueManager instance;
    return instance;
}

void WorkQueueManager::enqueue(WorkQueueType type, const WorkQueueItem& item) {
    if (type >= WorkQueueType::Maximum) {
        type = WorkQueueType::Delayed;  // Default
    }
    queues_[static_cast<size_t>(type)].enqueue(item);
}

bool WorkQueueManager::dequeue(WorkQueueType type, WorkQueueItem& item, u32 timeout_ms) {
    if (type >= WorkQueueType::Maximum) {
        type = WorkQueueType::Delayed;
    }
    return queues_[static_cast<size_t>(type)].dequeue(item, timeout_ms);
}

void WorkQueueManager::shutdown_all() {
    for (auto& queue : queues_) {
        queue.shutdown();
    }
}

} // namespace x360mu
```

---

### Task 3: Implement HLE Functions

**File:** `native/src/kernel/hle/xboxkrnl_work.cpp` (NEW FILE)

```cpp
#include "xboxkrnl.h"
#include "../work_queue.h"
#include "../../memory/memory.h"

namespace x360mu {

// Ordinal 60: ExInitializeWorkItem
// void ExInitializeWorkItem(PWORK_QUEUE_ITEM WorkItem,
//                           PWORKER_THREAD_ROUTINE Routine,
//                           PVOID Context)
void HLE_ExInitializeWorkItem(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr work_item = static_cast<GuestAddr>(args[0]);
    GuestAddr routine = static_cast<GuestAddr>(args[1]);
    GuestAddr context = static_cast<GuestAddr>(args[2]);

    // Initialize LIST_ENTRY to point to itself (empty list)
    memory->write_u32(work_item + 0x00, work_item);  // Flink = self
    memory->write_u32(work_item + 0x04, work_item);  // Blink = self
    memory->write_u32(work_item + 0x08, routine);    // WorkerRoutine
    memory->write_u32(work_item + 0x0C, context);    // Parameter

    LOGI("ExInitializeWorkItem: item=0x%08X, routine=0x%08X, context=0x%08X",
         (u32)work_item, (u32)routine, (u32)context);

    *result = 0;  // void function
}

// Ordinal 61: ExQueueWorkItem
// void ExQueueWorkItem(PWORK_QUEUE_ITEM WorkItem, WORK_QUEUE_TYPE QueueType)
void HLE_ExQueueWorkItem(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr work_item_addr = static_cast<GuestAddr>(args[0]);
    u32 queue_type = static_cast<u32>(args[1]);

    // Read the work item from guest memory
    WorkQueueItem item;
    item.list_flink = memory->read_u32(work_item_addr + 0x00);
    item.list_blink = memory->read_u32(work_item_addr + 0x04);
    item.worker_routine = memory->read_u32(work_item_addr + 0x08);
    item.parameter = memory->read_u32(work_item_addr + 0x0C);
    item.item_address = work_item_addr;

    LOGI("ExQueueWorkItem: item=0x%08X, routine=0x%08X, param=0x%08X, type=%u",
         (u32)work_item_addr, (u32)item.worker_routine, (u32)item.parameter, queue_type);

    // Validate routine pointer
    if (item.worker_routine == 0 || item.worker_routine < 0x80000000) {
        LOGW("ExQueueWorkItem: Invalid routine pointer 0x%08X", (u32)item.worker_routine);
        *result = 0;
        return;
    }

    // Queue to appropriate work queue
    WorkQueueManager::instance().enqueue(static_cast<WorkQueueType>(queue_type), item);

    *result = 0;  // void function
}

// Register these HLE functions
void register_work_queue_functions(HleFunctionMap& hle_functions) {
    hle_functions[make_import_key(0, 60)] = HLE_ExInitializeWorkItem;
    hle_functions[make_import_key(0, 61)] = HLE_ExQueueWorkItem;
    // Add ExQueueWorkItemEx (ordinal 62) if needed
}

} // namespace x360mu
```

---

### Task 4: Implement Worker Thread Entry Point

**File:** `native/src/kernel/kernel.cpp` (MODIFY)

**Change:** Instead of creating worker threads with `entry_point=0`, create them with a real entry point that processes the work queue.

```cpp
// NEW: Worker thread main loop (executed as guest code)
// This function runs on the worker GuestThread and processes work items
void worker_thread_loop(ThreadScheduler* scheduler, Cpu* cpu, Memory* memory,
                        GuestThread* thread, WorkQueueType queue_type) {
    LOGI("Worker thread %u starting (queue type %u)", thread->thread_id, (u32)queue_type);

    while (thread->state != ThreadState::Terminated) {
        WorkQueueItem item;

        // Try to dequeue a work item (blocking with timeout)
        if (WorkQueueManager::instance().dequeue(queue_type, item, 100)) {
            LOGI("Worker thread %u executing routine 0x%08X with param 0x%08X",
                 thread->thread_id, (u32)item.worker_routine, (u32)item.parameter);

            // Set up guest context to call the worker routine
            // The routine signature is: void WorkerRoutine(PVOID Parameter)
            thread->context.pc = item.worker_routine;
            thread->context.gpr[3] = item.parameter;  // r3 = first parameter
            thread->context.lr = 0;  // Return address 0 = exit routine

            // Execute the guest routine
            // The routine will run until it returns (blr to address 0)
            thread->state = ThreadState::Running;

            // Execute until routine completes
            // This is handled by the scheduler - when PC becomes 0 (from blr),
            // the thread returns to this loop
            while (thread->context.pc != 0 && thread->state == ThreadState::Running) {
                cpu->execute_guest_code(thread->context, 10000);  // 10K cycles per iteration
            }

            LOGI("Worker thread %u completed routine 0x%08X",
                 thread->thread_id, (u32)item.worker_routine);
        }

        // Yield to allow other threads to run
        std::this_thread::yield();
    }

    LOGI("Worker thread %u exiting", thread->thread_id);
}
```

---

### Task 5: Modify System Thread Creation

**File:** `native/src/kernel/kernel.cpp` (MODIFY)

**Current code (broken):**

```cpp
GuestThread* worker = scheduler_->create_thread(
    0,           // No initial entry point - DPC processor  <-- PROBLEM!
    i,           // param (worker ID)
    64 * 1024,   // 64KB stack
    0            // not suspended
);
worker->state = ThreadState::Waiting;  // Start waiting
```

**New code:**

```cpp
// Create worker threads that actually process work queues
void Kernel::create_system_worker_threads() {
    for (u32 i = 0; i < NUM_WORKER_THREADS; i++) {
        GuestThread* worker = scheduler_->create_thread(
            WORKER_THREAD_ENTRY,  // Special marker or trampoline address
            i,                     // Worker ID / queue type
            64 * 1024,            // 64KB stack
            0                      // Not suspended
        );

        worker->is_system_thread = true;
        worker->is_worker_thread = true;  // NEW FLAG
        worker->worker_queue_type = static_cast<WorkQueueType>(i % 3);

        // Don't set to Waiting - let it run and process work queue
        worker->state = ThreadState::Ready;

        LOGI("Created system worker thread %u for queue type %u",
             worker->thread_id, (u32)worker->worker_queue_type);
    }
}
```

---

### Task 6: Modify Thread Scheduler

**File:** `native/src/cpu/xenon/threading.cpp` (MODIFY)

**Add worker thread handling in `hw_thread_main`:**

```cpp
void ThreadScheduler::hw_thread_main(u32 hw_thread_id) {
    // ... existing setup code ...

    while (running_) {
        GuestThread* thread = nullptr;

        // ... existing thread selection code ...

        if (thread) {
            // Check if this is a worker thread
            if (thread->is_worker_thread) {
                // Run worker thread loop instead of normal execution
                process_worker_thread(thread);
            } else {
                // Normal guest thread execution
                execute_guest_thread(thread, cycles_per_batch);
            }
        }

        // ... existing code ...
    }
}

void ThreadScheduler::process_worker_thread(GuestThread* thread) {
    WorkQueueItem item;

    // Try to get work (non-blocking for responsiveness)
    if (WorkQueueManager::instance().dequeue(thread->worker_queue_type, item, 10)) {
        LOGI("Worker %u processing routine 0x%08X", thread->thread_id, (u32)item.worker_routine);

        // Save current context
        ThreadContext saved_ctx = thread->context;

        // Set up context to call worker routine
        thread->context.pc = item.worker_routine;
        thread->context.gpr[3] = item.parameter;
        thread->context.lr = 0;  // Return to 0 = done
        thread->state = ThreadState::Running;

        // Execute until routine returns (pc == 0 after blr)
        constexpr u64 MAX_WORKER_CYCLES = 1000000;  // 1M cycles max per work item
        u64 cycles_executed = 0;

        while (thread->context.pc != 0 &&
               thread->state == ThreadState::Running &&
               cycles_executed < MAX_WORKER_CYCLES) {
            cpu_->execute_with_context(thread->thread_id, thread->context, 10000);
            cycles_executed += 10000;
        }

        if (thread->context.pc != 0) {
            LOGW("Worker routine 0x%08X didn't complete in time", (u32)item.worker_routine);
        }

        // Restore context (worker is now ready for next item)
        thread->context = saved_ctx;
    }
}
```

---

### Task 7: Handle Worker Routine Return

**File:** `native/src/cpu/xenon/cpu.cpp` or `jit_compiler.cpp` (MODIFY)

When a `blr` instruction returns to address 0, the worker routine has completed:

```cpp
// In JIT block execution or interpreter
if (ctx.pc == 0) {
    // Worker routine returned - this is expected
    // The calling code (process_worker_thread) will handle this
    ctx.running = false;
    return;
}
```

---

### Task 8: Integration & Testing

**File:** `native/src/core/emulator.cpp` (MODIFY)

Ensure work queue manager is initialized and shut down properly:

```cpp
Status Emulator::initialize(...) {
    // ... existing init ...

    // Work queue manager is a singleton, but ensure it's ready
    // (It auto-initializes, but add explicit init if needed)

    return Status::Ok;
}

void Emulator::shutdown() {
    // Shutdown work queues to unblock worker threads
    WorkQueueManager::instance().shutdown_all();

    // ... existing shutdown ...
}
```

---

## File Changes Summary

| File                                      | Action | Description                               |
| ----------------------------------------- | ------ | ----------------------------------------- |
| `native/src/kernel/work_queue.h`          | CREATE | Work queue structures and manager         |
| `native/src/kernel/work_queue.cpp`        | CREATE | Work queue implementation                 |
| `native/src/kernel/hle/xboxkrnl_work.cpp` | CREATE | ExInitializeWorkItem, ExQueueWorkItem HLE |
| `native/src/kernel/kernel.cpp`            | MODIFY | Create worker threads with real code      |
| `native/src/cpu/xenon/threading.cpp`      | MODIFY | Add worker thread processing              |
| `native/src/cpu/xenon/threading.h`        | MODIFY | Add worker thread flags to GuestThread    |
| `native/src/core/emulator.cpp`            | MODIFY | Initialize/shutdown work queue manager    |
| `native/CMakeLists.txt`                   | MODIFY | Add new source files                      |

---

## Testing Plan

### Test 1: Work Queue Unit Test

- Enqueue items, verify dequeue order
- Test timeout behavior
- Test shutdown behavior

### Test 2: HLE Function Test

- Call ExInitializeWorkItem, verify memory layout
- Call ExQueueWorkItem, verify item is queued

### Test 3: Worker Thread Test

- Create worker thread, queue work item
- Verify worker routine gets called
- Verify parameter is passed correctly

### Test 4: Integration Test (Call of Duty: Black Ops)

- Boot game
- Monitor for ExQueueWorkItem calls
- Verify worker processes work
- Verify completion flag gets set
- Verify game progresses past purple screen

---

## Expected Outcome

After implementation:

1. Game calls `ExQueueWorkItem` during initialization
2. Work item is added to queue
3. Worker thread dequeues and executes guest routine
4. Guest routine sets completion flag at `r31 + 0x14C`
5. Main thread sees flag, exits spin loop
6. Game continues to GPU initialization
7. **Purple screen disappears, game renders!**

---

## Risks & Mitigations

| Risk                      | Mitigation                            |
| ------------------------- | ------------------------------------- |
| Wrong ordinals            | Verify against Xenia source code      |
| Structure layout mismatch | Compare with Windows NT documentation |
| Worker routine crashes    | Add exception handling, logging       |
| Deadlock in work queue    | Use timeout-based dequeue             |
| Performance issues        | Profile, optimize critical paths      |

---

## References

- [Xenia xboxkrnl_threading.cc](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc)
- [Windows WORK_QUEUE_ITEM](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_work_queue_item)
- [Windows ExQueueWorkItem](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exqueueworkitem)
- [Free60 Wiki](https://free60.org/)
