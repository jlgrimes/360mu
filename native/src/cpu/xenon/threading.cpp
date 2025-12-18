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
    
    // Initialize hardware thread state
    for (u32 i = 0; i < 6; i++) {
        hw_threads_[i].current_thread = nullptr;
        hw_threads_[i].running = false;
        hw_threads_[i].stop_flag = false;
        hw_threads_[i].time_slice_remaining = 0;
    }
    
    // Start host threads for multi-threaded execution
    running_ = true;
    for (u32 i = 0; i < num_host_threads_; i++) {
        hw_threads_[i].running = true;
        hw_threads_[i].stop_flag = false;
        hw_threads_[i].host_thread = std::thread(
            &ThreadScheduler::hw_thread_main, this, i
        );
        
        LOGI("Started host thread %u", i);
    }
    
    LOGI("ThreadScheduler initialized with %u active host threads", num_host_threads_);
    return Status::Ok;
}

void ThreadScheduler::shutdown() {
    LOGI("ThreadScheduler shutting down...");
    
    // Signal all threads to stop
    running_ = false;
    
    // Stop all hardware threads
    for (auto& hw : hw_threads_) {
        hw.stop_flag = true;
        hw.running = false;
        hw.wake_cv.notify_all();
    }
    
    // Wait for host threads to finish
    for (u32 i = 0; i < num_host_threads_; i++) {
        auto& hw = hw_threads_[i];
        if (hw.host_thread.joinable()) {
            LOGI("Waiting for host thread %u to finish...", i);
            hw.host_thread.join();
            LOGI("Host thread %u finished", i);
        }
    }
    
    // Clean up all threads
    std::lock_guard<std::mutex> lock(threads_mutex_);
    threads_.clear();
    
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
    
    // Find free stack space
    static GuestAddr next_stack = 0x70000000;
    thread->stack_base = next_stack;
    thread->stack_size = stack_size;
    thread->stack_limit = thread->stack_base + stack_size;
    next_stack += stack_size + memory::MEM_PAGE_SIZE;  // Guard page
    
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
    
    // Check creation flags
    if (creation_flags & 0x04) {  // CREATE_SUSPENDED
        thread->suspend_count = 1;
        thread->state = ThreadState::Suspended;
    } else {
        thread->state = ThreadState::Ready;
        enqueue_thread(thread.get());
    }
    
    LOGI("Created thread %u: entry=0x%08X, stack=0x%08X-0x%08X",
         thread->thread_id, entry_point, thread->stack_base, thread->stack_limit);
    
    stats_.total_threads_created++;
    
    GuestThread* ptr = thread.get();
    threads_.push_back(std::move(thread));
    return ptr;
}

void ThreadScheduler::terminate_thread(GuestThread* thread, u32 exit_code) {
    if (!thread) return;
    
    std::lock_guard<std::mutex> lock(threads_mutex_);
    
    thread->exit_code = exit_code;
    thread->state = ThreadState::Terminated;
    thread->context.running = false;
    
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
    
    LOGI("Terminated thread %u with exit code %u", thread->thread_id, exit_code);
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
        enqueue_thread(thread);
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

GuestThread* ThreadScheduler::dequeue_thread(u32 affinity_mask) {
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

int ThreadScheduler::priority_to_queue_index(ThreadPriority priority) const {
    // Map priority (-15 to +15) to queue index (0 to 31)
    return static_cast<int>(priority) + 16;
}

u64 ThreadScheduler::run(u64 cycles) {
    u64 total_executed = 0;
    
    // Always execute CPU thread 0 (the main game thread)
    // This is set up via cpu_->start_thread() during prepare_entry
    if (cpu_) {
        cpu_->execute_thread(0, cycles);
        total_executed = cycles;
    }
    
    // If we have scheduler guest threads (from game creating threads),
    // wake host threads to execute them in parallel
    if (num_host_threads_ > 0 && running_ && !threads_.empty()) {
        for (u32 hw = 1; hw < num_host_threads_; hw++) {
            hw_threads_[hw].wake_cv.notify_one();
        }
    }
    
    current_time_ += cycles;
    stats_.total_cycles_executed += total_executed;
    
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
        return 0;  // STATUS_SUCCESS
    }
    
    // Need to wait
    thread->state = ThreadState::Waiting;
    thread->wait_object = object;
    
    if (timeout_ns == 0) {
        return 0x00000102;  // STATUS_TIMEOUT
    }
    
    if (timeout_ns != ~0ULL) {
        thread->wait_timeout = current_time_ + (timeout_ns / 100);
    } else {
        thread->wait_timeout = ~0ULL;  // Infinite
    }
    
    // Remove from running
    for (auto& hw : hw_threads_) {
        if (hw.current_thread == thread) {
            hw.current_thread = nullptr;
            break;
        }
    }
    
    stats_.waiting_thread_count++;
    
    return 0x00000102;  // STATUS_WAIT (will be updated when signaled)
}

void ThreadScheduler::signal_object(GuestAddr object) {
    // Set signal state
    memory_->write_u32(object + 4, 1);
    
    DispatcherHeader header;
    header.type = memory_->read_u8(object);
    
    // Wake waiting threads
    std::lock_guard<std::mutex> lock(threads_mutex_);
    
    for (auto& thread : threads_) {
        if (thread->state == ThreadState::Waiting && 
            thread->wait_object == object) {
            thread->state = ThreadState::Ready;
            thread->wait_object = 0;
            enqueue_thread(thread.get());
            stats_.waiting_thread_count--;
            
            // For synchronization events, only wake one thread
            if (header.type == static_cast<u8>(KernelObjectType::SynchronizationEvent)) {
                memory_->write_u32(object + 4, 0);  // Auto-reset
                break;
            }
        }
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
            hwt.wake_cv.wait_for(lock, std::chrono::milliseconds(1), [&]() {
                return hwt.stop_flag || hwt.current_thread != nullptr || 
                       dequeue_thread(affinity_bit) != nullptr;
            });
            
            if (hwt.stop_flag) break;
            
            // Check if we have a current thread or can get one
            if (hwt.current_thread == nullptr) {
                hwt.current_thread = dequeue_thread(affinity_bit);
            }
            thread = hwt.current_thread;
        }
        
        if (!thread) continue;
        
        // Execute guest thread on the actual CPU
        thread->state = ThreadState::Running;
        thread->context.running = true;
        
        // Map guest thread to CPU hardware thread context
        // Use the thread's context.thread_id for CPU execution
        u32 cpu_thread_id = thread->context.thread_id % 6;
        
        // Execute for a time slice using the real CPU
        if (cpu_) {
            cpu_->execute_thread(cpu_thread_id, TIME_SLICE);
            thread->execution_time += TIME_SLICE;
        }
        
        thread->context.running = false;
        
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

} // namespace x360mu

