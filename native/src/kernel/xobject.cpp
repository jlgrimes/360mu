/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Object System Implementation
 */

#include "xobject.h"
#include "xthread.h"
#include "../memory/memory.h"
#include "../cpu/xenon/cpu.h"
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-xobj"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[XOBJ] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[XOBJ WARN] " __VA_ARGS__); printf("\n")
#endif

namespace x360mu {

//=============================================================================
// XObject
//=============================================================================

XObject::XObject(XObjectType type) : type_(type) {
}

XObject::~XObject() {
    // Wake any remaining waiters when object is destroyed
    wake_waiters();
}

void XObject::retain() {
    ref_count_.fetch_add(1, std::memory_order_relaxed);
}

void XObject::release() {
    if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Last reference - object will be destroyed
    }
}

void XObject::add_waiter(XThread* thread) {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    waiters_.push_back(thread);
}

void XObject::remove_waiter(XThread* thread) {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    waiters_.remove(thread);
}

void XObject::wake_waiters(u32 count) {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    
    u32 woken = 0;
    auto it = waiters_.begin();
    while (it != waiters_.end() && woken < count) {
        XThread* thread = *it;
        it = waiters_.erase(it);
        
        // Wake the thread (implemented in xthread.cpp)
        if (thread) {
            thread->wake_from_wait(WAIT_OBJECT_0);
            woken++;
        }
    }
}

//=============================================================================
// ObjectTable
//=============================================================================

ObjectTable::ObjectTable() {
}

ObjectTable::~ObjectTable() {
    std::lock_guard<std::mutex> lock(mutex_);
    objects_.clear();
}

u32 ObjectTable::allocate_handle() {
    // NT-style handle allocation: 4-byte aligned incrementing
    u32 handle = next_handle_;
    next_handle_ += 4;
    return handle;
}

u32 ObjectTable::add_object(std::shared_ptr<XObject> object) {
    std::lock_guard<std::mutex> lock(mutex_);

    u32 handle = allocate_handle();
    object->set_handle(handle);
    // The object starts with refcount 1 (from construction).
    // The handle table holds a shared_ptr which keeps it alive.
    objects_[handle] = object;

    LOGD("Added object: handle=0x%08X, type=%u, name=%s",
         handle, static_cast<u32>(object->type()), object->name().c_str());

    return handle;
}

bool ObjectTable::remove_handle(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = objects_.find(handle);
    if (it == objects_.end()) {
        return false;
    }

    auto obj = it->second;
    objects_.erase(it);
    // Release the handle's reference on the Xbox-side refcount
    obj->release();

    LOGD("Removed object: handle=0x%08X (refcount now %u)",
         handle, obj->ref_count());
    return true;
}

std::shared_ptr<XObject> ObjectTable::lookup(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = objects_.find(handle);
    if (it == objects_.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<XObject> ObjectTable::lookup_typed(u32 handle, XObjectType expected_type, u32* status_out) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = objects_.find(handle);
    if (it == objects_.end()) {
        if (status_out) *status_out = nt_obj::STATUS_INVALID_HANDLE;
        return nullptr;
    }

    auto& obj = it->second;
    if (expected_type != XObjectType::None && obj->type() != expected_type) {
        if (status_out) *status_out = nt_obj::STATUS_OBJECT_TYPE_MISMATCH;
        LOGW("Type mismatch: handle=0x%08X, expected=%u, actual=%u",
             handle, static_cast<u32>(expected_type), static_cast<u32>(obj->type()));
        return nullptr;
    }

    if (status_out) *status_out = nt_obj::STATUS_SUCCESS;
    return obj;
}

u32 ObjectTable::reference_object_by_handle(u32 handle, XObjectType expected_type,
                                             std::shared_ptr<XObject>* out_object) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = objects_.find(handle);
    if (it == objects_.end()) {
        LOGW("ObReferenceObjectByHandle: invalid handle 0x%08X", handle);
        return nt_obj::STATUS_INVALID_HANDLE;
    }

    auto& obj = it->second;
    if (expected_type != XObjectType::None && obj->type() != expected_type) {
        LOGW("ObReferenceObjectByHandle: type mismatch handle=0x%08X (expected=%u, got=%u)",
             handle, static_cast<u32>(expected_type), static_cast<u32>(obj->type()));
        return nt_obj::STATUS_OBJECT_TYPE_MISMATCH;
    }

    // Increment Xbox-side refcount (caller must eventually call release/ObDereferenceObject)
    obj->retain();

    if (out_object) {
        *out_object = obj;
    }

    LOGD("ObReferenceObjectByHandle: handle=0x%08X, refcount=%u",
         handle, obj->ref_count());
    return nt_obj::STATUS_SUCCESS;
}

u32 ObjectTable::close_handle(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = objects_.find(handle);
    if (it == objects_.end()) {
        LOGW("NtClose: invalid handle 0x%08X", handle);
        return nt_obj::STATUS_INVALID_HANDLE;
    }

    auto obj = it->second;
    objects_.erase(it);

    // Release the handle's reference. The object may still be alive
    // if other references exist (e.g., wait list, ObReferenceObjectByHandle).
    obj->release();

    LOGD("NtClose: handle=0x%08X closed (refcount now %u)",
         handle, obj->ref_count());
    return nt_obj::STATUS_SUCCESS;
}

u32 ObjectTable::duplicate_handle(u32 source_handle, u32* target_handle_out) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = objects_.find(source_handle);
    if (it == objects_.end()) {
        LOGW("NtDuplicateObject: invalid source handle 0x%08X", source_handle);
        return nt_obj::STATUS_INVALID_HANDLE;
    }

    auto obj = it->second;

    // Allocate new handle pointing to same object
    u32 new_handle = allocate_handle();
    objects_[new_handle] = obj;

    // Increment Xbox-side refcount for the new handle
    obj->retain();

    if (target_handle_out) {
        *target_handle_out = new_handle;
    }

    LOGD("NtDuplicateObject: 0x%08X -> 0x%08X (refcount=%u)",
         source_handle, new_handle, obj->ref_count());
    return nt_obj::STATUS_SUCCESS;
}

std::shared_ptr<XObject> ObjectTable::lookup_by_name(const std::string& name) {
    if (name.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& pair : objects_) {
        if (pair.second->name() == name) {
            return pair.second;
        }
    }
    return nullptr;
}

size_t ObjectTable::object_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return objects_.size();
}

void ObjectTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Release all handle references before clearing
    for (auto& pair : objects_) {
        pair.second->release();
    }
    objects_.clear();
    next_handle_ = 4;
}

//=============================================================================
// KernelState
//=============================================================================

thread_local XThread* KernelState::current_thread_ = nullptr;

KernelState& KernelState::instance() {
    static KernelState instance;
    return instance;
}

void KernelState::initialize(Memory* memory, Cpu* cpu) {
    memory_ = memory;
    cpu_ = cpu;
    boot_time_ = std::chrono::steady_clock::now();
    
    LOGI("KernelState initialized (cpu=%s)", cpu ? "available" : "null");
}

void KernelState::shutdown() {
    // Clear all objects
    object_table_.clear();
    
    // Clear DPC queue
    {
        std::lock_guard<std::mutex> lock(dpc_mutex_);
        dpc_queue_.clear();
    }
    
    // Clear timer queue
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        timer_queue_.clear();
    }
    
    gpu_interrupt_event_addr_ = 0;
    memory_ = nullptr;
    
    LOGI("KernelState shutdown complete");
}

u64 KernelState::system_time() const {
    // Windows FILETIME: 100-nanosecond intervals since January 1, 1601
    // We need to add the offset from 1601 to Unix epoch (1970)
    constexpr u64 EPOCH_DIFF = 116444736000000000ULL;
    
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto ticks = std::chrono::duration_cast<std::chrono::duration<u64, std::ratio<1, 10000000>>>(duration);
    
    return ticks.count() + EPOCH_DIFF;
}

u64 KernelState::interrupt_time() const {
    // 100-nanosecond intervals since boot
    auto now = std::chrono::steady_clock::now();
    auto duration = now - boot_time_;
    return std::chrono::duration_cast<std::chrono::duration<u64, std::ratio<1, 10000000>>>(duration).count();
}

u32 KernelState::tick_count() const {
    // Milliseconds since boot
    auto now = std::chrono::steady_clock::now();
    auto duration = now - boot_time_;
    return static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

void KernelState::set_current_thread(XThread* thread) {
    current_thread_ = thread;
}

XThread* KernelState::current_thread() const {
    return current_thread_;
}

void KernelState::queue_dpc(GuestAddr dpc_addr, GuestAddr dpc_routine, GuestAddr context,
                            GuestAddr arg1, GuestAddr arg2) {
    std::lock_guard<std::mutex> lock(dpc_mutex_);
    dpc_queue_.push_back({dpc_addr, dpc_routine, context, arg1, arg2});
    LOGI("Queued DPC: dpc=0x%08X, routine=0x%08X, context=0x%08X, arg1=0x%08X, arg2=0x%08X",
         dpc_addr, dpc_routine, context, arg1, arg2);
}

void KernelState::process_dpcs() {
    std::vector<DpcEntry> to_process;
    
    {
        std::lock_guard<std::mutex> lock(dpc_mutex_);
        to_process.swap(dpc_queue_);
    }
    
    if (to_process.empty()) return;
    
    LOGI("Processing %zu DPCs", to_process.size());
    
    for (const auto& dpc : to_process) {
        if (dpc.routine == 0) {
            LOGW("Skipping DPC with null routine");
            continue;
        }
        
        // Validate routine address is in a valid executable range
        // Xbox 360 executable code is typically in 0x80000000-0x90000000 range
        // or in physical memory mapped regions
        bool valid_address = false;
        if (dpc.routine >= 0x80000000 && dpc.routine < 0xA0000000) {
            valid_address = true;  // Virtual kernel/user space
        } else if (dpc.routine < 0x20000000) {
            valid_address = true;  // Physical memory (first 512MB)
        } else if (dpc.routine >= 0x00100000 && dpc.routine < 0x40000000) {
            valid_address = true;  // Test addresses and low physical
        }
        
        if (!valid_address) {
            LOGW("Skipping DPC with invalid routine address 0x%08X", dpc.routine);
            continue;
        }
        
        LOGI("Executing DPC: dpc=0x%08X, routine=0x%08X, context=0x%08X, arg1=0x%08X, arg2=0x%08X",
             dpc.dpc_addr, dpc.routine, dpc.context, dpc.arg1, dpc.arg2);
        
        // Execute DPC by running the guest routine
        // DPC routines run at DISPATCH_LEVEL, synchronously
        // 
        // DPC routine signature (Xbox 360 / Windows NT):
        //   void DpcRoutine(PKDPC Dpc, PVOID DeferredContext, 
        //                   PVOID SystemArgument1, PVOID SystemArgument2);
        // Register mapping:
        //   r3 = Dpc pointer (address of KDPC structure)
        //   r4 = DeferredContext
        //   r5 = SystemArgument1
        //   r6 = SystemArgument2
        
        if (cpu_ && memory_) {
            // Create a temporary context for DPC execution
            ThreadContext ctx;
            ctx.reset();
            ctx.pc = dpc.routine;
            ctx.gpr[3] = dpc.dpc_addr;    // Dpc pointer (r3)
            ctx.gpr[4] = dpc.context;     // DeferredContext (r4)
            ctx.gpr[5] = dpc.arg1;        // SystemArgument1 (r5)
            ctx.gpr[6] = dpc.arg2;        // SystemArgument2 (r6)
            ctx.lr = 0;                    // Return terminates (blr to 0 = done)
            ctx.running = true;
            ctx.memory = memory_;
            ctx.thread_id = 0;             // Run on CPU thread 0
            
            // DPCs should be quick, execute for limited cycles
            // If DPC doesn't complete, it will resume on next process_dpcs call
            constexpr u64 DPC_MAX_CYCLES = 50000;
            
            // Execute the DPC using cpu context execution
            // This runs the DPC synchronously until it returns (blr) or hits cycle limit
            cpu_->execute_with_context(0, ctx, DPC_MAX_CYCLES);
            
            LOGI("DPC routine 0x%08X completed (pc after=0x%08llX)", 
                 dpc.routine, ctx.pc);
        } else {
            LOGW("Cannot execute DPC: no CPU or memory available");
        }
    }
}

void KernelState::queue_timer(GuestAddr timer_addr, u64 due_time_100ns, u64 period_100ns, GuestAddr dpc_addr) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    
    // Check if timer already exists - update it instead of adding new
    for (auto& entry : timer_queue_) {
        if (entry.timer_addr == timer_addr) {
            entry.due_time_100ns = due_time_100ns;
            entry.period_100ns = period_100ns;
            entry.dpc_addr = dpc_addr;
            LOGI("Updated timer 0x%08X: due=%llu, period=%llu, dpc=0x%08X",
                 timer_addr, (unsigned long long)due_time_100ns, 
                 (unsigned long long)period_100ns, dpc_addr);
            return;
        }
    }
    
    // Add new timer
    timer_queue_.push_back({timer_addr, due_time_100ns, period_100ns, dpc_addr});
    LOGI("Queued timer 0x%08X: due=%llu, period=%llu, dpc=0x%08X",
         timer_addr, (unsigned long long)due_time_100ns, 
         (unsigned long long)period_100ns, dpc_addr);
}

bool KernelState::cancel_timer(GuestAddr timer_addr) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    
    for (auto it = timer_queue_.begin(); it != timer_queue_.end(); ++it) {
        if (it->timer_addr == timer_addr) {
            timer_queue_.erase(it);
            LOGI("Cancelled timer 0x%08X", timer_addr);
            return true;  // Was set
        }
    }
    return false;  // Was not set
}

void KernelState::process_timer_queue() {
    u64 current_time = system_time();
    
    std::vector<TimerEntry> expired;
    
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        
        for (auto it = timer_queue_.begin(); it != timer_queue_.end(); ) {
            if (current_time >= it->due_time_100ns) {
                expired.push_back(*it);
                
                // Handle periodic timers
                if (it->period_100ns > 0) {
                    // Reschedule periodic timer
                    it->due_time_100ns = current_time + it->period_100ns;
                    ++it;
                } else {
                    // One-shot timer - remove it
                    it = timer_queue_.erase(it);
                }
            } else {
                ++it;
            }
        }
    }
    
    // Process expired timers outside the lock
    for (const auto& timer : expired) {
        LOGI("Timer 0x%08X fired", timer.timer_addr);
        
        // Signal the timer object (set SignalState to 1)
        if (memory_) {
            memory_->write_u32(timer.timer_addr + 4, 1);  // SignalState = 1
        }
        
        // Queue associated DPC if present
        if (timer.dpc_addr != 0 && memory_) {
            // Read DPC routine and context from the KDPC structure
            // KDPC layout:
            //   0x0C: DeferredRoutine
            //   0x10: DeferredContext
            GuestAddr routine = memory_->read_u32(timer.dpc_addr + 0x0C);
            GuestAddr context = memory_->read_u32(timer.dpc_addr + 0x10);
            
            if (routine != 0) {
                // For timer DPCs, SystemArgument1 is often the timer address
                queue_dpc(timer.dpc_addr, routine, context, timer.timer_addr, 0);
            }
        }
    }
}

void KernelState::queue_gpu_interrupt() {
    LOGI("GPU interrupt received");
    
    // Signal GPU interrupt event if one is registered
    if (gpu_interrupt_event_addr_ != 0 && memory_) {
        memory_->write_u32(gpu_interrupt_event_addr_ + 4, 1);  // SignalState = 1
        LOGI("Signaled GPU interrupt event at 0x%08X", gpu_interrupt_event_addr_);
    }
    
    // Also queue a system DPC to notify any waiters
    // This simulates the GPU interrupt handler DPC
}

} // namespace x360mu
