/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Object System Implementation
 */

#include "xobject.h"
#include "xthread.h"
#include "../memory/memory.h"
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

void KernelState::initialize(Memory* memory) {
    memory_ = memory;
    boot_time_ = std::chrono::steady_clock::now();
    
    LOGI("KernelState initialized");
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

void KernelState::queue_dpc(GuestAddr dpc_routine, GuestAddr context) {
    std::lock_guard<std::mutex> lock(dpc_mutex_);
    dpc_queue_.push_back({dpc_routine, context});
    LOGD("Queued DPC: routine=0x%08X, context=0x%08X", dpc_routine, context);
}

void KernelState::process_dpcs() {
    std::vector<DpcEntry> to_process;
    
    {
        std::lock_guard<std::mutex> lock(dpc_mutex_);
        to_process.swap(dpc_queue_);
    }
    
    for (const auto& dpc : to_process) {
        // In a full implementation, we'd call the DPC routine
        // For now, we just log it
        LOGD("Processing DPC: routine=0x%08X, context=0x%08X", 
             dpc.routine, dpc.context);
        
        // TODO: Actually execute the DPC routine
        // This requires calling back into the CPU emulator
    }
}

} // namespace x360mu
