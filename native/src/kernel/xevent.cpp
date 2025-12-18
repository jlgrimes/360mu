/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Event Objects Implementation
 */

#include "xevent.h"
#include "xthread.h"
#include "../memory/memory.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-xevent"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[XEVENT] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#endif

namespace x360mu {

//=============================================================================
// XEvent
//=============================================================================

XEvent::XEvent(XEventType type, bool initial_state)
    : XObject(type == XEventType::NotificationEvent 
              ? XObjectType::NotificationEvent 
              : XObjectType::SynchronizationEvent)
    , event_type_(type)
    , signaled_(initial_state)
{
}

XEvent::~XEvent() {
    wake_waiters();
}

std::shared_ptr<XEvent> XEvent::create_from_guest(Memory* memory, GuestAddr event_addr) {
    if (!memory || event_addr == 0) return nullptr;
    
    // Read dispatcher header
    u8 type = memory->read_u8(event_addr);
    u32 signal_state = memory->read_u32(event_addr + 4);
    
    XEventType event_type = (type == 0) ? XEventType::NotificationEvent 
                                        : XEventType::SynchronizationEvent;
    
    auto event = std::make_shared<XEvent>(event_type, signal_state != 0);
    event->set_guest_object(event_addr);
    
    return event;
}

void XEvent::set() {
    signaled_ = true;
    
    // Update guest memory if we have a guest object
    if (guest_object_) {
        // Write signal state at offset +4
        // We need memory access - this should be done via KernelState
        // For now, just set our local state
    }
    
    // Wake waiters
    if (event_type_ == XEventType::NotificationEvent) {
        // Manual reset - wake all waiters
        wake_waiters();
    } else {
        // Auto reset - wake one waiter and reset
        wake_waiters(1);
        signaled_ = false;
    }
    
    LOGD("Event set: signaled=%d, type=%d", signaled_.load(), 
         static_cast<int>(event_type_));
}

void XEvent::reset() {
    signaled_ = false;
}

void XEvent::pulse() {
    // Set then immediately reset
    signaled_ = true;
    wake_waiters();
    signaled_ = false;
}

bool XEvent::is_signaled() const {
    return signaled_.load();
}

void XEvent::signal() {
    set();
}

void XEvent::unsignal() {
    reset();
}

//=============================================================================
// XSemaphore
//=============================================================================

XSemaphore::XSemaphore(s32 initial_count, s32 maximum_count)
    : XObject(XObjectType::Semaphore)
    , count_(initial_count)
    , maximum_(maximum_count)
{
}

XSemaphore::~XSemaphore() {
    wake_waiters();
}

s32 XSemaphore::release(s32 count) {
    s32 prev = count_.load();
    s32 new_count = std::min(prev + count, maximum_);
    count_.store(new_count);
    
    // Wake waiters
    if (new_count > 0) {
        wake_waiters(static_cast<u32>(new_count));
    }
    
    return prev;
}

bool XSemaphore::is_signaled() const {
    return count_.load() > 0;
}

//=============================================================================
// XMutant
//=============================================================================

XMutant::XMutant(bool initial_owner)
    : XObject(XObjectType::Mutant)
{
    if (initial_owner) {
        // The creating thread owns it initially
        owner_ = KernelState::instance().current_thread();
        recursion_count_ = 1;
    }
}

XMutant::~XMutant() {
    wake_waiters();
}

bool XMutant::acquire(XThread* thread, u64 timeout_100ns) {
    (void)timeout_100ns;  // TODO: Implement timeout
    
    std::lock_guard<std::mutex> lock(acquire_mutex_);
    
    XThread* current_owner = owner_.load();
    
    if (current_owner == nullptr) {
        // Not owned - acquire it
        owner_ = thread;
        recursion_count_ = 1;
        return true;
    }
    
    if (current_owner == thread) {
        // Already owned by this thread - increment recursion
        recursion_count_++;
        return true;
    }
    
    // Owned by another thread - would need to wait
    return false;
}

s32 XMutant::release() {
    s32 prev_count = recursion_count_.load();
    
    if (prev_count > 0) {
        recursion_count_--;
        
        if (recursion_count_ == 0) {
            owner_ = nullptr;
            wake_waiters(1);
        }
    }
    
    return prev_count;
}

bool XMutant::is_signaled() const {
    return owner_.load() == nullptr;
}

//=============================================================================
// XTimer
//=============================================================================

XTimer::XTimer(XEventType type)
    : XObject(type == XEventType::NotificationEvent
              ? XObjectType::TimerNotification
              : XObjectType::TimerSynchronization)
    , timer_type_(type)
{
}

XTimer::~XTimer() {
    cancel();
    wake_waiters();
}

void XTimer::set(u64 due_time_100ns, u64 period_ms, 
                 GuestAddr dpc_routine, GuestAddr dpc_context) {
    due_time_ = due_time_100ns;
    period_ms_ = period_ms;
    dpc_routine_ = dpc_routine;
    dpc_context_ = dpc_context;
    active_ = true;
    signaled_ = false;
    
    LOGD("Timer set: due=%llu, period=%llu, dpc=0x%08X",
         (unsigned long long)due_time_, (unsigned long long)period_ms_, dpc_routine);
}

void XTimer::cancel() {
    active_ = false;
    signaled_ = false;
}

bool XTimer::is_signaled() const {
    return signaled_.load();
}

void XTimer::check_and_fire(u64 current_time_100ns) {
    if (!active_) return;
    
    if (current_time_100ns >= due_time_) {
        signaled_ = true;
        
        // Queue DPC if specified
        if (dpc_routine_) {
            KernelState::instance().queue_dpc(dpc_routine_, dpc_context_);
        }
        
        // Wake waiters
        if (timer_type_ == XEventType::NotificationEvent) {
            wake_waiters();
        } else {
            wake_waiters(1);
            signaled_ = false;
        }
        
        // Handle periodic timer
        if (period_ms_ > 0) {
            due_time_ = current_time_100ns + (period_ms_ * 10000);
        } else {
            active_ = false;
        }
        
        LOGD("Timer fired");
    }
}

} // namespace x360mu
