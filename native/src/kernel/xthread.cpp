/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Thread Implementation
 */

#include "xthread.h"
#include "xobject.h"
#include "../cpu/xenon/cpu.h"
#include "../memory/memory.h"
#include <algorithm>
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-xthread"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[XTHREAD] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[XTHREAD WARN] " __VA_ARGS__); printf("\n")
#endif

namespace x360mu {

std::atomic<u32> XThread::next_thread_id_{1};

//=============================================================================
// XThread
//=============================================================================

XThread::XThread(Cpu* cpu, Memory* memory)
    : XObject(XObjectType::Thread)
    , cpu_(cpu)
    , memory_(memory)
    , thread_id_(next_thread_id_.fetch_add(1))
    , cpu_thread_id_(0)
{
}

XThread::~XThread() {
    // Signal anyone waiting on this thread
    wake_waiters();
    
    // Stop host thread if running
    should_stop_ = true;
    wait_cv_.notify_all();
    
    if (host_thread_.joinable()) {
        host_thread_.join();
    }
    
    // Free stack memory
    if (stack_base_ != 0) {
        memory_->free(stack_base_);
    }
}

std::shared_ptr<XThread> XThread::create(
    Cpu* cpu,
    Memory* memory,
    GuestAddr entry_point,
    GuestAddr parameter,
    u32 stack_size,
    u32 creation_flags,
    bool system_thread)
{
    auto thread = std::make_shared<XThread>(cpu, memory);
    
    thread->entry_point_ = entry_point;
    thread->stack_size_ = std::max(stack_size, 64u * 1024u);  // Minimum 64KB
    thread->is_system_thread_ = system_thread;
    
    // Allocate stack
    thread->allocate_stack();
    
    // Allocate TLS
    thread->allocate_tls();
    
    // Create guest KTHREAD structure
    thread->create_guest_thread_struct();
    
    // Assign to a CPU hardware thread (round-robin)
    thread->cpu_thread_id_ = thread->thread_id_ % 6;
    
    // Set initial CPU state
    auto& ctx = cpu->get_context(thread->cpu_thread_id_);
    ctx.pc = entry_point;
    ctx.gpr[1] = thread->stack_limit_ - 0x100;  // Stack pointer (r1)
    ctx.gpr[3] = parameter;                      // First argument (r3)
    ctx.gpr[13] = thread->tls_address_;          // TLS pointer (r13)
    ctx.lr = 0;                                   // Return to kernel
    ctx.thread_id = thread->cpu_thread_id_;
    ctx.running = false;
    
    // Check creation flags
    if (creation_flags & 0x04) {  // CREATE_SUSPENDED
        thread->state_ = XThreadState::Suspended;
    } else {
        thread->state_ = XThreadState::Ready;
    }
    
    LOGI("Created XThread %u: entry=0x%08X, stack=0x%08X-0x%08X, cpu=%u",
         thread->thread_id_, entry_point, thread->stack_base_, 
         thread->stack_limit_, thread->cpu_thread_id_);
    
    return thread;
}

void XThread::allocate_stack() {
    // Align stack size to page boundary
    stack_size_ = (stack_size_ + memory::MEM_PAGE_SIZE - 1) & ~(memory::MEM_PAGE_SIZE - 1);
    
    // Allocate stack in physical memory (first 512MB)
    // Use addresses starting at 16MB mark to avoid kernel structures
    static GuestAddr next_stack = 0x01000000;  // 16MB
    stack_base_ = next_stack;
    stack_limit_ = stack_base_ + stack_size_;
    next_stack += stack_size_ + memory::MEM_PAGE_SIZE;  // Guard page
    
    // Memory should already be mapped, just zero it
    for (u32 i = 0; i < stack_size_; i += 4) {
        memory_->write_u32(stack_base_ + i, 0);
    }
}

void XThread::allocate_tls() {
    // Allocate TLS slots in guest memory (within first 512MB)
    static GuestAddr next_tls = 0x00800000;  // 8MB mark
    tls_address_ = next_tls;
    next_tls += sizeof(XTls);
    
    // Zero the TLS area
    for (u32 i = 0; i < sizeof(XTls); i += 4) {
        memory_->write_u32(tls_address_ + i, 0);
    }
}

void XThread::create_guest_thread_struct() {
    // Allocate KTHREAD structure in guest memory
    // This is what the game sees when it calls KeGetCurrentThread()
    constexpr u32 KTHREAD_SIZE = 0x200;  // Size of KTHREAD structure
    
    // Use addresses in low memory (within first 512MB)
    static GuestAddr next_kthread = 0x00400000;  // 4MB mark
    guest_thread_ = next_kthread;
    next_kthread += KTHREAD_SIZE;
    
    // Zero the KTHREAD structure
    for (u32 i = 0; i < KTHREAD_SIZE; i += 4) {
        memory_->write_u32(guest_thread_ + i, 0);
    }
    
    // Initialize key KTHREAD fields
    // Offset 0x00: DISPATCHER_HEADER
    memory_->write_u8(guest_thread_ + 0, static_cast<u8>(XObjectType::Thread));
    memory_->write_u8(guest_thread_ + 2, KTHREAD_SIZE / 4);
    memory_->write_u32(guest_thread_ + 4, 0);  // SignalState
    
    // Offset 0x18: TEB pointer
    memory_->write_u32(guest_thread_ + 0x18, tls_address_);
    
    // Offset 0x1C: Stack base
    memory_->write_u32(guest_thread_ + 0x1C, stack_base_);
    
    // Offset 0x20: Stack limit
    memory_->write_u32(guest_thread_ + 0x20, stack_limit_);
    
    // Offset 0x8C: Thread ID
    memory_->write_u32(guest_thread_ + 0x8C, thread_id_);
    
    // Offset 0x90: Processor number
    memory_->write_u8(guest_thread_ + 0x90, cpu_thread_id_);
    
    // Offset 0x9C: Priority
    memory_->write_u8(guest_thread_ + 0x9C, static_cast<u8>(priority_));
}

void XThread::start() {
    if (state_ == XThreadState::Terminated) {
        return;
    }
    
    state_ = XThreadState::Ready;
    auto self = std::static_pointer_cast<XThread>(shared_from_this());
    XScheduler::instance().add_thread(self);
    
    LOGI("XThread %u started", thread_id_);
}

void XThread::suspend() {
    state_ = XThreadState::Suspended;
}

void XThread::resume() {
    if (state_ == XThreadState::Suspended) {
        state_ = XThreadState::Ready;
    }
}

void XThread::terminate(u32 exit_code) {
    exit(exit_code);
}

void XThread::exit(u32 exit_code) {
    exit_code_ = exit_code;
    state_ = XThreadState::Terminated;
    
    // Signal anyone waiting on thread termination
    wake_waiters();
    
    LOGI("XThread %u exited with code %u", thread_id_, exit_code);
}

bool XThread::is_signaled() const {
    // Thread is signaled when terminated
    return state_ == XThreadState::Terminated;
}

u32 XThread::wait(XObject* object, u64 timeout_100ns) {
    if (!object) return WAIT_FAILED;
    
    // Check if already signaled
    if (object->is_signaled()) {
        return WAIT_OBJECT_0;
    }
    
    // Zero timeout = poll
    if (timeout_100ns == 0) {
        return WAIT_TIMEOUT;
    }
    
    // Set up wait
    state_ = XThreadState::Waiting;
    wait_satisfied_ = false;
    wait_result_ = WAIT_TIMEOUT;
    
    // Add to object's wait list
    object->add_waiter(this);
    
    // Wait
    {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        
        if (timeout_100ns == UINT64_MAX) {
            // Infinite wait
            wait_cv_.wait(lock, [this]() {
                return wait_satisfied_.load() || should_stop_.load();
            });
        } else {
            // Timed wait
            auto timeout = std::chrono::nanoseconds(timeout_100ns * 100);
            wait_cv_.wait_for(lock, timeout, [this]() {
                return wait_satisfied_.load() || should_stop_.load();
            });
        }
    }
    
    // Clean up
    object->remove_waiter(this);
    state_ = XThreadState::Ready;
    
    return wait_result_.load();
}

u32 XThread::wait_multiple(XObject** objects, u32 count, bool wait_all, u64 timeout_100ns) {
    // Simplified implementation - just check each object
    for (u32 i = 0; i < count; i++) {
        if (!objects[i]) continue;
        
        if (objects[i]->is_signaled()) {
            if (!wait_all) {
                return WAIT_OBJECT_0 + i;
            }
        } else if (wait_all) {
            // Wait for this one
            return wait(objects[i], timeout_100ns);
        }
    }
    
    if (wait_all) {
        return WAIT_OBJECT_0;
    }
    
    return WAIT_TIMEOUT;
}

void XThread::wake_from_wait(u32 result) {
    wait_result_ = result;
    wait_satisfied_ = true;
    wait_cv_.notify_all();
}

void XThread::delay(u64 interval_100ns, bool alertable) {
    (void)alertable;  // TODO: Handle alertable waits
    
    state_ = XThreadState::Waiting;
    
    auto duration = std::chrono::nanoseconds(interval_100ns * 100);
    std::this_thread::sleep_for(duration);
    
    state_ = XThreadState::Ready;
}

void XThread::set_priority(XThreadPriority priority) {
    priority_ = priority;
    
    // Update guest structure
    if (guest_thread_) {
        memory_->write_u8(guest_thread_ + 0x9C, static_cast<u8>(priority));
    }
}

void XThread::set_affinity(u32 mask) {
    affinity_mask_ = mask & AllCores;
    if (affinity_mask_ == 0) {
        affinity_mask_ = AllCores;
    }
}

void XThread::queue_apc(GuestAddr routine, GuestAddr context) {
    std::lock_guard<std::mutex> lock(apc_mutex_);
    apc_queue_.push_back({routine, context});
}

void XThread::deliver_apcs() {
    std::vector<Apc> apcs;
    {
        std::lock_guard<std::mutex> lock(apc_mutex_);
        apcs.swap(apc_queue_);
    }
    
    // TODO: Actually execute APCs
    for (const auto& apc : apcs) {
        LOGD("Delivering APC: routine=0x%08X, context=0x%08X",
             apc.routine, apc.context);
    }
}

//=============================================================================
// XScheduler
//=============================================================================

XScheduler& XScheduler::instance() {
    static XScheduler instance;
    return instance;
}

void XScheduler::initialize(Cpu* cpu, Memory* memory) {
    cpu_ = cpu;
    memory_ = memory;
    current_time_ = 0;
    
    LOGI("XScheduler initialized");
}

void XScheduler::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.clear();
    current_thread_ = nullptr;
    
    LOGI("XScheduler shutdown complete");
}

void XScheduler::add_thread(std::shared_ptr<XThread> thread) {
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.push_back(thread);
    
    LOGD("Added thread %u to scheduler", thread->thread_id());
}

void XScheduler::remove_thread(XThread* thread) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    threads_.erase(
        std::remove_if(threads_.begin(), threads_.end(),
            [thread](const std::shared_ptr<XThread>& t) {
                return t.get() == thread;
            }),
        threads_.end()
    );
}

std::shared_ptr<XThread> XScheduler::get_thread(u32 thread_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& thread : threads_) {
        if (thread->thread_id() == thread_id) {
            return thread;
        }
    }
    return nullptr;
}

void XScheduler::run_for(u64 cycles) {
    // Execute all ready threads
    std::vector<std::shared_ptr<XThread>> ready;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& thread : threads_) {
            if (thread->state() == XThreadState::Ready ||
                thread->state() == XThreadState::Running) {
                ready.push_back(thread);
            }
        }
    }
    
    if (ready.empty()) {
        // No threads to run, just advance time
        advance_time(cycles);
        return;
    }
    
    // Divide cycles among threads
    u64 per_thread = cycles / ready.size();
    
    for (const auto& thread : ready) {
        current_thread_ = thread;
        KernelState::instance().set_current_thread(thread.get());
        
        thread->state_ = XThreadState::Running;
        
        // Execute on CPU
        cpu_->execute_thread(thread->cpu_thread_id(), per_thread);
        
        // Deliver any pending APCs
        thread->deliver_apcs();
        
        if (thread->state() == XThreadState::Running) {
            thread->state_ = XThreadState::Ready;
        }
    }
    
    current_thread_ = nullptr;
    KernelState::instance().set_current_thread(nullptr);
    
    advance_time(cycles);
}

void XScheduler::yield() {
    // Current thread yields, pick next
    schedule();
}

void XScheduler::schedule() {
    // Simple round-robin for now
    // TODO: Priority-based scheduling
}

u32 XScheduler::wait_for_object(XThread* thread, XObject* object, u64 timeout_100ns) {
    if (!thread || !object) return WAIT_FAILED;
    return thread->wait(object, timeout_100ns);
}

void XScheduler::signal_object(XObject* object) {
    if (object) {
        object->signal();
        object->wake_waiters();
    }
}

void XScheduler::advance_time(u64 cycles) {
    // Convert cycles to 100ns units (assuming ~3.2 GHz)
    current_time_ += cycles / 32;
}

void XScheduler::process_timers() {
    // TODO: Process timer queue
}

void XScheduler::process_dpcs() {
    KernelState::instance().process_dpcs();
}

} // namespace x360mu
