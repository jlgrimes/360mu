# Task: Implement Alertable Waits and APC Processing

## Priority: MEDIUM

## Problem

Thread wait functions in `native/src/kernel/xthread.cpp` ignore the `alertable` parameter, so Asynchronous Procedure Calls (APCs) never fire.

Current broken code at line 298:

```cpp
(void)alertable;  // TODO: Handle alertable waits
```

Games use APCs for:

- Async I/O completion callbacks
- Timer callbacks
- Thread pool work items
- Deferred procedure calls

Without alertable waits, async operations never complete their callbacks.

## Solution

Implement an APC queue for each thread and process pending APCs when entering alertable waits.

## Background: Windows/Xbox APC Model

APCs (Asynchronous Procedure Calls) are callbacks queued to a specific thread:

- **User-mode APCs**: Only delivered when thread is in "alertable" wait state
- **Kernel-mode APCs**: Delivered immediately (we treat these the same for simplicity)

An alertable wait is a wait function called with `alertable=true`:

- `KeWaitForSingleObject(..., TRUE)`
- `NtWaitForSingleObject(..., TRUE)`
- `SleepEx(ms, TRUE)`

When entering an alertable wait with pending APCs, the wait returns immediately with `STATUS_USER_APC` and the APCs are executed.

## Implementation Steps

### Step 1: Add APC structures to xthread.h

In `native/src/kernel/xthread.h`, add these definitions:

```cpp
// APC (Asynchronous Procedure Call) entry
struct XApcEntry {
    GuestAddr routine;      // Function to call
    GuestAddr context;      // First argument (context pointer)
    GuestAddr system_arg1;  // Second argument
    GuestAddr system_arg2;  // Third argument
    bool kernel_mode;       // Kernel-mode APCs execute immediately
};

class XThread : public XObject {
    // ... existing members ...

private:
    // APC queue
    std::deque<XApcEntry> apc_queue_;
    std::mutex apc_mutex_;

    // Reference to CPU for executing APC routines
    Cpu* cpu_ = nullptr;

public:
    // APC management
    void set_cpu(Cpu* cpu) { cpu_ = cpu; }

    /**
     * Queue an APC to this thread
     * @param routine Guest address of APC routine
     * @param context Context pointer passed to routine
     * @param arg1 First system argument
     * @param arg2 Second system argument
     * @param kernel_mode If true, executes immediately
     */
    void queue_apc(GuestAddr routine, GuestAddr context,
                   GuestAddr arg1, GuestAddr arg2,
                   bool kernel_mode = false);

    /**
     * Check if there are pending user-mode APCs
     */
    bool has_pending_apcs() const;

    /**
     * Process all pending APCs
     * Called when entering alertable wait or thread is alerted
     * @return Number of APCs processed
     */
    u32 process_pending_apcs();

    /**
     * Alert this thread (causes alertable waits to return)
     */
    void alert();

private:
    bool alerted_ = false;
};
```

### Step 2: Implement APC methods in xthread.cpp

Add these implementations to `native/src/kernel/xthread.cpp`:

```cpp
void XThread::queue_apc(GuestAddr routine, GuestAddr context,
                        GuestAddr arg1, GuestAddr arg2,
                        bool kernel_mode) {
    std::lock_guard<std::mutex> lock(apc_mutex_);

    XApcEntry apc;
    apc.routine = routine;
    apc.context = context;
    apc.system_arg1 = arg1;
    apc.system_arg2 = arg2;
    apc.kernel_mode = kernel_mode;

    if (kernel_mode) {
        // Kernel APCs go to front of queue
        apc_queue_.push_front(apc);
    } else {
        // User APCs go to back
        apc_queue_.push_back(apc);
    }

    LOGD("Queued APC to thread %u: routine=0x%08X, context=0x%08X",
         thread_id_, routine, context);

    // If kernel-mode APC and thread is running, interrupt it
    if (kernel_mode) {
        // Signal the thread to process APCs
        alert();
    }
}

bool XThread::has_pending_apcs() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(apc_mutex_));
    return !apc_queue_.empty();
}

u32 XThread::process_pending_apcs() {
    if (!cpu_) {
        LOGW("Cannot process APCs: no CPU reference");
        return 0;
    }

    u32 count = 0;

    while (true) {
        XApcEntry apc;
        {
            std::lock_guard<std::mutex> lock(apc_mutex_);
            if (apc_queue_.empty()) {
                break;
            }
            apc = apc_queue_.front();
            apc_queue_.pop_front();
        }

        LOGI("Executing APC: routine=0x%08X, context=0x%08X",
             apc.routine, apc.context);

        // Call the APC routine
        // APC signature: void ApcRoutine(PVOID context, PVOID arg1, PVOID arg2)
        ThreadContext& ctx = cpu_->get_context(thread_id_);

        // Save current state
        GuestAddr saved_pc = ctx.pc;
        GuestAddr saved_lr = ctx.lr;

        // Set up APC call
        ctx.gpr[3] = apc.context;      // First argument
        ctx.gpr[4] = apc.system_arg1;  // Second argument
        ctx.gpr[5] = apc.system_arg2;  // Third argument
        ctx.lr = saved_pc;              // Return to current PC
        ctx.pc = apc.routine;           // Jump to APC routine

        // Execute until return
        // This is simplified - real implementation would run until blr
        // For now, we'll execute a fixed number of cycles
        cpu_->execute_thread(thread_id_, 10000);

        // Restore state (the APC should have returned via blr)
        // In practice, the LR setup handles this

        count++;
    }

    alerted_ = false;
    return count;
}

void XThread::alert() {
    alerted_ = true;
    // Wake up the thread if it's waiting
    // This depends on your wait implementation
    wake();
}
```

### Step 3: Update wait functions to check alertable

In `native/src/kernel/xthread.cpp`, update the `wait` method (around line 290-310):

```cpp
Status XThread::wait(u64 timeout_100ns, bool alertable) {
    // Check for pending APCs if alertable
    if (alertable && has_pending_apcs()) {
        // Process APCs and return STATUS_USER_APC
        process_pending_apcs();
        return Status::UserApc;  // Add this status code (0x000000C0)
    }

    // Check if already alerted
    if (alertable && alerted_) {
        alerted_ = false;
        process_pending_apcs();
        return Status::Alerted;  // Add this status code (0x00000101)
    }

    // Perform actual wait
    // ... existing wait implementation ...

    // After waking up, check for APCs again if alertable
    if (alertable && has_pending_apcs()) {
        process_pending_apcs();
        return Status::UserApc;
    }

    return wait_result;
}
```

### Step 4: Add status codes

In your status/error code definitions (likely in `native/include/x360mu/types.h` or a status header):

```cpp
// NT Status codes for APC handling
constexpr u32 STATUS_USER_APC = 0x000000C0;
constexpr u32 STATUS_ALERTED = 0x00000101;
```

And in the Status enum if you have one:

```cpp
enum class Status {
    // ... existing ...
    UserApc,    // Thread was alerted and APCs were processed
    Alerted,    // Thread was alerted
};
```

### Step 5: Implement KeInsertQueueApc HLE

In `native/src/kernel/hle/xboxkrnl.cpp` or `xboxkrnl_threading.cpp`, add:

```cpp
// Ordinal 108: KeInsertQueueApc
static void HLE_KeInsertQueueApc(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr apc_ptr = args[0];
    GuestAddr target_thread = args[1];
    u32 mode = static_cast<u32>(args[2]);  // KernelMode=0, UserMode=1

    // Read APC structure from guest memory
    // KAPC structure (simplified):
    // +0x00: Type (short)
    // +0x08: Thread pointer
    // +0x10: KernelRoutine
    // +0x18: RundownRoutine
    // +0x20: NormalRoutine (the actual callback)
    // +0x28: NormalContext
    // +0x30: SystemArgument1
    // +0x38: SystemArgument2

    GuestAddr routine = memory->read_u32(apc_ptr + 0x20);
    GuestAddr context = memory->read_u32(apc_ptr + 0x28);
    GuestAddr arg1 = memory->read_u32(apc_ptr + 0x30);
    GuestAddr arg2 = memory->read_u32(apc_ptr + 0x38);

    // Find the target thread
    XThread* thread = kernel->get_thread_by_handle(target_thread);
    if (!thread) {
        *result = STATUS_INVALID_HANDLE;
        return;
    }

    // Queue the APC
    thread->queue_apc(routine, context, arg1, arg2, mode == 0);

    *result = STATUS_SUCCESS;
}

// Register in HLE table
hle_functions_[make_import_key(0, 108)] = HLE_KeInsertQueueApc;
```

### Step 6: Wire up CPU reference

When creating threads, make sure to set the CPU reference:

```cpp
// In kernel thread creation
XThread* thread = new XThread(...);
thread->set_cpu(cpu_);
```

## Testing

Add a unit test in `native/tests/kernel/test_xthread.cpp`:

```cpp
TEST(XThread, AlertableWaitWithApc) {
    // Setup
    XKernel kernel;
    kernel.initialize();

    // Create a thread
    auto thread = kernel.create_thread(0x82000000, 0);
    thread->set_cpu(kernel.cpu());

    // Queue an APC
    GuestAddr apc_routine = 0x82001000;  // Dummy address
    thread->queue_apc(apc_routine, 0x100, 0x200, 0x300, false);

    // Verify APC is pending
    EXPECT_TRUE(thread->has_pending_apcs());

    // Enter alertable wait - should return immediately with UserApc
    Status result = thread->wait(1000000, true);  // alertable=true
    EXPECT_EQ(result, Status::UserApc);

    // APCs should now be processed
    EXPECT_FALSE(thread->has_pending_apcs());
}
```

## Files to Modify

- `native/src/kernel/xthread.h` - Add XApcEntry, APC queue, methods
- `native/src/kernel/xthread.cpp` - Implement APC processing, update wait()
- `native/include/x360mu/types.h` - Add STATUS_USER_APC, STATUS_ALERTED
- `native/src/kernel/hle/xboxkrnl_threading.cpp` - Add KeInsertQueueApc HLE
- `native/tests/kernel/test_xthread.cpp` - Add unit tests

## Dependencies

None - this task is independent of other blockers.

## Notes

- APCs are per-thread, not global
- Kernel-mode APCs execute immediately, user-mode only during alertable waits
- The actual APC execution requires calling guest code through the CPU
- This implementation is simplified; full implementation would handle:
  - APC rundown (cleanup when thread exits)
  - Kernel-mode APC interruption
  - Special kernel APCs
- Most games only use user-mode APCs for async I/O completion
