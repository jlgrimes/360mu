/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Thread Implementation
 */

#pragma once

#include "xobject.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <atomic>

namespace x360mu {

class Cpu;
class Memory;

/**
 * Thread state
 */
enum class XThreadState {
    Created,
    Ready,
    Running,
    Waiting,
    Suspended,
    Terminated
};

/**
 * Thread priority levels (Xbox 360)
 * Values from -15 to +15, 0 is normal
 */
enum class XThreadPriority : s8 {
    TimeCritical = 15,
    Highest = 2,
    AboveNormal = 1,
    Normal = 0,
    BelowNormal = -1,
    Lowest = -2,
    Idle = -15
};

/**
 * Thread CPU affinity mask
 * Xbox 360 has 6 hardware threads across 3 cores
 */
enum XThreadAffinity : u32 {
    Core0Thread0 = 1 << 0,
    Core0Thread1 = 1 << 1,
    Core1Thread0 = 1 << 2,
    Core1Thread1 = 1 << 3,
    Core2Thread0 = 1 << 4,
    Core2Thread1 = 1 << 5,
    AllCores = 0x3F
};

/**
 * Thread Local Storage (TLS)
 */
struct XTls {
    static constexpr u32 MAX_SLOTS = 64;
    GuestAddr slots[MAX_SLOTS] = {0};
};

/**
 * XThread - Kernel thread object
 */
class XThread : public XObject {
public:
    X_OBJECT_TYPE(XThread, XObjectType::Thread);
    
    XThread(Cpu* cpu, Memory* memory);
    ~XThread() override;
    
    // Thread creation
    static std::shared_ptr<XThread> create(
        Cpu* cpu,
        Memory* memory,
        GuestAddr entry_point,
        GuestAddr parameter,
        u32 stack_size,
        u32 creation_flags,
        bool system_thread = false
    );
    
    // Thread control
    void start();
    void suspend();
    void resume();
    void terminate(u32 exit_code);
    void exit(u32 exit_code);
    
    // Wait/signal support
    bool is_signaled() const override;
    u32 wait(XObject* object, u64 timeout_100ns);
    u32 wait_multiple(XObject** objects, u32 count, bool wait_all, u64 timeout_100ns);
    void wake_from_wait(u32 result);
    
    // Delay/sleep
    void delay(u64 interval_100ns, bool alertable);
    
    // Thread properties
    u32 thread_id() const { return thread_id_; }
    XThreadState state() const { return state_; }
    XThreadPriority priority() const { return priority_; }
    void set_priority(XThreadPriority priority);
    u32 affinity_mask() const { return affinity_mask_; }
    void set_affinity(u32 mask);
    
    // CPU context
    u32 cpu_thread_id() const { return cpu_thread_id_; }
    GuestAddr entry_point() const { return entry_point_; }
    GuestAddr stack_base() const { return stack_base_; }
    u32 stack_size() const { return stack_size_; }
    
    // TLS
    XTls& tls() { return tls_; }
    GuestAddr tls_address() const { return tls_address_; }
    
    // Guest thread structure (KTHREAD)
    GuestAddr guest_thread() const { return guest_thread_; }
    
    // Exit info
    u32 exit_code() const { return exit_code_; }
    bool is_terminated() const { return state_ == XThreadState::Terminated; }
    
    // APC (Asynchronous Procedure Call) support
    void queue_apc(GuestAddr routine, GuestAddr context,
                   GuestAddr arg1 = 0, GuestAddr arg2 = 0, bool kernel_mode = false);
    void deliver_apcs();
    bool has_pending_apcs() const;
    u32 process_pending_apcs();
    void alert();
    bool is_alerted() const { return alerted_; }
    
    // System thread flag
    bool is_system_thread() const { return is_system_thread_; }
    
private:
    friend class XScheduler;
    
    Cpu* cpu_;
    Memory* memory_;
    
    // Thread identification
    u32 thread_id_;
    u32 cpu_thread_id_;  // Which CPU hardware thread this runs on
    
    // State
    std::atomic<XThreadState> state_{XThreadState::Created};
    XThreadPriority priority_ = XThreadPriority::Normal;
    u32 affinity_mask_ = AllCores;
    bool is_system_thread_ = false;
    
    // Stack
    GuestAddr entry_point_ = 0;
    GuestAddr stack_base_ = 0;
    GuestAddr stack_limit_ = 0;
    u32 stack_size_ = 0;
    
    // TLS
    XTls tls_;
    GuestAddr tls_address_ = 0;
    
    // Guest thread structure (in guest memory)
    GuestAddr guest_thread_ = 0;
    
    // Exit
    std::atomic<u32> exit_code_{0};
    
    // Wait support
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::atomic<bool> wait_satisfied_{false};
    std::atomic<u32> wait_result_{0};
    
    // Host thread (for background execution)
    std::thread host_thread_;
    std::atomic<bool> should_stop_{false};
    
    // APCs
    std::mutex apc_mutex_;
    struct Apc {
        GuestAddr routine;
        GuestAddr context;
        GuestAddr system_arg1;
        GuestAddr system_arg2;
        bool kernel_mode;
    };
    std::vector<Apc> apc_queue_;
    std::atomic<bool> alerted_{false};
    
    // Internal
    void thread_main();
    void allocate_stack();
    void allocate_tls();
    void create_guest_thread_struct();
    
    static std::atomic<u32> next_thread_id_;
};

/**
 * XScheduler - Thread scheduler
 * Manages all threads and their execution
 */
class XScheduler {
public:
    static XScheduler& instance();
    
    void initialize(Cpu* cpu, Memory* memory);
    void shutdown();
    
    // Thread management
    void add_thread(std::shared_ptr<XThread> thread);
    void remove_thread(XThread* thread);
    std::shared_ptr<XThread> get_thread(u32 thread_id);
    
    // Scheduling
    void schedule();
    void yield();
    void run_for(u64 cycles);
    
    // Wait support
    u32 wait_for_object(XThread* thread, XObject* object, u64 timeout_100ns);
    void signal_object(XObject* object);
    
    // Time management
    void advance_time(u64 cycles);
    u64 current_time() const { return current_time_; }
    
    // Process pending work
    void process_timers();
    void process_dpcs();
    
private:
    XScheduler() = default;
    
    Cpu* cpu_ = nullptr;
    Memory* memory_ = nullptr;
    
    std::mutex mutex_;
    std::vector<std::shared_ptr<XThread>> threads_;
    std::shared_ptr<XThread> current_thread_;
    
    u64 current_time_ = 0;
    
    // Ready queue (by priority)
    void enqueue_ready(XThread* thread);
    XThread* dequeue_ready();
};

} // namespace x360mu
