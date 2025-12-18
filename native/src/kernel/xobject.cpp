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

u32 ObjectTable::add_object(std::shared_ptr<XObject> object) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    u32 handle = next_handle_++;
    object->set_handle(handle);
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
    
    LOGD("Removed object: handle=0x%08X", handle);
    objects_.erase(it);
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
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    return objects_.size();
}

void ObjectTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    objects_.clear();
    next_handle_ = 0x10000;
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

} // namespace x360mu
