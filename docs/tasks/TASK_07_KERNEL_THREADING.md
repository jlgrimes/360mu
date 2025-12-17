# Task: Kernel Threading & Synchronization HLE

## Priority: 🟡 HIGH (Required for Multi-threaded Games)
## Estimated Time: 3-4 weeks
## Dependencies: None

---

## Objective

Implement Xbox 360 kernel thread management and synchronization primitives so multi-threaded games work correctly.

---

## What To Build

### Location
- `native/src/kernel/hle/xboxkrnl_threading.cpp`
- `native/src/kernel/threading.cpp`

---

## Part 1: Thread Management

### 1.1 ExCreateThread

```cpp
// Create a new thread
NTSTATUS HLE_ExCreateThread(
    HANDLE* ThreadHandle,         // OUT: thread handle
    DWORD StackSize,              // Stack size
    DWORD* ThreadId,              // OUT: thread ID
    void* XapiThreadStartup,      // Startup wrapper
    void* StartAddress,           // Thread entry point
    void* StartParameter,         // Argument to entry point
    DWORD CreationFlags           // CREATE_SUSPENDED, etc.
) {
    // Allocate thread context
    auto thread = std::make_unique<EmulatedThread>();
    thread->ctx.reset();
    thread->ctx.gpr[1] = allocate_stack(StackSize);  // r1 = stack pointer
    thread->ctx.gpr[3] = (u64)StartParameter;        // r3 = first argument
    thread->ctx.pc = (u64)StartAddress;
    thread->ctx.lr = THREAD_EXIT_STUB;               // Return address
    
    // Set processor affinity
    thread->processor = next_processor_++;
    if (next_processor_ >= 6) next_processor_ = 0;
    
    // Assign thread ID
    thread->id = ++next_thread_id_;
    *ThreadId = thread->id;
    
    // Start thread (unless CREATE_SUSPENDED)
    if (!(CreationFlags & CREATE_SUSPENDED)) {
        thread->state = ThreadState::Running;
        schedule_thread(thread.get());
    } else {
        thread->state = ThreadState::Suspended;
    }
    
    *ThreadHandle = kernel_->create_handle(std::move(thread));
    return STATUS_SUCCESS;
}
```

### 1.2 Thread Scheduling

```cpp
class ThreadScheduler {
public:
    void schedule_thread(EmulatedThread* thread);
    void yield_thread();
    void sleep_thread(u32 milliseconds);
    void suspend_thread(EmulatedThread* thread);
    void resume_thread(EmulatedThread* thread);
    void terminate_thread(EmulatedThread* thread, u32 exit_code);
    
    // Run one timeslice on current thread
    void run_timeslice();
    
private:
    std::vector<EmulatedThread*> ready_queue_;
    EmulatedThread* current_thread_ = nullptr;
    std::mutex scheduler_lock_;
};

void ThreadScheduler::run_timeslice() {
    if (ready_queue_.empty()) return;
    
    // Round-robin scheduling
    current_thread_ = ready_queue_.front();
    ready_queue_.erase(ready_queue_.begin());
    
    // Execute ~1000 instructions
    interpreter_->execute(current_thread_->ctx, 1000);
    
    // Re-queue if still running
    if (current_thread_->state == ThreadState::Running) {
        ready_queue_.push_back(current_thread_);
    }
}
```

### 1.3 Other Thread Functions

```cpp
// Suspend a thread
DWORD HLE_NtSuspendThread(HANDLE ThreadHandle, DWORD* PreviousSuspendCount) {
    auto thread = kernel_->get_thread(ThreadHandle);
    *PreviousSuspendCount = thread->suspend_count;
    thread->suspend_count++;
    scheduler_->suspend_thread(thread);
    return STATUS_SUCCESS;
}

// Resume a thread
DWORD HLE_NtResumeThread(HANDLE ThreadHandle, DWORD* SuspendCount) {
    auto thread = kernel_->get_thread(ThreadHandle);
    *SuspendCount = thread->suspend_count;
    if (thread->suspend_count > 0) {
        thread->suspend_count--;
        if (thread->suspend_count == 0) {
            scheduler_->resume_thread(thread);
        }
    }
    return STATUS_SUCCESS;
}

// Terminate thread
void HLE_NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus) {
    auto thread = kernel_->get_thread(ThreadHandle);
    scheduler_->terminate_thread(thread, ExitStatus);
}

// Get current thread ID
DWORD HLE_KeGetCurrentProcessorNumber() {
    return scheduler_->current_thread()->processor;
}
```

---

## Part 2: Synchronization Primitives

### 2.1 Events

```cpp
// Create event
NTSTATUS HLE_NtCreateEvent(
    HANDLE* EventHandle,
    ACCESS_MASK DesiredAccess,
    OBJECT_ATTRIBUTES* ObjectAttributes,
    EVENT_TYPE EventType,        // NotificationEvent or SynchronizationEvent
    BOOLEAN InitialState
) {
    auto event = std::make_unique<KernelEvent>();
    event->type = EventType;
    event->signaled = InitialState;
    
    *EventHandle = kernel_->create_handle(std::move(event));
    return STATUS_SUCCESS;
}

// Set event (signal)
NTSTATUS HLE_NtSetEvent(HANDLE EventHandle, LONG* PreviousState) {
    auto event = kernel_->get_event(EventHandle);
    if (PreviousState) *PreviousState = event->signaled;
    
    event->signaled = true;
    
    // Wake waiting threads
    wake_waiters(event);
    
    return STATUS_SUCCESS;
}

// Clear event
NTSTATUS HLE_NtClearEvent(HANDLE EventHandle) {
    auto event = kernel_->get_event(EventHandle);
    event->signaled = false;
    return STATUS_SUCCESS;
}
```

### 2.2 Semaphores

```cpp
NTSTATUS HLE_NtCreateSemaphore(
    HANDLE* SemaphoreHandle,
    ACCESS_MASK DesiredAccess,
    OBJECT_ATTRIBUTES* ObjectAttributes,
    LONG InitialCount,
    LONG MaximumCount
) {
    auto sem = std::make_unique<KernelSemaphore>();
    sem->count = InitialCount;
    sem->max_count = MaximumCount;
    
    *SemaphoreHandle = kernel_->create_handle(std::move(sem));
    return STATUS_SUCCESS;
}

NTSTATUS HLE_NtReleaseSemaphore(
    HANDLE SemaphoreHandle,
    LONG ReleaseCount,
    LONG* PreviousCount
) {
    auto sem = kernel_->get_semaphore(SemaphoreHandle);
    if (PreviousCount) *PreviousCount = sem->count;
    
    sem->count = std::min(sem->count + ReleaseCount, sem->max_count);
    wake_waiters(sem);
    
    return STATUS_SUCCESS;
}
```

### 2.3 Mutexes / Critical Sections

```cpp
// Fast user-mode critical section
void HLE_RtlInitializeCriticalSection(RTL_CRITICAL_SECTION* cs) {
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    cs->LockSemaphore = 0;
    cs->SpinCount = 0;
}

void HLE_RtlEnterCriticalSection(RTL_CRITICAL_SECTION* cs) {
    DWORD tid = scheduler_->current_thread()->id;
    
    // Try to acquire
    if (InterlockedIncrement(&cs->LockCount) == 0) {
        cs->OwningThread = tid;
        cs->RecursionCount = 1;
        return;
    }
    
    // Already owned by us? (recursive)
    if (cs->OwningThread == tid) {
        cs->RecursionCount++;
        return;
    }
    
    // Wait for lock
    while (true) {
        if (InterlockedCompareExchange(&cs->LockCount, 0, -1) == -1) {
            cs->OwningThread = tid;
            cs->RecursionCount = 1;
            return;
        }
        scheduler_->yield_thread();
    }
}

void HLE_RtlLeaveCriticalSection(RTL_CRITICAL_SECTION* cs) {
    if (--cs->RecursionCount == 0) {
        cs->OwningThread = 0;
        InterlockedDecrement(&cs->LockCount);
    }
}
```

### 2.4 Wait Functions

```cpp
NTSTATUS HLE_NtWaitForSingleObject(
    HANDLE Handle,
    BOOLEAN Alertable,
    LARGE_INTEGER* Timeout
) {
    auto obj = kernel_->get_waitable(Handle);
    
    // Calculate timeout
    u64 timeout_ns = Timeout ? Timeout->QuadPart * 100 : INFINITE;
    auto deadline = std::chrono::steady_clock::now() + 
                    std::chrono::nanoseconds(timeout_ns);
    
    while (!obj->is_signaled()) {
        if (timeout_ns != INFINITE && 
            std::chrono::steady_clock::now() >= deadline) {
            return STATUS_TIMEOUT;
        }
        
        // Add to wait list
        obj->add_waiter(scheduler_->current_thread());
        scheduler_->yield_thread();
    }
    
    // Auto-reset for synchronization events
    if (obj->type == ObjectType::SynchronizationEvent) {
        static_cast<KernelEvent*>(obj)->signaled = false;
    }
    
    return STATUS_SUCCESS;
}

NTSTATUS HLE_NtWaitForMultipleObjects(
    ULONG Count,
    HANDLE* Handles,
    WAIT_TYPE WaitType,  // WaitAll or WaitAny
    BOOLEAN Alertable,
    LARGE_INTEGER* Timeout
) {
    // Similar to single wait, but check multiple objects
    // WaitAll: all must be signaled
    // WaitAny: any one must be signaled
}
```

---

## Part 3: Thread Local Storage (TLS)

```cpp
// Allocate TLS slot
DWORD HLE_TlsAlloc() {
    for (DWORD i = 0; i < 64; i++) {
        if (!tls_slots_used_[i]) {
            tls_slots_used_[i] = true;
            return i;
        }
    }
    return TLS_OUT_OF_INDEXES;
}

// Get TLS value
void* HLE_TlsGetValue(DWORD Index) {
    return scheduler_->current_thread()->tls[Index];
}

// Set TLS value
BOOL HLE_TlsSetValue(DWORD Index, void* Value) {
    scheduler_->current_thread()->tls[Index] = Value;
    return TRUE;
}

// Free TLS slot
BOOL HLE_TlsFree(DWORD Index) {
    tls_slots_used_[Index] = false;
    return TRUE;
}
```

---

## Test Cases

```cpp
TEST(ThreadingTest, CreateThread) {
    Kernel kernel;
    HANDLE thread;
    DWORD tid;
    
    NTSTATUS status = kernel.syscall_ExCreateThread(
        &thread, 64*1024, &tid, nullptr,
        (void*)0x10000, nullptr, 0
    );
    
    EXPECT_EQ(status, STATUS_SUCCESS);
    EXPECT_NE(tid, 0);
}

TEST(ThreadingTest, EventSignaling) {
    Kernel kernel;
    HANDLE event;
    
    kernel.syscall_NtCreateEvent(&event, 0, nullptr, 
                                  SynchronizationEvent, FALSE);
    
    // Event should not be signaled
    NTSTATUS status = kernel.syscall_NtWaitForSingleObject(
        event, FALSE, &zero_timeout);
    EXPECT_EQ(status, STATUS_TIMEOUT);
    
    // Signal it
    kernel.syscall_NtSetEvent(event, nullptr);
    
    // Now should succeed
    status = kernel.syscall_NtWaitForSingleObject(
        event, FALSE, &zero_timeout);
    EXPECT_EQ(status, STATUS_SUCCESS);
}
```

---

## Do NOT Touch

- File I/O (separate task)
- Memory management (existing)
- GPU/Audio code

---

## Success Criteria

1. ✅ Create and start threads
2. ✅ Thread scheduling works (round-robin)
3. ✅ Events can be created, set, and waited on
4. ✅ Critical sections work for mutual exclusion
5. ✅ TLS allocation and access works

---

*This task handles threading and synchronization. File I/O and other kernel functions are separate.*

