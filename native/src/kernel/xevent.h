/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Event Object
 */

#pragma once

#include "xobject.h"
#include <atomic>
#include <condition_variable>
#include <chrono>

namespace x360mu {

/**
 * Event type
 */
enum class XEventType {
    NotificationEvent = 0,    // Manual reset
    SynchronizationEvent = 1  // Auto reset
};

/**
 * XEvent - Kernel event object
 */
class XEvent : public XObject {
public:
    // Use notification event as default type for object lookup
    X_OBJECT_TYPE(XEvent, XObjectType::NotificationEvent);
    
    XEvent(XEventType type, bool initial_state);
    ~XEvent() override;
    
    // Create from guest memory event structure
    static std::shared_ptr<XEvent> create_from_guest(
        Memory* memory,
        GuestAddr event_addr
    );
    
    // Event operations
    void set();
    void reset();
    void pulse();
    
    // Wait support
    bool is_signaled() const override;
    void signal() override;
    void unsignal() override;
    
    // Properties
    XEventType event_type() const { return event_type_; }
    bool is_manual_reset() const { 
        return event_type_ == XEventType::NotificationEvent; 
    }
    
private:
    XEventType event_type_;
    std::atomic<bool> signaled_{false};
};

/**
 * XSemaphore - Kernel semaphore object
 */
class XSemaphore : public XObject {
public:
    X_OBJECT_TYPE(XSemaphore, XObjectType::Semaphore);
    
    XSemaphore(s32 initial_count, s32 maximum_count);
    ~XSemaphore() override;
    
    // Semaphore operations
    s32 release(s32 count);
    
    // Wait support
    bool is_signaled() const override;
    
    // Properties
    s32 count() const { return count_.load(); }
    s32 maximum() const { return maximum_; }
    
private:
    std::atomic<s32> count_;
    s32 maximum_;
};

/**
 * XMutant - Kernel mutex object (called "Mutant" in Xbox kernel)
 */
class XMutant : public XObject {
public:
    X_OBJECT_TYPE(XMutant, XObjectType::Mutant);
    
    XMutant(bool initial_owner);
    ~XMutant() override;
    
    // Mutex operations
    bool acquire(XThread* thread, u64 timeout_100ns);
    s32 release();
    
    // Wait support
    bool is_signaled() const override;
    
    // Properties
    XThread* owner() const { return owner_; }
    s32 recursion_count() const { return recursion_count_; }
    bool is_abandoned() const { return abandoned_; }
    
private:
    std::atomic<XThread*> owner_{nullptr};
    std::atomic<s32> recursion_count_{0};
    bool abandoned_ = false;
    std::mutex acquire_mutex_;
    std::condition_variable acquire_cv_;
};

/**
 * XTimer - Kernel timer object
 */
class XTimer : public XObject {
public:
    // Timer can be notification or synchronization type
    X_OBJECT_TYPE(XTimer, XObjectType::TimerNotification);
    
    XTimer(XEventType type);
    ~XTimer() override;
    
    // Timer operations
    void set(u64 due_time_100ns, u64 period_ms, GuestAddr dpc_routine, GuestAddr dpc_context);
    void cancel();
    
    // Wait support  
    bool is_signaled() const override;
    
    // Check and fire if due
    void check_and_fire(u64 current_time_100ns);
    
    // Properties
    bool is_periodic() const { return period_ms_ > 0; }
    u64 due_time() const { return due_time_; }
    
private:
    XEventType timer_type_;
    std::atomic<bool> signaled_{false};
    std::atomic<bool> active_{false};
    u64 due_time_ = 0;
    u64 period_ms_ = 0;
    GuestAddr dpc_routine_ = 0;
    GuestAddr dpc_context_ = 0;
};

} // namespace x360mu
