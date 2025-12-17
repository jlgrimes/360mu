/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel Threading & Synchronization HLE
 * 
 * This module provides kernel-level threading and synchronization primitives
 * for Xbox 360 emulation including:
 * - Thread creation and management (ExCreateThread, NtTerminateThread, etc.)
 * - Synchronization objects (Events, Semaphores, Mutants)
 * - Critical sections
 * - Thread Local Storage (TLS)
 * - Wait functions
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <array>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>

namespace x360mu {

// Forward declarations
class Memory;
class Cpu;
class ThreadScheduler;
struct GuestThread;

//=============================================================================
// NTSTATUS Codes
//=============================================================================

namespace nt {
    constexpr u32 STATUS_SUCCESS = 0x00000000;
    constexpr u32 STATUS_UNSUCCESSFUL = 0xC0000001;
    constexpr u32 STATUS_NOT_IMPLEMENTED = 0xC0000002;
    constexpr u32 STATUS_INVALID_HANDLE = 0xC0000008;
    constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000D;
    constexpr u32 STATUS_NO_MEMORY = 0xC0000017;
    constexpr u32 STATUS_TIMEOUT = 0x00000102;
    constexpr u32 STATUS_PENDING = 0x00000103;
    constexpr u32 STATUS_WAIT_0 = 0x00000000;
    constexpr u32 STATUS_ABANDONED_WAIT_0 = 0x00000080;
    constexpr u32 STATUS_ALERTED = 0x00000101;
    constexpr u32 STATUS_USER_APC = 0x000000C0;
    constexpr u32 STATUS_MUTANT_NOT_OWNED = 0xC0000046;
    constexpr u32 STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
    
    constexpr u32 TLS_OUT_OF_INDEXES = 0xFFFFFFFF;
    constexpr u32 CREATE_SUSPENDED = 0x00000004;
    constexpr u64 INFINITE_TIMEOUT = ~0ULL;
}

//=============================================================================
// Event Types
//=============================================================================

enum class EventType : u32 {
    NotificationEvent = 0,      // Manual reset - stays signaled until explicitly reset
    SynchronizationEvent = 1,   // Auto-reset - resets after waking one waiter
};

//=============================================================================
// Wait Types
//=============================================================================

enum class WaitType : u32 {
    WaitAll = 0,    // Wait for all objects to be signaled
    WaitAny = 1,    // Wait for any object to be signaled
};

//=============================================================================
// Kernel Object Types for Waitable Objects
//=============================================================================

enum class KernelWaitableType : u32 {
    None = 0,
    Event = 1,
    Semaphore = 2,
    Mutant = 3,
    Thread = 4,
    Timer = 5,
};

//=============================================================================
// Kernel Waitable Object Base
//=============================================================================

struct KernelWaitable {
    KernelWaitableType type = KernelWaitableType::None;
    u32 handle = 0;
    std::string name;
    
    // List of threads waiting on this object
    std::vector<GuestThread*> waiters;
    
    virtual ~KernelWaitable() = default;
    virtual bool is_signaled() const = 0;
    virtual void on_wait_satisfied(GuestThread* thread) = 0;
};

//=============================================================================
// Kernel Event
//=============================================================================

struct KernelEvent : public KernelWaitable {
    EventType event_type = EventType::NotificationEvent;
    bool signaled = false;
    
    KernelEvent() { type = KernelWaitableType::Event; }
    
    bool is_signaled() const override { return signaled; }
    
    void on_wait_satisfied(GuestThread* thread) override {
        // Auto-reset for synchronization events
        if (event_type == EventType::SynchronizationEvent) {
            signaled = false;
        }
    }
};

//=============================================================================
// Kernel Semaphore
//=============================================================================

struct KernelSemaphore : public KernelWaitable {
    s32 count = 0;
    s32 max_count = 1;
    
    KernelSemaphore() { type = KernelWaitableType::Semaphore; }
    
    bool is_signaled() const override { return count > 0; }
    
    void on_wait_satisfied(GuestThread* thread) override {
        if (count > 0) {
            count--;
        }
    }
};

//=============================================================================
// Kernel Mutant (Mutex)
//=============================================================================

struct KernelMutant : public KernelWaitable {
    GuestThread* owner = nullptr;
    u32 owner_thread_id = 0;
    u32 recursion_count = 0;
    bool abandoned = false;
    
    KernelMutant() { type = KernelWaitableType::Mutant; }
    
    bool is_signaled() const override { return owner == nullptr; }
    
    void on_wait_satisfied(GuestThread* thread) override;  // Defined in .cpp
};

//=============================================================================
// RTL_CRITICAL_SECTION Layout
// 
// Fast user-mode synchronization primitive stored in guest memory
//=============================================================================

struct RTL_CRITICAL_SECTION_LAYOUT {
    static constexpr u32 OFFSET_DEBUG_INFO = 0;       // PRTL_CRITICAL_SECTION_DEBUG
    static constexpr u32 OFFSET_LOCK_COUNT = 4;       // LONG
    static constexpr u32 OFFSET_RECURSION_COUNT = 8;  // LONG
    static constexpr u32 OFFSET_OWNING_THREAD = 12;   // HANDLE
    static constexpr u32 OFFSET_LOCK_SEMAPHORE = 16;  // HANDLE
    static constexpr u32 OFFSET_SPIN_COUNT = 20;      // ULONG_PTR
    static constexpr u32 SIZE = 24;
    
    // LockCount values:
    // -1 = unlocked
    // 0  = locked, no waiters
    // >0 = locked, waiters present
};

//=============================================================================
// Kernel Thread Manager
//
// Manages kernel-level thread objects and synchronization primitives.
// Works in conjunction with ThreadScheduler for actual scheduling.
//=============================================================================

class KernelThreadManager {
public:
    KernelThreadManager();
    ~KernelThreadManager();
    
    /**
     * Initialize the thread manager
     */
    Status initialize(Memory* memory, Cpu* cpu, ThreadScheduler* scheduler);
    
    /**
     * Shutdown and cleanup
     */
    void shutdown();
    
    /**
     * Reset to initial state
     */
    void reset();
    
    //=========================================================================
    // Thread Management
    //=========================================================================
    
    /**
     * Create a new thread (ExCreateThread)
     * @param handle_out Output handle
     * @param stack_size Stack size in bytes
     * @param thread_id_out Output thread ID
     * @param xapi_startup XAPI thread startup wrapper
     * @param start_address Thread entry point
     * @param start_param Parameter passed to thread
     * @param creation_flags CREATE_SUSPENDED, etc.
     * @return NTSTATUS
     */
    u32 create_thread(u32* handle_out, u32 stack_size, u32* thread_id_out,
                      GuestAddr xapi_startup, GuestAddr start_address,
                      GuestAddr start_param, u32 creation_flags);
    
    /**
     * Terminate a thread
     */
    u32 terminate_thread(u32 handle, u32 exit_code);
    
    /**
     * Suspend a thread
     * @return Previous suspend count, or error
     */
    u32 suspend_thread(u32 handle, u32* prev_count);
    
    /**
     * Resume a thread
     * @return Previous suspend count, or error
     */
    u32 resume_thread(u32 handle, u32* prev_count);
    
    /**
     * Get current thread handle
     */
    u32 get_current_thread_handle();
    
    /**
     * Get current thread ID
     */
    u32 get_current_thread_id();
    
    /**
     * Get current processor number (0-5)
     */
    u32 get_current_processor();
    
    /**
     * Set thread affinity
     */
    u32 set_thread_affinity(u32 handle, u32 affinity_mask, u32* prev_affinity);
    
    /**
     * Set thread priority
     */
    u32 set_thread_priority(u32 handle, s32 priority);
    
    //=========================================================================
    // Event Management
    //=========================================================================
    
    /**
     * Create an event (NtCreateEvent)
     */
    u32 create_event(u32* handle_out, u32 access_mask, GuestAddr obj_attr,
                     EventType event_type, bool initial_state);
    
    /**
     * Set (signal) an event
     * @return Previous state, or error
     */
    u32 set_event(u32 handle, s32* prev_state);
    
    /**
     * Clear (reset) an event
     */
    u32 clear_event(u32 handle);
    
    /**
     * Pulse an event (set then immediately reset)
     */
    u32 pulse_event(u32 handle, s32* prev_state);
    
    //=========================================================================
    // Semaphore Management
    //=========================================================================
    
    /**
     * Create a semaphore (NtCreateSemaphore)
     */
    u32 create_semaphore(u32* handle_out, u32 access_mask, GuestAddr obj_attr,
                         s32 initial_count, s32 max_count);
    
    /**
     * Release a semaphore
     * @return Previous count, or error
     */
    u32 release_semaphore(u32 handle, s32 release_count, s32* prev_count);
    
    //=========================================================================
    // Mutant (Mutex) Management
    //=========================================================================
    
    /**
     * Create a mutant (NtCreateMutant)
     */
    u32 create_mutant(u32* handle_out, u32 access_mask, GuestAddr obj_attr,
                      bool initial_owner);
    
    /**
     * Release a mutant
     */
    u32 release_mutant(u32 handle, bool abandoned, s32* prev_count);
    
    //=========================================================================
    // Wait Functions
    //=========================================================================
    
    /**
     * Wait for a single object (NtWaitForSingleObject)
     * @param timeout_100ns Timeout in 100ns units (negative = relative, positive = absolute)
     *                      nullptr = infinite
     */
    u32 wait_for_single_object(u32 handle, bool alertable, s64* timeout_100ns);
    
    /**
     * Wait for multiple objects (NtWaitForMultipleObjects)
     */
    u32 wait_for_multiple_objects(u32 count, const u32* handles,
                                   WaitType wait_type, bool alertable,
                                   s64* timeout_100ns);
    
    //=========================================================================
    // Critical Section Support
    //=========================================================================
    
    /**
     * Initialize a critical section (RtlInitializeCriticalSection)
     */
    void init_critical_section(GuestAddr cs_ptr);
    
    /**
     * Initialize a critical section with spin count
     */
    u32 init_critical_section_with_spin(GuestAddr cs_ptr, u32 spin_count);
    
    /**
     * Enter a critical section (RtlEnterCriticalSection)
     */
    u32 enter_critical_section(GuestAddr cs_ptr);
    
    /**
     * Leave a critical section (RtlLeaveCriticalSection)
     */
    u32 leave_critical_section(GuestAddr cs_ptr);
    
    /**
     * Try to enter a critical section (RtlTryEnterCriticalSection)
     * @return Non-zero if acquired, zero if not
     */
    u32 try_enter_critical_section(GuestAddr cs_ptr);
    
    /**
     * Delete a critical section (RtlDeleteCriticalSection)
     */
    u32 delete_critical_section(GuestAddr cs_ptr);
    
    //=========================================================================
    // Thread Local Storage
    //=========================================================================
    
    /**
     * Allocate a TLS slot
     * @return Slot index, or TLS_OUT_OF_INDEXES
     */
    u32 tls_alloc();
    
    /**
     * Free a TLS slot
     * @return TRUE on success
     */
    u32 tls_free(u32 index);
    
    /**
     * Get TLS value for current thread
     */
    u64 tls_get_value(u32 index);
    
    /**
     * Set TLS value for current thread
     * @return TRUE on success
     */
    u32 tls_set_value(u32 index, u64 value);
    
    //=========================================================================
    // Scheduler Interface
    //=========================================================================
    
    /**
     * Yield execution from current thread
     */
    void yield();
    
    /**
     * Sleep current thread
     * @param interval_100ns Sleep time in 100ns units (negative = relative)
     */
    u32 delay_execution(bool alertable, s64* interval_100ns);
    
    //=========================================================================
    // Handle Management
    //=========================================================================
    
    /**
     * Close any kernel handle
     */
    u32 close_handle(u32 handle);
    
    /**
     * Duplicate a handle
     */
    u32 duplicate_handle(u32 source_handle, u32* target_handle);
    
    //=========================================================================
    // Statistics
    //=========================================================================
    
    struct Stats {
        u64 threads_created;
        u64 events_created;
        u64 semaphores_created;
        u64 mutants_created;
        u64 total_waits;
        u64 wait_timeouts;
    };
    
    Stats get_stats() const { return stats_; }
    
private:
    Memory* memory_ = nullptr;
    Cpu* cpu_ = nullptr;
    ThreadScheduler* scheduler_ = nullptr;
    
    // Kernel objects (events, semaphores, mutants)
    std::unordered_map<u32, std::unique_ptr<KernelWaitable>> objects_;
    std::mutex objects_mutex_;
    
    // Thread handle to GuestThread* mapping (managed by ThreadScheduler)
    // We just track handles here for lookup
    std::unordered_map<u32, u32> thread_handles_;  // handle -> thread_id
    std::mutex thread_handles_mutex_;
    
    // TLS slot allocation
    std::array<bool, 64> tls_slots_used_;
    std::mutex tls_mutex_;
    
    // Handle generation
    std::atomic<u32> next_handle_{0x80001000};
    
    // Statistics
    Stats stats_ = {};
    
    // Helper methods
    u32 allocate_handle();
    KernelWaitable* get_waitable(u32 handle);
    void wake_waiters(KernelWaitable* obj);
    u64 get_current_time_100ns() const;
    bool check_wait_satisfied(const std::vector<KernelWaitable*>& objects, WaitType wait_type);
    u32 perform_wait(const std::vector<KernelWaitable*>& objects, WaitType wait_type,
                     bool alertable, s64* timeout_100ns);
};

// Global accessor (set during kernel initialization)
KernelThreadManager* get_kernel_thread_manager();
void set_kernel_thread_manager(KernelThreadManager* manager);

} // namespace x360mu
