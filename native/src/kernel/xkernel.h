/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xbox Kernel Manager
 * Manages system state, objects, and thread execution
 */

#pragma once

#include "xobject.h"
#include "xthread.h"
#include "xevent.h"
#include <memory>
#include <functional>

namespace x360mu {

class Cpu;
class Memory;
class Kernel;

/**
 * System initialization flags
 * These flags are checked by games to ensure system is ready
 */
struct SystemFlags {
    bool kernel_initialized = false;
    bool video_initialized = false;
    bool audio_initialized = false;
    bool network_initialized = false;
    bool storage_initialized = false;
    bool xam_initialized = false;
    bool all_ready = false;
};

/**
 * XKernel - Main kernel manager
 * 
 * This is the heart of the Xbox 360 kernel emulation.
 * It manages:
 * - System initialization
 * - Object handles
 * - Thread scheduling
 * - Event dispatching
 * - DPC processing
 */
class XKernel {
public:
    static XKernel& instance();
    
    // Lifecycle
    void initialize(Cpu* cpu, Memory* memory, Kernel* hle_kernel);
    void shutdown();
    
    // System initialization
    void perform_system_init();
    const SystemFlags& system_flags() const { return system_flags_; }
    
    // Object management
    template<typename T, typename... Args>
    std::shared_ptr<T> create_object(Args&&... args) {
        auto obj = std::make_shared<T>(std::forward<Args>(args)...);
        u32 handle = KernelState::instance().object_table().add_object(obj);
        obj->set_handle(handle);
        return obj;
    }
    
    u32 create_handle(std::shared_ptr<XObject> object);
    std::shared_ptr<XObject> get_object(u32 handle);
    void close_handle(u32 handle);
    
    // Thread management
    std::shared_ptr<XThread> create_thread(
        GuestAddr entry_point,
        GuestAddr parameter,
        u32 stack_size,
        u32 creation_flags
    );
    
    std::shared_ptr<XThread> get_current_thread();
    void terminate_thread(u32 handle, u32 exit_code);
    
    // Event management
    std::shared_ptr<XEvent> create_event(XEventType type, bool initial_state);
    std::shared_ptr<XEvent> get_or_create_event(GuestAddr guest_event);
    void set_event(GuestAddr event);
    void reset_event(GuestAddr event);
    void pulse_event(GuestAddr event);
    
    // Semaphore management
    std::shared_ptr<XSemaphore> create_semaphore(s32 initial, s32 maximum);
    s32 release_semaphore(GuestAddr semaphore, s32 count);
    
    // Mutant management
    std::shared_ptr<XMutant> create_mutant(bool initial_owner);
    s32 release_mutant(GuestAddr mutant);
    
    // Wait operations
    u32 wait_for_single_object(GuestAddr object, u64 timeout_100ns);
    u32 wait_for_multiple_objects(
        u32 count,
        GuestAddr* objects,
        bool wait_all,
        u64 timeout_100ns
    );
    
    // Execution
    void run_for(u64 cycles);
    void process_timers();
    void process_dpcs();
    void process_apcs();
    
    // System state (in guest memory)
    GuestAddr get_kpcr_address(u32 processor) const;
    GuestAddr get_system_process() const;
    
    // Accessors
    Cpu* cpu() const { return cpu_; }
    Memory* memory() const { return memory_; }
    Kernel* hle_kernel() const { return hle_kernel_; }
    
private:
    XKernel() = default;
    
    void init_system_structures();
    void init_system_events();
    void init_processors();
    
    Cpu* cpu_ = nullptr;
    Memory* memory_ = nullptr;
    Kernel* hle_kernel_ = nullptr;
    
    SystemFlags system_flags_;
    
    // System structures in guest memory
    GuestAddr kpcr_base_ = 0;           // Per-processor control region
    GuestAddr system_process_ = 0;       // System process
    GuestAddr idle_thread_ = 0;          // Idle thread
    
    // System events
    std::shared_ptr<XEvent> system_ready_event_;
    std::shared_ptr<XEvent> video_ready_event_;
    
    // Object cache (for guest address -> XObject mapping)
    std::unordered_map<GuestAddr, std::weak_ptr<XObject>> guest_object_cache_;
    std::mutex cache_mutex_;
};

/**
 * Helper functions for HLE
 */
namespace xkernel {

// Thread helpers
XThread* get_current_thread();
u32 get_current_thread_id();
GuestAddr get_current_thread_handle();

// Wait helpers  
u32 nt_wait_for_single_object(GuestAddr handle, u64 timeout);
u32 ke_wait_for_single_object(GuestAddr object, u64 timeout);

// Event helpers
void ke_initialize_event(GuestAddr event, XEventType type, bool state);
s32 ke_set_event(GuestAddr event);
s32 ke_reset_event(GuestAddr event);
s32 ke_pulse_event(GuestAddr event);

// Semaphore helpers
void ke_initialize_semaphore(GuestAddr semaphore, s32 count, s32 limit);
s32 ke_release_semaphore(GuestAddr semaphore, s32 increment);

// Mutant helpers
void ke_initialize_mutant(GuestAddr mutant, bool initial_owner);
s32 ke_release_mutant(GuestAddr mutant);

// DPC helpers
void ke_initialize_dpc(GuestAddr dpc, GuestAddr routine, GuestAddr context);
bool ke_insert_queue_dpc(GuestAddr dpc);

// Timer helpers
void ke_initialize_timer(GuestAddr timer);
bool ke_set_timer(GuestAddr timer, u64 due_time, GuestAddr dpc);

// Processor helpers
u32 ke_get_current_processor_number();

} // namespace xkernel

} // namespace x360mu
