/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Threading & Synchronization Implementation
 */

#include "threading.h"
#include "kernel.h"
#include "../memory/memory.h"
#include "../cpu/xenon/cpu.h"
#include "../cpu/xenon/threading.h"
#include <algorithm>
#include <chrono>
#include <thread>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-kthread"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[KTHREAD] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[KTHREAD WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[KTHREAD ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

// Global thread manager instance
static KernelThreadManager* g_thread_manager = nullptr;

KernelThreadManager* get_kernel_thread_manager() {
    return g_thread_manager;
}

void set_kernel_thread_manager(KernelThreadManager* manager) {
    g_thread_manager = manager;
}

//=============================================================================
// KernelMutant Implementation
//=============================================================================

void KernelMutant::on_wait_satisfied(GuestThread* thread) {
    owner = thread;
    if (thread) {
        owner_thread_id = thread->thread_id;
    }
    recursion_count = 1;
    abandoned = false;
}

//=============================================================================
// KernelThreadManager Implementation
//=============================================================================

KernelThreadManager::KernelThreadManager() {
    tls_slots_used_.fill(false);
}

KernelThreadManager::~KernelThreadManager() {
    shutdown();
}

Status KernelThreadManager::initialize(Memory* memory, Cpu* cpu, ThreadScheduler* scheduler) {
    memory_ = memory;
    cpu_ = cpu;
    scheduler_ = scheduler;
    
    tls_slots_used_.fill(false);
    stats_ = {};
    
    LOGI("KernelThreadManager initialized");
    return Status::Ok;
}

void KernelThreadManager::shutdown() {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    objects_.clear();
    thread_handles_.clear();
    tls_slots_used_.fill(false);
}

void KernelThreadManager::reset() {
    shutdown();
    next_handle_ = 0x80001000;
    stats_ = {};
}

//=============================================================================
// Thread Management
//=============================================================================

u32 KernelThreadManager::create_thread(u32* handle_out, u32 stack_size, u32* thread_id_out,
                                        GuestAddr xapi_startup, GuestAddr start_address,
                                        GuestAddr start_param, u32 creation_flags) {
    if (!scheduler_) {
        return nt::STATUS_UNSUCCESSFUL;
    }
    
    // Determine entry point
    GuestAddr entry = xapi_startup ? xapi_startup : start_address;
    
    // Create thread through scheduler
    GuestThread* thread = scheduler_->create_thread(entry, start_param, stack_size, creation_flags);
    if (!thread) {
        LOGE("Failed to create thread");
        return nt::STATUS_NO_MEMORY;
    }
    
    // If using XAPI startup wrapper, set start_address as second argument
    if (xapi_startup) {
        thread->context.gpr[4] = start_address;
    }
    
    // Track the handle
    {
        std::lock_guard<std::mutex> lock(thread_handles_mutex_);
        thread_handles_[thread->handle] = thread->thread_id;
    }
    
    // Output results
    if (handle_out) *handle_out = thread->handle;
    if (thread_id_out) *thread_id_out = thread->thread_id;
    
    stats_.threads_created++;
    
    LOGI("Created thread: handle=0x%X, id=%u, entry=0x%08X, stack_size=0x%X",
         thread->handle, thread->thread_id, entry, stack_size);
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::terminate_thread(u32 handle, u32 exit_code) {
    if (!scheduler_) return nt::STATUS_UNSUCCESSFUL;
    
    GuestThread* thread = scheduler_->get_thread_by_handle(handle);
    if (!thread) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    scheduler_->terminate_thread(thread, exit_code);
    
    // Release any mutants owned by this thread
    {
        std::lock_guard<std::mutex> lock(objects_mutex_);
        for (auto& [h, obj] : objects_) {
            if (obj->type == KernelWaitableType::Mutant) {
                auto* mutant = static_cast<KernelMutant*>(obj.get());
                if (mutant->owner == thread) {
                    mutant->owner = nullptr;
                    mutant->owner_thread_id = 0;
                    mutant->abandoned = true;
                    wake_waiters(mutant);
                }
            }
        }
    }
    
    // Remove from handle tracking
    {
        std::lock_guard<std::mutex> lock(thread_handles_mutex_);
        thread_handles_.erase(handle);
    }
    
    LOGI("Terminated thread: handle=0x%X, exit_code=%u", handle, exit_code);
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::suspend_thread(u32 handle, u32* prev_count) {
    if (!scheduler_) return nt::STATUS_UNSUCCESSFUL;
    
    GuestThread* thread = scheduler_->get_thread_by_handle(handle);
    if (!thread) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    u32 count = scheduler_->suspend_thread(thread);
    if (prev_count) *prev_count = count;
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::resume_thread(u32 handle, u32* prev_count) {
    if (!scheduler_) return nt::STATUS_UNSUCCESSFUL;
    
    GuestThread* thread = scheduler_->get_thread_by_handle(handle);
    if (!thread) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    u32 count = scheduler_->resume_thread(thread);
    if (prev_count) *prev_count = count;
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::get_current_thread_handle() {
    if (!scheduler_) return 0;
    
    GuestThread* thread = scheduler_->get_current_thread(0);  // Simplified: use hw thread 0
    return thread ? thread->handle : 0;
}

u32 KernelThreadManager::get_current_thread_id() {
    if (!scheduler_) return 0;
    
    GuestThread* thread = scheduler_->get_current_thread(0);
    return thread ? thread->thread_id : 0;
}

u32 KernelThreadManager::get_current_processor() {
    if (!scheduler_) return 0;
    
    GuestThread* thread = scheduler_->get_current_thread(0);
    if (thread) {
        // Return which hardware thread this is assigned to
        return thread->context.thread_id % 6;
    }
    return 0;
}

u32 KernelThreadManager::set_thread_affinity(u32 handle, u32 affinity_mask, u32* prev_affinity) {
    if (!scheduler_) return nt::STATUS_UNSUCCESSFUL;
    
    GuestThread* thread = scheduler_->get_thread_by_handle(handle);
    if (!thread) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    if (prev_affinity) *prev_affinity = thread->affinity_mask;
    scheduler_->set_affinity(thread, affinity_mask);
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::set_thread_priority(u32 handle, s32 priority) {
    if (!scheduler_) return nt::STATUS_UNSUCCESSFUL;
    
    GuestThread* thread = scheduler_->get_thread_by_handle(handle);
    if (!thread) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    scheduler_->set_priority(thread, static_cast<ThreadPriority>(priority));
    return nt::STATUS_SUCCESS;
}

//=============================================================================
// Event Management
//=============================================================================

u32 KernelThreadManager::create_event(u32* handle_out, u32 access_mask, GuestAddr obj_attr,
                                       EventType event_type, bool initial_state) {
    auto event = std::make_unique<KernelEvent>();
    event->event_type = event_type;
    event->signaled = initial_state;
    event->handle = allocate_handle();
    
    u32 handle = event->handle;
    
    {
        std::lock_guard<std::mutex> lock(objects_mutex_);
        objects_[handle] = std::move(event);
    }
    
    if (handle_out) *handle_out = handle;
    stats_.events_created++;
    
    LOGD("Created event: handle=0x%X, type=%u, initial=%d",
         handle, static_cast<u32>(event_type), initial_state);
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::set_event(u32 handle, s32* prev_state) {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    
    auto it = objects_.find(handle);
    if (it == objects_.end() || it->second->type != KernelWaitableType::Event) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    auto* event = static_cast<KernelEvent*>(it->second.get());
    if (prev_state) *prev_state = event->signaled ? 1 : 0;
    
    event->signaled = true;
    wake_waiters(event);
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::clear_event(u32 handle) {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    
    auto it = objects_.find(handle);
    if (it == objects_.end() || it->second->type != KernelWaitableType::Event) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    static_cast<KernelEvent*>(it->second.get())->signaled = false;
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::pulse_event(u32 handle, s32* prev_state) {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    
    auto it = objects_.find(handle);
    if (it == objects_.end() || it->second->type != KernelWaitableType::Event) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    auto* event = static_cast<KernelEvent*>(it->second.get());
    if (prev_state) *prev_state = event->signaled ? 1 : 0;
    
    // Set then immediately reset
    event->signaled = true;
    wake_waiters(event);
    event->signaled = false;
    
    return nt::STATUS_SUCCESS;
}

//=============================================================================
// Semaphore Management
//=============================================================================

u32 KernelThreadManager::create_semaphore(u32* handle_out, u32 access_mask, GuestAddr obj_attr,
                                           s32 initial_count, s32 max_count) {
    if (initial_count < 0 || max_count <= 0 || initial_count > max_count) {
        return nt::STATUS_INVALID_PARAMETER;
    }
    
    auto sem = std::make_unique<KernelSemaphore>();
    sem->count = initial_count;
    sem->max_count = max_count;
    sem->handle = allocate_handle();
    
    u32 handle = sem->handle;
    
    {
        std::lock_guard<std::mutex> lock(objects_mutex_);
        objects_[handle] = std::move(sem);
    }
    
    if (handle_out) *handle_out = handle;
    stats_.semaphores_created++;
    
    LOGD("Created semaphore: handle=0x%X, count=%d, max=%d", handle, initial_count, max_count);
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::release_semaphore(u32 handle, s32 release_count, s32* prev_count) {
    if (release_count <= 0) {
        return nt::STATUS_INVALID_PARAMETER;
    }
    
    std::lock_guard<std::mutex> lock(objects_mutex_);
    
    auto it = objects_.find(handle);
    if (it == objects_.end() || it->second->type != KernelWaitableType::Semaphore) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    auto* sem = static_cast<KernelSemaphore*>(it->second.get());
    
    if (prev_count) *prev_count = sem->count;
    
    // Check for overflow
    if (sem->count + release_count > sem->max_count) {
        return nt::STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
    
    sem->count += release_count;
    wake_waiters(sem);
    
    return nt::STATUS_SUCCESS;
}

//=============================================================================
// Mutant (Mutex) Management
//=============================================================================

u32 KernelThreadManager::create_mutant(u32* handle_out, u32 access_mask, GuestAddr obj_attr,
                                        bool initial_owner) {
    auto mutant = std::make_unique<KernelMutant>();
    mutant->handle = allocate_handle();
    
    if (initial_owner && scheduler_) {
        GuestThread* current = scheduler_->get_current_thread(0);
        if (current) {
            mutant->owner = current;
            mutant->owner_thread_id = current->thread_id;
            mutant->recursion_count = 1;
        }
    }
    
    u32 handle = mutant->handle;
    
    {
        std::lock_guard<std::mutex> lock(objects_mutex_);
        objects_[handle] = std::move(mutant);
    }
    
    if (handle_out) *handle_out = handle;
    stats_.mutants_created++;
    
    LOGD("Created mutant: handle=0x%X, initial_owner=%d", handle, initial_owner);
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::release_mutant(u32 handle, bool abandoned, s32* prev_count) {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    
    auto it = objects_.find(handle);
    if (it == objects_.end() || it->second->type != KernelWaitableType::Mutant) {
        return nt::STATUS_INVALID_HANDLE;
    }
    
    auto* mutant = static_cast<KernelMutant*>(it->second.get());
    
    // Verify ownership
    GuestThread* current = scheduler_ ? scheduler_->get_current_thread(0) : nullptr;
    if (mutant->owner != current && !abandoned) {
        return nt::STATUS_MUTANT_NOT_OWNED;
    }
    
    if (prev_count) *prev_count = mutant->recursion_count;
    
    if (mutant->recursion_count > 1 && !abandoned) {
        mutant->recursion_count--;
    } else {
        mutant->owner = nullptr;
        mutant->owner_thread_id = 0;
        mutant->recursion_count = 0;
        mutant->abandoned = abandoned;
        wake_waiters(mutant);
    }
    
    return nt::STATUS_SUCCESS;
}

//=============================================================================
// Wait Functions
//=============================================================================

u32 KernelThreadManager::wait_for_single_object(u32 handle, bool alertable, s64* timeout_100ns) {
    return wait_for_multiple_objects(1, &handle, WaitType::WaitAny, alertable, timeout_100ns);
}

u32 KernelThreadManager::wait_for_multiple_objects(u32 count, const u32* handles,
                                                    WaitType wait_type, bool alertable,
                                                    s64* timeout_100ns) {
    if (count == 0 || count > 64 || !handles) {
        return nt::STATUS_INVALID_PARAMETER;
    }
    
    stats_.total_waits++;
    
    // Collect waitable objects
    std::vector<KernelWaitable*> objects;
    objects.reserve(count);
    
    {
        std::lock_guard<std::mutex> lock(objects_mutex_);
        for (u32 i = 0; i < count; i++) {
            auto* obj = get_waitable(handles[i]);
            if (!obj) {
                return nt::STATUS_INVALID_HANDLE;
            }
            objects.push_back(obj);
        }
    }
    
    return perform_wait(objects, wait_type, alertable, timeout_100ns);
}

u32 KernelThreadManager::perform_wait(const std::vector<KernelWaitable*>& objects,
                                       WaitType wait_type, bool alertable, s64* timeout_100ns) {
    GuestThread* current = scheduler_ ? scheduler_->get_current_thread(0) : nullptr;
    
    // Check for pending APCs if alertable
    if (alertable && current && current->has_pending_apcs()) {
        // Process APCs and return STATUS_USER_APC
        current->in_alertable_wait = true;
        scheduler_->process_pending_apcs(current);
        return nt::STATUS_USER_APC;
    }
    
    // Check if already alerted
    if (alertable && current && current->alerted) {
        current->alerted = false;
        current->in_alertable_wait = true;
        scheduler_->process_pending_apcs(current);
        return nt::STATUS_ALERTED;
    }
    
    // Calculate deadline
    bool infinite_wait = (timeout_100ns == nullptr);
    u64 deadline = 0;
    
    if (!infinite_wait) {
        s64 timeout = *timeout_100ns;
        if (timeout < 0) {
            // Relative timeout (negative)
            deadline = get_current_time_100ns() + static_cast<u64>(-timeout);
        } else if (timeout == 0) {
            // No wait - just check
            if (check_wait_satisfied(objects, wait_type)) {
                // Satisfy the wait
                for (auto* obj : objects) {
                    if (obj->is_signaled()) {
                        obj->on_wait_satisfied(current);
                        if (wait_type == WaitType::WaitAny) break;
                    }
                }
                return nt::STATUS_WAIT_0;
            }
            stats_.wait_timeouts++;
            return nt::STATUS_TIMEOUT;
        } else {
            // Absolute timeout
            deadline = static_cast<u64>(timeout);
        }
    }
    
    // Mark thread as in alertable wait if requested
    if (alertable && current) {
        current->in_alertable_wait = true;
    }
    
    // Wait loop
    while (true) {
        // Check for pending APCs if alertable (APCs may have been queued during wait)
        if (alertable && current) {
            if (current->has_pending_apcs()) {
                scheduler_->process_pending_apcs(current);
                return nt::STATUS_USER_APC;
            }
            if (current->alerted) {
                current->alerted = false;
                scheduler_->process_pending_apcs(current);
                return nt::STATUS_ALERTED;
            }
        }
        
        // Check if wait is satisfied
        if (check_wait_satisfied(objects, wait_type)) {
            if (alertable && current) {
                current->in_alertable_wait = false;
            }
            
            u32 result = nt::STATUS_WAIT_0;
            for (size_t i = 0; i < objects.size(); i++) {
                if (objects[i]->is_signaled()) {
                    // Check for abandoned mutant
                    if (objects[i]->type == KernelWaitableType::Mutant) {
                        auto* mutant = static_cast<KernelMutant*>(objects[i]);
                        if (mutant->abandoned) {
                            result = nt::STATUS_ABANDONED_WAIT_0 + static_cast<u32>(i);
                        }
                    }
                    
                    objects[i]->on_wait_satisfied(current);
                    
                    if (wait_type == WaitType::WaitAny) {
                        return result + static_cast<u32>(i);
                    }
                }
            }
            return result;
        }
        
        // Check timeout
        if (!infinite_wait && get_current_time_100ns() >= deadline) {
            if (alertable && current) {
                current->in_alertable_wait = false;
            }
            stats_.wait_timeouts++;
            return nt::STATUS_TIMEOUT;
        }
        
        // Yield to allow other threads to run
        if (scheduler_ && current) {
            scheduler_->yield(current);
        }
        
        // Small sleep to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

bool KernelThreadManager::check_wait_satisfied(const std::vector<KernelWaitable*>& objects,
                                                WaitType wait_type) {
    if (wait_type == WaitType::WaitAll) {
        // All must be signaled
        for (auto* obj : objects) {
            if (!obj->is_signaled()) return false;
        }
        return true;
    } else {
        // Any one signaled is enough
        for (auto* obj : objects) {
            if (obj->is_signaled()) return true;
        }
        return false;
    }
}

//=============================================================================
// Critical Section Support
//=============================================================================

void KernelThreadManager::init_critical_section(GuestAddr cs_ptr) {
    init_critical_section_with_spin(cs_ptr, 0);
}

u32 KernelThreadManager::init_critical_section_with_spin(GuestAddr cs_ptr, u32 spin_count) {
    using CS = RTL_CRITICAL_SECTION_LAYOUT;
    
    memory_->write_u32(cs_ptr + CS::OFFSET_DEBUG_INFO, 0);
    memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, static_cast<u32>(-1));  // Unlocked
    memory_->write_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT, 0);
    memory_->write_u32(cs_ptr + CS::OFFSET_OWNING_THREAD, 0);
    memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_SEMAPHORE, 0);
    memory_->write_u32(cs_ptr + CS::OFFSET_SPIN_COUNT, spin_count);
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::enter_critical_section(GuestAddr cs_ptr) {
    using CS = RTL_CRITICAL_SECTION_LAYOUT;
    
    u32 current_tid = get_current_thread_id();
    if (current_tid == 0) current_tid = 1;  // Default to main thread
    
    // Try to acquire
    s32 lock_count = static_cast<s32>(memory_->read_u32(cs_ptr + CS::OFFSET_LOCK_COUNT));
    u32 owning_thread = memory_->read_u32(cs_ptr + CS::OFFSET_OWNING_THREAD);
    
    // Already owned by us? (recursive)
    if (owning_thread == current_tid) {
        s32 recursion = static_cast<s32>(memory_->read_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT));
        memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, static_cast<u32>(lock_count + 1));
        memory_->write_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT, static_cast<u32>(recursion + 1));
        return nt::STATUS_SUCCESS;
    }
    
    // Spin first (if spin count > 0)
    u32 spin_count = memory_->read_u32(cs_ptr + CS::OFFSET_SPIN_COUNT);
    for (u32 i = 0; i < spin_count; i++) {
        lock_count = static_cast<s32>(memory_->read_u32(cs_ptr + CS::OFFSET_LOCK_COUNT));
        if (lock_count == -1) {
            // Unlocked - try to acquire
            memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, 0);
            memory_->write_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT, 1);
            memory_->write_u32(cs_ptr + CS::OFFSET_OWNING_THREAD, current_tid);
            return nt::STATUS_SUCCESS;
        }
    }
    
    // Need to wait
    while (true) {
        lock_count = static_cast<s32>(memory_->read_u32(cs_ptr + CS::OFFSET_LOCK_COUNT));
        
        if (lock_count == -1) {
            // Unlocked - acquire
            memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, 0);
            memory_->write_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT, 1);
            memory_->write_u32(cs_ptr + CS::OFFSET_OWNING_THREAD, current_tid);
            return nt::STATUS_SUCCESS;
        }
        
        // Increment lock count to indicate we're waiting
        memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, static_cast<u32>(lock_count + 1));
        
        // Yield
        yield();
    }
}

u32 KernelThreadManager::leave_critical_section(GuestAddr cs_ptr) {
    using CS = RTL_CRITICAL_SECTION_LAYOUT;
    
    s32 recursion = static_cast<s32>(memory_->read_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT));
    s32 lock_count = static_cast<s32>(memory_->read_u32(cs_ptr + CS::OFFSET_LOCK_COUNT));
    
    if (recursion > 1) {
        // Still have recursive locks
        memory_->write_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT, static_cast<u32>(recursion - 1));
        memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, static_cast<u32>(lock_count - 1));
    } else {
        // Release lock completely
        memory_->write_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT, 0);
        memory_->write_u32(cs_ptr + CS::OFFSET_OWNING_THREAD, 0);
        memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, static_cast<u32>(-1));
    }
    
    return nt::STATUS_SUCCESS;
}

u32 KernelThreadManager::try_enter_critical_section(GuestAddr cs_ptr) {
    using CS = RTL_CRITICAL_SECTION_LAYOUT;
    
    u32 current_tid = get_current_thread_id();
    if (current_tid == 0) current_tid = 1;
    
    s32 lock_count = static_cast<s32>(memory_->read_u32(cs_ptr + CS::OFFSET_LOCK_COUNT));
    u32 owning_thread = memory_->read_u32(cs_ptr + CS::OFFSET_OWNING_THREAD);
    
    // Already owned by us?
    if (owning_thread == current_tid) {
        s32 recursion = static_cast<s32>(memory_->read_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT));
        memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, static_cast<u32>(lock_count + 1));
        memory_->write_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT, static_cast<u32>(recursion + 1));
        return 1;  // TRUE - acquired
    }
    
    // Try to acquire
    if (lock_count == -1) {
        memory_->write_u32(cs_ptr + CS::OFFSET_LOCK_COUNT, 0);
        memory_->write_u32(cs_ptr + CS::OFFSET_RECURSION_COUNT, 1);
        memory_->write_u32(cs_ptr + CS::OFFSET_OWNING_THREAD, current_tid);
        return 1;  // TRUE - acquired
    }
    
    return 0;  // FALSE - not acquired
}

u32 KernelThreadManager::delete_critical_section(GuestAddr cs_ptr) {
    using CS = RTL_CRITICAL_SECTION_LAYOUT;
    
    // Zero out the structure
    for (u32 i = 0; i < CS::SIZE; i += 4) {
        memory_->write_u32(cs_ptr + i, 0);
    }
    
    return nt::STATUS_SUCCESS;
}

//=============================================================================
// Thread Local Storage
//=============================================================================

u32 KernelThreadManager::tls_alloc() {
    std::lock_guard<std::mutex> lock(tls_mutex_);
    
    for (u32 i = 0; i < 64; i++) {
        if (!tls_slots_used_[i]) {
            tls_slots_used_[i] = true;
            LOGD("TLS allocated slot %u", i);
            return i;
        }
    }
    
    LOGW("TLS allocation failed - out of slots");
    return nt::TLS_OUT_OF_INDEXES;
}

u32 KernelThreadManager::tls_free(u32 index) {
    if (index >= 64) return 0;
    
    std::lock_guard<std::mutex> lock(tls_mutex_);
    tls_slots_used_[index] = false;
    
    // Clear the slot in all threads
    // The ThreadScheduler manages the actual TLS values per thread
    
    return 1;  // TRUE
}

u64 KernelThreadManager::tls_get_value(u32 index) {
    if (index >= 64 || !scheduler_) return 0;
    
    GuestThread* thread = scheduler_->get_current_thread(0);
    if (!thread) return 0;
    
    return thread->tls_slots[index];
}

u32 KernelThreadManager::tls_set_value(u32 index, u64 value) {
    if (index >= 64 || !scheduler_) return 0;
    
    GuestThread* thread = scheduler_->get_current_thread(0);
    if (!thread) return 0;
    
    thread->tls_slots[index] = value;
    return 1;  // TRUE
}

//=============================================================================
// Scheduler Interface
//=============================================================================

void KernelThreadManager::yield() {
    if (scheduler_) {
        GuestThread* current = scheduler_->get_current_thread(0);
        if (current) {
            scheduler_->yield(current);
        }
    }
    std::this_thread::yield();
}

u32 KernelThreadManager::delay_execution(bool alertable, s64* interval_100ns) {
    GuestThread* current = scheduler_ ? scheduler_->get_current_thread(0) : nullptr;
    
    // Check for pending APCs if alertable
    if (alertable && current && current->has_pending_apcs()) {
        current->in_alertable_wait = true;
        scheduler_->process_pending_apcs(current);
        return nt::STATUS_USER_APC;
    }
    
    // Check if already alerted
    if (alertable && current && current->alerted) {
        current->alerted = false;
        current->in_alertable_wait = true;
        scheduler_->process_pending_apcs(current);
        return nt::STATUS_ALERTED;
    }
    
    if (!interval_100ns) {
        yield();
        return nt::STATUS_SUCCESS;
    }
    
    s64 interval = *interval_100ns;
    
    // Mark thread as in alertable wait if requested
    if (alertable && current) {
        current->in_alertable_wait = true;
    }
    
    if (interval < 0) {
        // Relative time in 100ns units (negative)
        u64 microseconds = static_cast<u64>(-interval) / 10;
        if (microseconds > 0) {
            // For alertable waits, we need to check for APCs periodically
            if (alertable && current) {
                u64 remaining_us = microseconds;
                constexpr u64 check_interval_us = 1000;  // Check every 1ms
                
                while (remaining_us > 0) {
                    // Check for APCs
                    if (current->has_pending_apcs()) {
                        scheduler_->process_pending_apcs(current);
                        return nt::STATUS_USER_APC;
                    }
                    if (current->alerted) {
                        current->alerted = false;
                        scheduler_->process_pending_apcs(current);
                        return nt::STATUS_ALERTED;
                    }
                    
                    u64 sleep_us = std::min(remaining_us, check_interval_us);
                    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                    remaining_us -= sleep_us;
                }
                current->in_alertable_wait = false;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
            }
        } else {
            yield();
        }
    } else if (interval == 0) {
        yield();
    } else {
        // Absolute time - calculate delay from current time
        u64 now = get_current_time_100ns();
        if (static_cast<u64>(interval) > now) {
            u64 delay_100ns = static_cast<u64>(interval) - now;
            u64 delay_us = delay_100ns / 10;
            
            // For alertable waits, check for APCs periodically
            if (alertable && current && delay_us > 0) {
                constexpr u64 check_interval_us = 1000;  // Check every 1ms
                
                while (delay_us > 0) {
                    // Check for APCs
                    if (current->has_pending_apcs()) {
                        scheduler_->process_pending_apcs(current);
                        return nt::STATUS_USER_APC;
                    }
                    if (current->alerted) {
                        current->alerted = false;
                        scheduler_->process_pending_apcs(current);
                        return nt::STATUS_ALERTED;
                    }
                    
                    u64 sleep_us = std::min(delay_us, check_interval_us);
                    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                    delay_us -= sleep_us;
                }
                current->in_alertable_wait = false;
            } else {
                std::this_thread::sleep_for(std::chrono::nanoseconds(delay_100ns * 100));
            }
        }
    }
    
    if (alertable && current) {
        current->in_alertable_wait = false;
    }
    
    return nt::STATUS_SUCCESS;
}

//=============================================================================
// Handle Management
//=============================================================================

u32 KernelThreadManager::close_handle(u32 handle) {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    
    auto it = objects_.find(handle);
    if (it != objects_.end()) {
        objects_.erase(it);
        return nt::STATUS_SUCCESS;
    }
    
    // Check thread handles
    {
        std::lock_guard<std::mutex> th_lock(thread_handles_mutex_);
        if (thread_handles_.erase(handle) > 0) {
            return nt::STATUS_SUCCESS;
        }
    }
    
    return nt::STATUS_INVALID_HANDLE;
}

u32 KernelThreadManager::duplicate_handle(u32 source_handle, u32* target_handle) {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    
    // For simplicity, just return the same handle
    // In a full implementation, we'd create a proper duplicate
    if (target_handle) *target_handle = source_handle;
    
    return nt::STATUS_SUCCESS;
}

//=============================================================================
// Helper Methods
//=============================================================================

u32 KernelThreadManager::allocate_handle() {
    return next_handle_++;
}

KernelWaitable* KernelThreadManager::get_waitable(u32 handle) {
    auto it = objects_.find(handle);
    if (it != objects_.end()) {
        return it->second.get();
    }
    return nullptr;
}

void KernelThreadManager::wake_waiters(KernelWaitable* obj) {
    if (!obj || !scheduler_) return;
    
    // Wake threads waiting on this object
    for (auto* thread : obj->waiters) {
        if (thread && thread->state == ThreadState::Waiting) {
            thread->state = ThreadState::Ready;
            thread->wait_object = 0;
        }
    }
    obj->waiters.clear();
}

u64 KernelThreadManager::get_current_time_100ns() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() / 100;
}

} // namespace x360mu
