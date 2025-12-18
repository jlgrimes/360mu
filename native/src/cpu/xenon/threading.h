/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xbox 360 Threading Model
 * 
 * The Xbox 360 has 3 IBM Xenon cores, each with 2 hardware threads,
 * for a total of 6 hardware threads. Games can use all 6 threads
 * simultaneously, with complex synchronization requirements.
 */

#pragma once

#include "x360mu/types.h"
#include "cpu.h"
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

namespace x360mu {

class Memory;
class Kernel;

/**
 * Thread state
 */
enum class ThreadState {
    Created,        // Thread created but not started
    Ready,          // Ready to run
    Running,        // Currently executing
    Waiting,        // Waiting on synchronization object
    Suspended,      // Suspended
    Terminated,     // Thread has exited
};

/**
 * Thread priority levels
 */
enum class ThreadPriority : s32 {
    TimeCritical = 15,
    Highest = 2,
    AboveNormal = 1,
    Normal = 0,
    BelowNormal = -1,
    Lowest = -2,
    Idle = -15,
};

/**
 * CPU affinity mask bits
 */
enum CpuAffinity : u32 {
    kCore0Thread0 = 1 << 0,
    kCore0Thread1 = 1 << 1,
    kCore1Thread0 = 1 << 2,
    kCore1Thread1 = 1 << 3,
    kCore2Thread0 = 1 << 4,
    kCore2Thread1 = 1 << 5,
    kAllThreads = 0x3F,
};

/**
 * Represents a guest thread
 */
struct GuestThread {
    // Thread identification
    u32 thread_id;
    u32 handle;
    
    // CPU context
    ThreadContext context;
    
    // Thread state
    ThreadState state;
    ThreadPriority priority;
    u32 affinity_mask;
    
    // Stack info
    GuestAddr stack_base;
    u32 stack_size;
    GuestAddr stack_limit;
    
    // TLS (Thread Local Storage)
    std::array<u64, 64> tls_slots;
    
    // Suspension
    u32 suspend_count;
    
    // Wait info
    GuestAddr wait_object;
    u64 wait_timeout;
    
    // Exit code
    u32 exit_code;
    
    // Host thread (for multi-threaded execution)
    std::thread* host_thread;
    
    // Timing
    u64 execution_time;  // Total cycles executed
    u64 last_schedule_time;
    
    // Link for scheduler queues
    GuestThread* next;
    GuestThread* prev;
    
    void reset() {
        context.reset();
        state = ThreadState::Created;
        priority = ThreadPriority::Normal;
        affinity_mask = kAllThreads;
        suspend_count = 0;
        wait_object = 0;
        wait_timeout = 0;
        exit_code = 0;
        execution_time = 0;
        host_thread = nullptr;
        next = prev = nullptr;
        tls_slots.fill(0);
    }
};

/**
 * Synchronization object base
 */
struct SyncObject {
    enum class Type {
        Event,
        Semaphore,
        Mutex,
        Timer,
        Thread,
    };
    
    Type type;
    GuestAddr guest_addr;  // Address in guest memory
    bool signaled;
    
    // Waiting threads
    std::vector<GuestThread*> wait_list;
};

/**
 * Thread Scheduler
 * 
 * Manages scheduling of guest threads across host CPU cores.
 * Implements priority-based preemptive scheduling similar to Xbox 360.
 */
class ThreadScheduler {
public:
    ThreadScheduler();
    ~ThreadScheduler();
    
    /**
     * Initialize the scheduler
     */
    Status initialize(Memory* memory, Kernel* kernel, class Cpu* cpu, u32 num_host_threads = 0);
    
    /**
     * Shutdown
     */
    void shutdown();
    
    /**
     * Create a new guest thread
     */
    GuestThread* create_thread(GuestAddr entry_point, GuestAddr param,
                               u32 stack_size, u32 creation_flags);
    
    /**
     * Terminate a thread
     */
    void terminate_thread(GuestThread* thread, u32 exit_code);
    
    /**
     * Suspend thread
     */
    u32 suspend_thread(GuestThread* thread);
    
    /**
     * Resume thread
     */
    u32 resume_thread(GuestThread* thread);
    
    /**
     * Set thread priority
     */
    void set_priority(GuestThread* thread, ThreadPriority priority);
    
    /**
     * Set thread affinity
     */
    void set_affinity(GuestThread* thread, u32 affinity_mask);
    
    /**
     * Get current thread for a hardware thread
     */
    GuestThread* get_current_thread(u32 hw_thread);
    
    /**
     * Run the scheduler for one time slice
     * Returns cycles executed
     */
    u64 run(u64 cycles);
    
    /**
     * Yield current thread
     */
    void yield(GuestThread* thread);
    
    /**
     * Put thread to sleep
     */
    void sleep(GuestThread* thread, u64 nanoseconds);
    
    /**
     * Wait for synchronization object
     */
    u32 wait_for_object(GuestThread* thread, GuestAddr object, u64 timeout_ns);
    
    /**
     * Wait for multiple objects
     */
    u32 wait_for_multiple(GuestThread* thread, GuestAddr* objects, u32 count,
                          bool wait_all, u64 timeout_ns);
    
    /**
     * Signal a synchronization object
     */
    void signal_object(GuestAddr object);
    
    /**
     * Get thread by ID
     */
    GuestThread* get_thread(u32 thread_id);
    
    /**
     * Get thread by handle
     */
    GuestThread* get_thread_by_handle(u32 handle);
    
    /**
     * Get scheduler statistics
     */
    struct Stats {
        u64 total_threads_created;
        u64 context_switches;
        u64 total_cycles_executed;
        u32 active_thread_count;
        u32 ready_thread_count;
        u32 waiting_thread_count;
    };
    Stats get_stats() const;
    
private:
    Memory* memory_;
    Kernel* kernel_;
    class Cpu* cpu_;
    
    // Thread storage
    std::vector<std::unique_ptr<GuestThread>> threads_;
    std::mutex threads_mutex_;
    
    // Ready queues (one per priority level)
    static constexpr int NUM_PRIORITIES = 32;
    std::array<GuestThread*, NUM_PRIORITIES> ready_queues_;
    
    // Hardware thread state
    struct HardwareThread {
        GuestThread* current_thread;
        bool running;
        std::thread host_thread;
        std::atomic<bool> stop_flag;
        std::condition_variable wake_cv;
        std::mutex mutex;
        u64 time_slice_remaining;
    };
    std::array<HardwareThread, 6> hw_threads_;
    
    // ID generation
    std::atomic<u32> next_thread_id_{1};
    std::atomic<u32> next_handle_{0x80000100};
    
    // Time tracking
    u64 current_time_;
    static constexpr u64 TIME_SLICE = 10000;  // Cycles per time slice
    
    // Statistics
    mutable Stats stats_;
    
    // Multi-threading control
    u32 num_host_threads_ = 0;
    std::atomic<bool> running_{false};
    
    // Internal methods
    void enqueue_thread(GuestThread* thread);
    GuestThread* dequeue_thread(u32 affinity_mask);
    bool has_ready_threads(u32 affinity_mask);
    void schedule_thread(u32 hw_thread_id);
    void execute_thread(u32 hw_thread_id);
    void hw_thread_main(u32 hw_thread_id);
    
    int priority_to_queue_index(ThreadPriority priority) const;
    
    // Synchronization helpers
    SyncObject* get_sync_object(GuestAddr addr);
    void wake_waiting_threads(SyncObject* obj);
};

/**
 * Xbox 360 Kernel Object Types
 */
enum class KernelObjectType : u8 {
    NotificationEvent = 0,
    SynchronizationEvent = 1,
    Mutant = 2,
    SemaphoreObject = 5,
    TimerNotificationObject = 8,
    TimerSynchronizationObject = 9,
    ThreadObject = 6,
    ProcessObject = 3,
    QueueObject = 4,
};

/**
 * DISPATCHER_HEADER structure (from Xbox 360 kernel)
 * This is at the start of all synchronization objects
 */
struct DispatcherHeader {
    u8 type;
    u8 absolute;
    u8 size;
    u8 inserted;
    s32 signal_state;
    // Followed by wait list head
};

} // namespace x360mu

