/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xbox 360 Threading Implementation
 */

#include "threading.h"
#include "cpu.h"
#include "../../memory/memory.h"
#include "../../kernel/kernel.h"
#include <algorithm>
#include <chrono>
#include <unordered_map>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-thread"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[THREAD] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[THREAD WARN] " __VA_ARGS__); printf("\n")
#endif

namespace x360mu {

// === THREAD-LOCAL STORAGE FOR 1:1 THREADING ===
// Each host thread has its own TLS slot pointing to the GuestThread it's emulating.
// This allows syscall handlers to find the correct thread context without a global
// "current thread" variable (which would be a race condition in multi-threaded code).
//
// When a 1:1 host thread starts, it sets this TLS variable.
// When a syscall happens, GetCurrentGuestThread() returns the correct thread.
thread_local GuestThread* g_current_guest_thread = nullptr;

GuestThread* GetCurrentGuestThread() {
    return g_current_guest_thread;
}

void SetCurrentGuestThread(GuestThread* thread) {
    g_current_guest_thread = thread;
}

ThreadScheduler::ThreadScheduler() {
    ready_queues_.fill(nullptr);
    stats_ = {};
}

ThreadScheduler::~ThreadScheduler() {
    shutdown();
}

Status ThreadScheduler::initialize(Memory* memory, Kernel* kernel, Cpu* cpu, u32 num_host_threads) {
    memory_ = memory;
    kernel_ = kernel;
    cpu_ = cpu;
    current_time_ = 0;
    
    // Determine number of host threads to use
    if (num_host_threads == 0) {
        // Auto-detect based on hardware
        num_host_threads = std::min(4u, std::thread::hardware_concurrency());
    }
    num_host_threads_ = std::min(num_host_threads, 6u);
    
    LOGI("ThreadScheduler: using %u host threads for %u guest hardware threads", 
         num_host_threads_, 6u);
    
    // Initialize hardware thread state (legacy, kept for compatibility)
    for (u32 i = 0; i < 6; i++) {
        hw_threads_[i].current_thread = nullptr;
        hw_threads_[i].running = false;
        hw_threads_[i].stop_flag = false;
        hw_threads_[i].time_slice_remaining = 0;
    }
    
    // === 1:1 THREADING MODEL ===
    // With 1:1 threading, we do NOT start the old hw_thread_main threads.
    // Each guest thread gets its own dedicated host thread when created.
    // The old system would run guest code from multiple host threads simultaneously,
    // causing race conditions.
    //
    // The old hw_thread_main system is DISABLED. Guest threads are executed
    // by their dedicated 1:1 host threads spawned in create_thread().
    running_ = true;
    num_host_threads_ = 0;  // Disable legacy scheduler threads
    
    LOGI("ThreadScheduler initialized with 1:1 threading model (no legacy hw_threads)");
    return Status::Ok;
}

void ThreadScheduler::shutdown() {
    LOGI("ThreadScheduler shutting down...");
    
    // Signal all threads to stop
    running_ = false;
    
    // === 1:1 THREADING MODEL: Stop all guest thread host threads ===
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (auto& thread : threads_) {
            thread->should_run.store(false);
            thread->state = ThreadState::Terminated;
            thread->signal_wake(0xC0000001);  // Wake any blocked threads
        }
    }
    
    // Wait for all 1:1 host threads to finish
    // Make a copy of thread pointers to avoid holding lock during join
    std::vector<std::thread*> threads_to_join;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        for (auto& thread : threads_) {
            if (thread->host_thread && thread->host_thread->joinable()) {
                threads_to_join.push_back(thread->host_thread.get());
            }
        }
    }
    
    for (auto* ht : threads_to_join) {
        if (ht->joinable()) {
            ht->join();
        }
    }
    LOGI("All 1:1 host threads joined");
    
    // Stop legacy hardware threads (scheduler infrastructure)
    for (auto& hw : hw_threads_) {
        hw.stop_flag = true;
        hw.running = false;
        hw.wake_cv.notify_all();
    }
    
    // Wait for legacy host threads to finish
    for (u32 i = 0; i < num_host_threads_; i++) {
        auto& hw = hw_threads_[i];
        if (hw.host_thread.joinable()) {
            LOGI("Waiting for legacy host thread %u to finish...", i);
            hw.host_thread.join();
            LOGI("Legacy host thread %u finished", i);
        }
    }
    
    // Clean up all threads
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        threads_.clear();
    }
    
    LOGI("ThreadScheduler shutdown complete");
}

GuestThread* ThreadScheduler::create_thread(GuestAddr entry_point, GuestAddr param,
                                             u32 stack_size, u32 creation_flags) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    
    // Allocate thread structure
    auto thread = std::make_unique<GuestThread>();
    thread->reset();
    
    thread->thread_id = next_thread_id_++;
    thread->handle = next_handle_++;
    thread->state = ThreadState::Created;
    thread->priority = ThreadPriority::Normal;
    thread->affinity_mask = kAllThreads;
    
    // Allocate stack
    stack_size = std::max(stack_size, 64u * 1024u);  // Minimum 64KB
    stack_size = align_up(stack_size, static_cast<u32>(memory::MEM_PAGE_SIZE));
    
    // Find free stack space (thread-safe atomic allocation)
    static std::atomic<GuestAddr> next_stack{0x70000000};
    u32 alloc_size = stack_size + memory::MEM_PAGE_SIZE;  // Include guard page
    thread->stack_base = next_stack.fetch_add(alloc_size, std::memory_order_relaxed);
    thread->stack_size = stack_size;
    thread->stack_limit = thread->stack_base + stack_size;
    
    // Allocate stack memory
    memory_->allocate(thread->stack_base, stack_size,
                      MemoryRegion::Read | MemoryRegion::Write);
    
    // Setup initial context
    thread->context.pc = entry_point;
    thread->context.gpr[1] = thread->stack_limit - 0x100;  // Stack pointer (r1)
    thread->context.gpr[3] = param;                         // First argument (r3)
    thread->context.gpr[13] = 0;                            // Small data pointer
    thread->context.lr = 0;                                 // Return to kernel on exit
    thread->context.running = false;
    thread->context.thread_id = thread->thread_id;
    thread->context.memory = memory_;  // For MMIO access
    
    // Check creation flags
    bool start_suspended = (creation_flags & 0x04) != 0;  // CREATE_SUSPENDED
    if (start_suspended) {
        thread->suspend_count = 1;
        thread->state = ThreadState::Suspended;
    } else {
        thread->state = ThreadState::Ready;
    }
    
    LOGI("Created thread %u: entry=0x%08X, stack=0x%08X-0x%08X",
         thread->thread_id, entry_point, thread->stack_base, thread->stack_limit);
    
    // === 1:1 THREADING MODEL ===
    // Each guest thread gets its own dedicated host thread.
    // The host thread runs a loop that executes guest code until the thread terminates.
    GuestThread* ptr = thread.get();
    
    if (entry_point != 0) {
        // Only spawn host thread if there's actual code to run
        thread->should_run.store(!start_suspended);
        
        // Capture needed pointers for the lambda
        Cpu* cpu = cpu_;
        Memory* memory = memory_;
        
        thread->host_thread = std::make_unique<std::thread>([ptr, cpu, memory, this]() {
            // === SET THREAD-LOCAL STORAGE ===
            // This allows syscall handlers to find this thread's context
            SetCurrentGuestThread(ptr);
            
            LOGI("1:1 Host thread started for guest thread %u (entry=0x%08X)", 
                 ptr->thread_id, (u32)ptr->context.pc);
            
            ptr->is_running.store(true);
            
            // Wait until we should run (handles CREATE_SUSPENDED)
            {
                std::unique_lock<std::mutex> lock(ptr->wait_mutex);
                ptr->wait_cv.wait(lock, [ptr]{ return ptr->should_run.load(); });
            }
            
            // Main execution loop - run until thread terminates
            while (ptr->should_run.load() && ptr->state != ThreadState::Terminated) {
                if (ptr->state == ThreadState::Waiting) {
                    // Thread is in a blocking wait - actually block here
                    std::unique_lock<std::mutex> lock(ptr->wait_mutex);
                    ptr->wait_cv.wait(lock, [ptr]{ 
                        return ptr->wait_signaled || !ptr->should_run.load() ||
                               ptr->state != ThreadState::Waiting;
                    });
                    
                    if (ptr->wait_signaled) {
                        ptr->state = ThreadState::Running;
                        ptr->wait_signaled = false;
                    }
                    continue;
                }
                
                if (ptr->state == ThreadState::Suspended) {
                    // Thread is suspended - wait for resume
                    std::unique_lock<std::mutex> lock(ptr->wait_mutex);
                    ptr->wait_cv.wait(lock, [ptr]{ 
                        return ptr->suspend_count == 0 || !ptr->should_run.load();
                    });
                    if (ptr->suspend_count == 0) {
                        ptr->state = ThreadState::Ready;
                    }
                    continue;
                }
                
                // Execute guest code
                ptr->state = ThreadState::Running;
                ptr->context.running = true;
                
                // Execute a batch of cycles
                constexpr u64 CYCLES_PER_BATCH = 10000;
                cpu->execute_with_context(ptr->thread_id, ptr->context, CYCLES_PER_BATCH);
                
                // Check if thread exited (LR=0 and PC=0 means returned from entry)
                if (ptr->context.pc == 0) {
                    LOGI("Guest thread %u returned (exit)", ptr->thread_id);
                    ptr->state = ThreadState::Terminated;
                    break;
                }
                
                ptr->execution_time += CYCLES_PER_BATCH;
                
                // Yield occasionally to other threads
                std::this_thread::yield();
            }
            
            ptr->is_running.store(false);
            ptr->context.running = false;
            LOGI("1:1 Host thread ended for guest thread %u", ptr->thread_id);
        });
    } else {
        // entry_point == 0: This is a special marker thread (like worker threads)
        // Don't spawn a host thread - these threads wait for work via other mechanisms
        LOGI("Thread %u has entry=0, no host thread spawned", thread->thread_id);
    }
    
    stats_.total_threads_created++;
    
    threads_.push_back(std::move(thread));
    return ptr;
}

void ThreadScheduler::terminate_thread(GuestThread* thread, u32 exit_code) {
    if (!thread) return;
    
    LOGI("Terminating thread %u with exit code %u", thread->thread_id, exit_code);
    
    // === 1:1 THREADING MODEL: Stop host thread ===
    thread->exit_code = exit_code;
    thread->state = ThreadState::Terminated;
    thread->context.running = false;
    thread->should_run.store(false);
    
    // Wake the thread if it's blocked in a wait
    thread->signal_wake(0xC0000001);  // STATUS_UNSUCCESSFUL
    
    // Wait for host thread to finish (don't hold threads_mutex_ during join)
    if (thread->host_thread && thread->host_thread->joinable()) {
        // Need to release lock before joining to avoid deadlock
        std::unique_ptr<std::thread> ht = std::move(thread->host_thread);
        {
            // Unlock temporarily for join
            // Note: This is a simplified approach - production code should use
            // a more robust mechanism
        }
        ht->join();
        LOGI("Host thread for guest %u joined", thread->thread_id);
    }
    
    std::lock_guard<std::mutex> lock(threads_mutex_);
    
    // Remove from any scheduler queues
    if (thread->prev) thread->prev->next = thread->next;
    if (thread->next) thread->next->prev = thread->prev;
    
    // Check if this is a current thread on any hardware thread
    for (auto& hw : hw_threads_) {
        if (hw.current_thread == thread) {
            hw.current_thread = nullptr;
        }
    }
    
    // Free stack memory
    memory_->free(thread->stack_base);
    
    LOGI("Terminated thread %u complete", thread->thread_id);
}

u32 ThreadScheduler::suspend_thread(GuestThread* thread) {
    if (!thread) return 0;
    
    u32 prev_count = thread->suspend_count;
    thread->suspend_count++;
    
    if (prev_count == 0 && thread->state == ThreadState::Ready) {
        thread->state = ThreadState::Suspended;
        // Remove from ready queue
        if (thread->prev) thread->prev->next = thread->next;
        if (thread->next) thread->next->prev = thread->prev;
        thread->next = thread->prev = nullptr;
    }
    
    return prev_count;
}

u32 ThreadScheduler::resume_thread(GuestThread* thread) {
    if (!thread || thread->suspend_count == 0) return 0;
    
    u32 prev_count = thread->suspend_count;
    thread->suspend_count--;
    
    if (thread->suspend_count == 0 && thread->state == ThreadState::Suspended) {
        thread->state = ThreadState::Ready;
        
        // === 1:1 THREADING MODEL: Wake the host thread ===
        thread->should_run.store(true);
        thread->wait_cv.notify_one();
        
        LOGI("Resumed thread %u", thread->thread_id);
    }
    
    return prev_count;
}

void ThreadScheduler::set_priority(GuestThread* thread, ThreadPriority priority) {
    if (!thread) return;
    
    bool was_ready = (thread->state == ThreadState::Ready);
    
    // Remove from current queue if ready
    if (was_ready) {
        if (thread->prev) thread->prev->next = thread->next;
        if (thread->next) thread->next->prev = thread->prev;
        thread->next = thread->prev = nullptr;
    }
    
    thread->priority = priority;
    
    // Re-add to appropriate queue
    if (was_ready) {
        enqueue_thread(thread);
    }
}

void ThreadScheduler::set_affinity(GuestThread* thread, u32 affinity_mask) {
    if (thread) {
        thread->affinity_mask = affinity_mask & kAllThreads;
        if (thread->affinity_mask == 0) {
            thread->affinity_mask = kAllThreads;  // Default to all
        }
    }
}

GuestThread* ThreadScheduler::get_current_thread(u32 hw_thread) {
    if (hw_thread < 6) {
        return hw_threads_[hw_thread].current_thread;
    }
    return nullptr;
}

void ThreadScheduler::enqueue_thread(GuestThread* thread) {
    if (!thread || thread->state != ThreadState::Ready) return;
    
    std::lock_guard<std::mutex> lock(ready_queues_mutex_);
    
    int queue_idx = priority_to_queue_index(thread->priority);
    
    // Add to tail of queue
    thread->next = nullptr;
    thread->prev = nullptr;
    
    if (ready_queues_[queue_idx] == nullptr) {
        ready_queues_[queue_idx] = thread;
    } else {
        GuestThread* tail = ready_queues_[queue_idx];
        while (tail->next) tail = tail->next;
        tail->next = thread;
        thread->prev = tail;
    }
    
    stats_.ready_thread_count++;
}

// Internal helper: dequeue without locking (caller must hold ready_queues_mutex_)
GuestThread* ThreadScheduler::dequeue_thread_unlocked(u32 affinity_mask) {
    // Find highest priority thread that matches affinity
    for (int i = NUM_PRIORITIES - 1; i >= 0; i--) {
        GuestThread* thread = ready_queues_[i];
        GuestThread* prev = nullptr;
        
        while (thread) {
            if (thread->affinity_mask & affinity_mask) {
                // Remove from queue
                if (prev) {
                    prev->next = thread->next;
                } else {
                    ready_queues_[i] = thread->next;
                }
                if (thread->next) {
                    thread->next->prev = prev;
                }
                thread->next = thread->prev = nullptr;
                
                stats_.ready_thread_count--;
                return thread;
            }
            prev = thread;
            thread = thread->next;
        }
    }
    return nullptr;
}

GuestThread* ThreadScheduler::dequeue_thread(u32 affinity_mask) {
    std::lock_guard<std::mutex> lock(ready_queues_mutex_);
    return dequeue_thread_unlocked(affinity_mask);
}

// Internal helper: check without locking (caller must hold ready_queues_mutex_)
bool ThreadScheduler::has_ready_threads_unlocked(u32 affinity_mask) const {
    // Check if there are any ready threads matching the affinity
    for (int i = NUM_PRIORITIES - 1; i >= 0; i--) {
        GuestThread* thread = ready_queues_[i];
        while (thread) {
            if (thread->affinity_mask & affinity_mask) {
                return true;
            }
            thread = thread->next;
        }
    }
    return false;
}

bool ThreadScheduler::has_ready_threads(u32 affinity_mask) {
    std::lock_guard<std::mutex> lock(ready_queues_mutex_);
    return has_ready_threads_unlocked(affinity_mask);
}

int ThreadScheduler::priority_to_queue_index(ThreadPriority priority) const {
    // Map priority (-15 to +15) to queue index (0 to 31)
    return static_cast<int>(priority) + 16;
}

u64 ThreadScheduler::run(u64 cycles) {
    u64 total_executed = 0;
    
    // === 1:1 THREADING MODEL ===
    // With 1:1 threading, each guest thread has its own dedicated host thread.
    // This run() function should NOT execute guest code directly - the 1:1 host
    // threads handle that. We just need to:
    // 1. Process timers/DPCs
    // 2. Track time
    // 3. NOT duplicate execution via the old hw_thread_main system
    //
    // The old code would call cpu_->execute_thread() AND have hw_thread_main
    // threads, causing the same guest code to run from multiple host threads
    // simultaneously - a race condition!
    
    // Just track time - the 1:1 host threads handle actual execution
    current_time_ += cycles;
    stats_.total_cycles_executed += cycles;
    total_executed = cycles;
    
    return total_executed;
}

void ThreadScheduler::yield(GuestThread* thread) {
    if (!thread) return;
    
    // Put back in ready queue
    thread->state = ThreadState::Ready;
    enqueue_thread(thread);
    
    // Clear from hardware thread
    for (auto& hw : hw_threads_) {
        if (hw.current_thread == thread) {
            hw.current_thread = nullptr;
            hw.time_slice_remaining = 0;
            break;
        }
    }
}

void ThreadScheduler::sleep(GuestThread* thread, u64 nanoseconds) {
    if (!thread) return;
    
    thread->state = ThreadState::Waiting;
    thread->wait_timeout = current_time_ + (nanoseconds / 100);  // Convert to ~cycles
    
    // TODO: Add to timer queue for wakeup
}

u32 ThreadScheduler::wait_for_object(GuestThread* thread, GuestAddr object, u64 timeout_ns) {
    if (!thread) return 0xC0000001;  // STATUS_UNSUCCESSFUL
    
    // Read dispatcher header from object
    DispatcherHeader header;
    header.type = memory_->read_u8(object);
    header.signal_state = memory_->read_u32(object + 4);
    
    // Check if already signaled
    if (header.signal_state > 0) {
        // For auto-reset events, clear the signal
        if (header.type == static_cast<u8>(KernelObjectType::SynchronizationEvent)) {
            memory_->write_u32(object + 4, 0);
        }
        return 0;  // STATUS_SUCCESS / STATUS_WAIT_0
    }
    
    // Zero timeout means just check, don't wait
    if (timeout_ns == 0) {
        return 0x00000102;  // STATUS_TIMEOUT
    }
    
    // === 1:1 THREADING MODEL: REAL BLOCKING ===
    // Actually block the host thread using condition variable.
    // This is the key change from cooperative scheduling.
    
    static int wait_log = 0;
    if (wait_log++ < 20) {
        LOGI("wait_for_object: thread %u blocking on object 0x%08X (timeout=%llu ns)",
             thread->thread_id, (u32)object, timeout_ns);
    }
    
    // Mark thread as waiting
    thread->state = ThreadState::Waiting;
    thread->wait_object = object;
    thread->wait_signaled = false;
    
    stats_.waiting_thread_count++;
    
    // Convert timeout
    u64 timeout_ms = (timeout_ns == ~0ULL) ? 0 : (timeout_ns / 1000000);
    
    // ACTUALLY BLOCK using the thread's condition variable
    // The thread will be woken when signal_object() is called
    u32 result = thread->block_until_signaled(timeout_ms);
    
    stats_.waiting_thread_count--;
    
    if (result == 0) {
        // Successfully signaled
        // For auto-reset events, clear the signal
        if (header.type == static_cast<u8>(KernelObjectType::SynchronizationEvent)) {
            memory_->write_u32(object + 4, 0);
        }
    }
    
    thread->wait_object = 0;
    return result;
}

void ThreadScheduler::signal_object(GuestAddr object) {
    DispatcherHeader header;
    header.type = memory_->read_u8(object);
    
    // Set signal state in guest memory
    constexpr u8 SEMAPHORE_TYPE = 5;
    if (header.type != SEMAPHORE_TYPE) {
        memory_->write_u32(object + 4, 1);
    }
    
    // === 1:1 THREADING MODEL: REAL WAKE ===
    // Actually wake blocked threads using their condition variables.
    
    std::lock_guard<std::mutex> lock(threads_mutex_);
    
    int woken_count = 0;
    for (auto& thread : threads_) {
        if (thread->state == ThreadState::Waiting && 
            thread->wait_object == object) {
            
            // ACTUALLY WAKE the thread using its condition variable
            thread->signal_wake(0);  // STATUS_SUCCESS
            thread->state = ThreadState::Ready;
            woken_count++;
            
            static int wake_log = 0;
            if (wake_log++ < 20) {
                LOGI("signal_object: WOKE thread %u from wait on 0x%08X",
                     thread->thread_id, (u32)object);
            }
            
            // For synchronization events, only wake one thread and auto-reset
            if (header.type == static_cast<u8>(KernelObjectType::SynchronizationEvent)) {
                memory_->write_u32(object + 4, 0);  // Auto-reset
                break;
            }
            
            // For semaphores, decrement count for each thread woken
            if (header.type == SEMAPHORE_TYPE) {
                s32 count = static_cast<s32>(memory_->read_u32(object + 4));
                if (count > 0) {
                    memory_->write_u32(object + 4, static_cast<u32>(count - 1));
                }
                if (count <= 1) {
                    break;  // No more resources available
                }
            }
        }
    }
    
    static int signal_log = 0;
    if (signal_log++ < 30) {
        LOGI("signal_object: object=0x%08X, type=%u, woken=%d threads",
             (u32)object, header.type, woken_count);
    }
}

GuestThread* ThreadScheduler::get_thread(u32 thread_id) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (auto& thread : threads_) {
        if (thread->thread_id == thread_id) {
            return thread.get();
        }
    }
    return nullptr;
}

GuestThread* ThreadScheduler::get_thread_by_handle(u32 handle) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (auto& thread : threads_) {
        if (thread->handle == handle) {
            return thread.get();
        }
    }
    return nullptr;
}

ThreadScheduler::Stats ThreadScheduler::get_stats() const {
    stats_.active_thread_count = 0;
    for (const auto& thread : threads_) {
        if (thread->state != ThreadState::Terminated) {
            stats_.active_thread_count++;
        }
    }
    return stats_;
}

void ThreadScheduler::hw_thread_main(u32 hw_thread_id) {
    auto& hwt = hw_threads_[hw_thread_id];
    u32 affinity_bit = 1u << hw_thread_id;
    
    LOGI("Hardware thread %u started (affinity=0x%X)", hw_thread_id, affinity_bit);
    
    while (!hwt.stop_flag && running_) {
        GuestThread* thread = nullptr;
        
        // Try to get a thread to execute
        {
            std::unique_lock<std::mutex> lock(hwt.mutex);
            
            // Wait for work or stop signal
            // Atomic check: acquire queue lock inside predicate to avoid TOCTOU race
            hwt.wake_cv.wait_for(lock, std::chrono::milliseconds(1), [&]() {
                if (hwt.stop_flag || hwt.current_thread != nullptr) {
                    return true;
                }
                // Atomically check AND dequeue to prevent TOCTOU race
                std::lock_guard<std::mutex> queue_lock(ready_queues_mutex_);
                if (has_ready_threads_unlocked(affinity_bit)) {
                    // Immediately dequeue while holding the lock
                    hwt.current_thread = dequeue_thread_unlocked(affinity_bit);
                    return hwt.current_thread != nullptr;
                }
                return false;
            });
            
            if (hwt.stop_flag) break;
            
            thread = hwt.current_thread;
        }
        
        if (!thread) continue;
        
        // Check if this is a worker thread
        if (thread->is_worker_thread) {
            // Worker threads process work queue items instead of running guest code
            thread->state = ThreadState::Running;
            
            // Process work items from the queue
            bool did_work = process_worker_thread(thread);
            
            if (!did_work) {
                // No work available - put thread back in ready state
                // and yield to avoid busy spinning
                thread->state = ThreadState::Ready;
                std::this_thread::yield();
            }
        } else {
            // Normal guest thread - execute guest code
            thread->state = ThreadState::Running;
            
            // Map guest thread to CPU hardware thread context
            // Use the thread's context.thread_id for CPU execution
            u32 cpu_thread_id = thread->context.thread_id % 6;
            
            // Execute for a time slice using the real CPU
            // Use execute_with_context for proper context synchronization and thread safety
            if (cpu_) {
                cpu_->execute_with_context(cpu_thread_id, thread->context, TIME_SLICE);
                thread->execution_time += TIME_SLICE;
            }
        }
        
        // Put thread back in ready queue if it's still runnable
        {
            std::lock_guard<std::mutex> lock(hwt.mutex);
            if (thread->state == ThreadState::Running) {
                thread->state = ThreadState::Ready;
                hwt.current_thread = nullptr;
                hwt.time_slice_remaining = 0;
                enqueue_thread(thread);
            } else if (thread->state == ThreadState::Waiting ||
                       thread->state == ThreadState::Terminated) {
                hwt.current_thread = nullptr;
            }
        }
        
        stats_.context_switches++;
    }
    
    LOGI("Hardware thread %u stopped", hw_thread_id);
}

void ThreadScheduler::schedule_thread(u32 hw_thread_id) {
    if (hw_thread_id >= 6) return;
    
    auto& hwt = hw_threads_[hw_thread_id];
    u32 affinity_bit = 1u << hw_thread_id;
    
    // Put current thread back if still running
    if (hwt.current_thread && hwt.current_thread->state == ThreadState::Running) {
        hwt.current_thread->state = ThreadState::Ready;
        enqueue_thread(hwt.current_thread);
    }
    
    // Get next thread
    hwt.current_thread = dequeue_thread(affinity_bit);
    if (hwt.current_thread) {
        hwt.current_thread->state = ThreadState::Running;
        hwt.time_slice_remaining = TIME_SLICE;
    }
}

void ThreadScheduler::execute_thread(u32 hw_thread_id) {
    // Wake up the hardware thread to execute
    if (hw_thread_id < num_host_threads_) {
        hw_threads_[hw_thread_id].wake_cv.notify_one();
    }
}

//=============================================================================
// APC (Asynchronous Procedure Call) Support
//=============================================================================

void ThreadScheduler::queue_apc(GuestThread* thread, GuestAddr routine, GuestAddr context,
                                 GuestAddr arg1, GuestAddr arg2, bool kernel_mode) {
    if (!thread) return;
    
    thread->queue_apc(routine, context, arg1, arg2, kernel_mode);
    
    LOGD("Queued APC to thread %u: routine=0x%08X, context=0x%08X, kernel=%d",
         thread->thread_id, routine, context, kernel_mode);
    
    // If kernel-mode APC, alert the thread
    if (kernel_mode) {
        alert_thread(thread);
    }
}

u32 ThreadScheduler::process_pending_apcs(GuestThread* thread) {
    if (!thread || !cpu_) {
        LOGW("Cannot process APCs: invalid thread or no CPU");
        return 0;
    }
    
    u32 count = 0;
    
    while (true) {
        ApcEntry apc;
        
        // Get next APC from queue
        {
            std::lock_guard<std::mutex> lock(thread->apc_mutex);
            if (thread->apc_queue.empty()) {
                break;
            }
            apc = thread->apc_queue.front();
            thread->apc_queue.pop_front();
        }
        
        LOGI("Executing APC for thread %u: routine=0x%08X, context=0x%08X",
             thread->thread_id, apc.routine, apc.context);
        
        // Call the APC routine by setting up the thread's context
        // APC signature: void ApcRoutine(PVOID context, PVOID arg1, PVOID arg2)
        
        // Save current PC/LR
        GuestAddr saved_pc = thread->context.pc;
        GuestAddr saved_lr = thread->context.lr;
        u64 saved_r3 = thread->context.gpr[3];
        u64 saved_r4 = thread->context.gpr[4];
        u64 saved_r5 = thread->context.gpr[5];
        
        // Set up APC call
        thread->context.gpr[3] = apc.context;       // First argument (context)
        thread->context.gpr[4] = apc.system_arg1;   // Second argument
        thread->context.gpr[5] = apc.system_arg2;   // Third argument
        thread->context.lr = saved_pc;              // Return to where we were
        thread->context.pc = apc.routine;           // Jump to APC routine
        
        // Execute the APC routine
        // We execute for a limited number of cycles to prevent infinite loops
        // The APC should return via blr which will restore PC to saved_pc
        u32 cpu_thread_id = thread->context.thread_id % 6;
        cpu_->execute_thread(cpu_thread_id, 100000);  // Execute up to 100K cycles
        
        // If the APC didn't return properly, restore state
        // (In practice, the APC should have executed blr to return)
        if (thread->context.pc != saved_pc) {
            LOGW("APC routine didn't return properly, forcing return");
            thread->context.pc = saved_pc;
            thread->context.lr = saved_lr;
            thread->context.gpr[3] = saved_r3;
            thread->context.gpr[4] = saved_r4;
            thread->context.gpr[5] = saved_r5;
        }
        
        count++;
    }
    
    // Clear alerted flag after processing APCs
    thread->alerted = false;
    thread->in_alertable_wait = false;
    
    return count;
}

void ThreadScheduler::alert_thread(GuestThread* thread) {
    if (!thread) return;
    
    thread->alert();
    
    // If thread is in an alertable wait, wake it up
    if (thread->in_alertable_wait && thread->state == ThreadState::Waiting) {
        thread->state = ThreadState::Ready;
        thread->wait_object = 0;
        enqueue_thread(thread);
        stats_.waiting_thread_count--;
        
        LOGD("Alerted thread %u from wait", thread->thread_id);
    }
}

bool ThreadScheduler::process_worker_thread(GuestThread* thread) {
    if (!thread || !thread->is_worker_thread) {
        return false;
    }
    
    // Try to dequeue a work item (non-blocking, short timeout)
    WorkQueueItem item;
    if (!WorkQueueManager::instance().dequeue(thread->worker_queue_type, item, 10)) {
        // No work available
        return false;
    }
    
    LOGI("Worker thread %u processing work item: routine=0x%08X, param=0x%08X",
         thread->thread_id, (u32)item.worker_routine, (u32)item.parameter);
    
    // Validate routine pointer
    if (item.worker_routine == 0 || item.worker_routine < 0x80000000) {
        LOGW("Worker thread %u: invalid routine pointer 0x%08X", 
             thread->thread_id, (u32)item.worker_routine);
        return false;
    }
    
    // Save current context (in case worker was doing something)
    ThreadContext saved_ctx = thread->context;
    
    // Set up context to call the worker routine
    // Worker routine signature: void WorkerRoutine(PVOID Parameter)
    thread->context.pc = item.worker_routine;
    thread->context.gpr[3] = item.parameter;  // r3 = first parameter
    thread->context.lr = 0;                    // Return address 0 = routine done
    thread->context.running = true;
    thread->state = ThreadState::Running;
    
    // Execute the worker routine until it returns (pc == 0 after blr to LR=0)
    // or we hit a cycle limit to prevent infinite loops
    constexpr u64 MAX_WORKER_CYCLES = 5000000;  // 5M cycles max per work item
    u64 cycles_executed = 0;
    constexpr u64 CYCLES_PER_BATCH = 50000;
    
    while (thread->context.pc != 0 && 
           thread->state == ThreadState::Running &&
           cycles_executed < MAX_WORKER_CYCLES) {
        
        // Execute a batch of cycles
        u32 cpu_thread_id = thread->context.thread_id % 6;
        cpu_->execute_with_context(cpu_thread_id, thread->context, CYCLES_PER_BATCH);
        cycles_executed += CYCLES_PER_BATCH;
        
        // Check if routine returned (blr to LR=0 sets PC=0)
        if (thread->context.pc == 0) {
            break;
        }
        
        // Check if thread got blocked on a wait
        if (thread->state == ThreadState::Waiting) {
            LOGD("Worker routine 0x%08X blocked on wait, continuing later", 
                 (u32)item.worker_routine);
            break;
        }
    }
    
    if (cycles_executed >= MAX_WORKER_CYCLES && thread->context.pc != 0) {
        LOGW("Worker routine 0x%08X hit cycle limit, forcing completion", 
             (u32)item.worker_routine);
    }
    
    LOGI("Worker thread %u completed work item (routine=0x%08X, cycles=%llu)", 
         thread->thread_id, (u32)item.worker_routine, (unsigned long long)cycles_executed);
    
    // Update execution time
    thread->execution_time += cycles_executed;
    
    // Restore context for next work item
    // Keep the thread in Ready state so it can process more work
    thread->context = saved_ctx;
    thread->context.pc = 0;  // Worker thread doesn't have guest entry point
    thread->state = ThreadState::Ready;
    
    return true;
}

} // namespace x360mu

