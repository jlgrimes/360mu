/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xbox Kernel Manager Implementation
 */

#include "xkernel.h"
#include "kernel.h"
#include "../cpu/xenon/cpu.h"
#include "../memory/memory.h"
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-xkernel"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[XKERNEL] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[XKERNEL WARN] " __VA_ARGS__); printf("\n")
#endif

namespace x360mu {

//=============================================================================
// XKernel
//=============================================================================

XKernel& XKernel::instance() {
    static XKernel instance;
    return instance;
}

void XKernel::initialize(Cpu* cpu, Memory* memory, Kernel* hle_kernel) {
    cpu_ = cpu;
    memory_ = memory;
    hle_kernel_ = hle_kernel;
    
    // Initialize subsystems - pass CPU to KernelState for DPC execution
    KernelState::instance().initialize(memory, cpu);
    XScheduler::instance().initialize(cpu, memory);
    
    LOGI("XKernel subsystems initialized (cpu=%s)", cpu ? "available" : "null");
    
    // Perform full system initialization
    perform_system_init();
}

void XKernel::shutdown() {
    XScheduler::instance().shutdown();
    KernelState::instance().shutdown();
    
    system_ready_event_.reset();
    video_ready_event_.reset();
    vblank_event_.reset();
    vblank_count_ = 0;
    
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        guest_object_cache_.clear();
    }
    
    cpu_ = nullptr;
    memory_ = nullptr;
    hle_kernel_ = nullptr;
    
    LOGI("XKernel shutdown complete");
}

void XKernel::perform_system_init() {
    LOGI("Performing Xbox 360 system initialization...");
    
    // Step 1: Initialize kernel data structures
    init_system_structures();
    system_flags_.kernel_initialized = true;
    LOGI("  - Kernel structures initialized");
    
    // Step 2: Initialize per-processor data (KPCR)
    init_processors();
    LOGI("  - Processor data initialized");
    
    // Step 3: Initialize system events
    init_system_events();
    LOGI("  - System events initialized");
    
    // Step 4: Mark video as initialized
    // In a real Xbox, this would be done by the video driver
    system_flags_.video_initialized = true;
    if (video_ready_event_) {
        video_ready_event_->set();
    }
    LOGI("  - Video subsystem ready");
    
    // Step 5: Mark audio as initialized
    system_flags_.audio_initialized = true;
    LOGI("  - Audio subsystem ready");
    
    // Step 6: Mark storage as initialized
    system_flags_.storage_initialized = true;
    LOGI("  - Storage subsystem ready");
    
    // Step 7: Mark network as initialized (offline mode)
    system_flags_.network_initialized = true;
    LOGI("  - Network subsystem ready");
    
    // Step 8: Mark XAM as initialized
    system_flags_.xam_initialized = true;
    LOGI("  - XAM subsystem ready");
    
    // Step 9: Signal system ready
    system_flags_.all_ready = true;
    if (system_ready_event_) {
        system_ready_event_->set();
    }
    
    // Step 10: Queue initialization timers that simulate kernel boot DPCs
    // These timers fire quickly to signal system initialization events
    // that games may be waiting for during their own initialization
    queue_initialization_timers();
    
    LOGI("System initialization complete - all subsystems ready");
}

void XKernel::init_system_structures() {
    // Use addresses in the first 512MB of physical memory which is always available
    // These structures should be at low addresses that are definitely accessible
    
    constexpr GuestAddr SYSTEM_BASE = 0x00001000;  // 4KB mark, after null page
    
    // Allocate system process (EPROCESS)
    constexpr u32 EPROCESS_SIZE = 0x300;
    system_process_ = SYSTEM_BASE;
    
    // Initialize memory (we don't need to allocate - main memory is already there)
    // Just write zeros carefully
    for (u32 i = 0; i < EPROCESS_SIZE; i += 4) {
        memory_->write_u32(system_process_ + i, 0);
    }
    
    // Set up process header
    memory_->write_u8(system_process_, static_cast<u8>(XObjectType::Process));
    memory_->write_u32(system_process_ + 4, 1);  // SignalState = signaled
    
    // Create idle thread for processor 0
    constexpr u32 KTHREAD_SIZE = 0x200;
    idle_thread_ = system_process_ + EPROCESS_SIZE;
    
    for (u32 i = 0; i < KTHREAD_SIZE; i += 4) {
        memory_->write_u32(idle_thread_ + i, 0);
    }
    
    memory_->write_u8(idle_thread_, static_cast<u8>(XObjectType::Thread));
    memory_->write_u32(idle_thread_ + 4, 0);  // SignalState
    
    LOGD("System structures at: process=0x%08X, idle_thread=0x%08X",
         system_process_, idle_thread_);
}

void XKernel::init_processors() {
    // Use addresses in physical memory that are safe
    // These addresses must be in the main memory region (first 512MB)
    constexpr u32 KPCR_SIZE = 0x1000;  // Smaller KPCR for safety
    kpcr_base_ = 0x00010000;  // Use low address that's definitely in main memory
    
    for (u32 i = 0; i < 6; i++) {
        GuestAddr kpcr = kpcr_base_ + (i * KPCR_SIZE);
        
        // First ensure memory is accessible (touch it)
        // The memory system should handle this through fastmem
        memory_->write_u32(kpcr, 0);  // Touch to ensure page is mapped
        
        // KPCR structure offsets based on Xenia's X_KPCR
        // 0x00: TLS pointer (set later by thread startup)
        memory_->write_u32(kpcr + 0x00, 0);
        
        // 0x30: PCR self-pointer
        memory_->write_u32(kpcr + 0x30, kpcr);
        
        // 0x70: Stack base (set later by thread startup)
        memory_->write_u32(kpcr + 0x70, 0);
        
        // 0x74: Stack limit (set later by thread startup)
        memory_->write_u32(kpcr + 0x74, 0);
        
        // 0x100: Current thread (starts as idle thread)
        memory_->write_u32(kpcr + 0x100, idle_thread_);
        
        // 0x10C: Current CPU number
        memory_->write_u8(kpcr + 0x10C, i);
        
        // 0x150: DPC active flag
        memory_->write_u32(kpcr + 0x150, 0);
        
        LOGD("Initialized KPCR for processor %u at 0x%08X", i, kpcr);
    }
}

void XKernel::init_system_events() {
    // Create system-ready event
    // This is the event games wait for at startup
    system_ready_event_ = create_event(XEventType::NotificationEvent, false);
    system_ready_event_->set_name("\\SystemRoot\\System32\\SystemReady");
    
    // Create video-ready event
    video_ready_event_ = create_event(XEventType::NotificationEvent, false);
    video_ready_event_->set_name("\\SystemRoot\\System32\\VideoReady");
    
    // Create VBlank event
    vblank_event_ = create_event(XEventType::SynchronizationEvent, false);
    vblank_event_->set_name("\\SystemRoot\\System32\\VBlank");
    
    // Set system initialization flags in a safe location
    // Use the first KPCR + 0x100 offset for flags
    GuestAddr flags_addr = kpcr_base_ + 0x100;
    memory_->write_u32(flags_addr + 0, 1);  // System ready flag
    memory_->write_u32(flags_addr + 4, 1);  // Video ready flag  
    memory_->write_u32(flags_addr + 8, 1);  // XAM ready flag
    
    LOGD("System events created, flags at 0x%08X", flags_addr);
}

void XKernel::queue_initialization_timers() {
    // Queue initialization timers that fire quickly to simulate kernel boot DPCs
    // These simulate the Xbox 360 kernel's internal initialization sequence
    // 
    // On a real Xbox 360, the kernel queues DPCs during system initialization
    // that signal various events when subsystems are ready. Games often wait
    // for these events during their own initialization.
    
    u64 current_time = KernelState::instance().system_time();
    
    // Timer 1: System initialization complete timer (fires after 10ms)
    // This simulates the kernel signaling that core initialization is done
    constexpr u64 INIT_DELAY_100NS = 100000;  // 10ms in 100ns units
    
    // Allocate a synthetic timer structure in guest memory
    // We use a reserved area after KPCR structures
    constexpr GuestAddr INIT_TIMER_BASE = 0x00020000;
    constexpr u32 TIMER_SIZE = 0x40;
    
    // Initialize a KTIMER structure for system init
    GuestAddr timer1 = INIT_TIMER_BASE;
    memory_->write_u8(timer1 + 0, 0x08);  // Type = NotificationTimer
    memory_->write_u8(timer1 + 1, 0);
    memory_->write_u16(timer1 + 2, TIMER_SIZE);
    memory_->write_u32(timer1 + 4, 0);  // SignalState = not signaled
    
    // Queue the timer - it will signal itself when it fires
    // No DPC needed, just the timer signal
    KernelState::instance().queue_timer(timer1, current_time + INIT_DELAY_100NS, 0, 0);
    
    // Timer 2: Video initialization timer (fires after 20ms)
    GuestAddr timer2 = INIT_TIMER_BASE + TIMER_SIZE;
    memory_->write_u8(timer2 + 0, 0x08);  // Type = NotificationTimer
    memory_->write_u8(timer2 + 1, 0);
    memory_->write_u16(timer2 + 2, TIMER_SIZE);
    memory_->write_u32(timer2 + 4, 0);  // SignalState = not signaled
    
    KernelState::instance().queue_timer(timer2, current_time + INIT_DELAY_100NS * 2, 0, 0);
    
    // Timer 3: XAM initialization timer (fires after 30ms)
    GuestAddr timer3 = INIT_TIMER_BASE + TIMER_SIZE * 2;
    memory_->write_u8(timer3 + 0, 0x08);  // Type = NotificationTimer
    memory_->write_u8(timer3 + 1, 0);
    memory_->write_u16(timer3 + 2, TIMER_SIZE);
    memory_->write_u32(timer3 + 4, 0);  // SignalState = not signaled
    
    KernelState::instance().queue_timer(timer3, current_time + INIT_DELAY_100NS * 3, 0, 0);
    
    // Timer 4: Periodic VBlank timer (fires every 16.67ms = 60Hz)
    // This ensures timer processing happens even if the game doesn't call any syscalls
    GuestAddr vblank_timer = INIT_TIMER_BASE + TIMER_SIZE * 3;
    memory_->write_u8(vblank_timer + 0, 0x08);  // Type = NotificationTimer
    memory_->write_u8(vblank_timer + 1, 0);
    memory_->write_u16(vblank_timer + 2, TIMER_SIZE);
    memory_->write_u32(vblank_timer + 4, 0);  // SignalState = not signaled
    
    constexpr u64 VBLANK_PERIOD_100NS = 166667;  // ~16.67ms in 100ns units (60Hz)
    KernelState::instance().queue_timer(vblank_timer, current_time + VBLANK_PERIOD_100NS, VBLANK_PERIOD_100NS, 0);
    
    LOGI("Queued %d initialization timers", 4);
}

u32 XKernel::create_handle(std::shared_ptr<XObject> object) {
    return KernelState::instance().object_table().add_object(object);
}

std::shared_ptr<XObject> XKernel::get_object(u32 handle) {
    return KernelState::instance().object_table().lookup(handle);
}

void XKernel::close_handle(u32 handle) {
    KernelState::instance().object_table().remove_handle(handle);
}

std::shared_ptr<XThread> XKernel::create_thread(
    GuestAddr entry_point,
    GuestAddr parameter,
    u32 stack_size,
    u32 creation_flags)
{
    auto thread = XThread::create(cpu_, memory_, entry_point, parameter,
                                   stack_size, creation_flags, false);
    
    // Add to object table
    u32 handle = create_handle(thread);
    thread->set_handle(handle);
    
    // Add to scheduler
    XScheduler::instance().add_thread(thread);
    
    return thread;
}

std::shared_ptr<XThread> XKernel::get_current_thread() {
    auto* thread = KernelState::instance().current_thread();
    if (thread) {
        return std::static_pointer_cast<XThread>(thread->shared_from_this());
    }
    return nullptr;
}

void XKernel::terminate_thread(u32 handle, u32 exit_code) {
    auto obj = get_object(handle);
    if (obj && obj->type() == XObjectType::Thread) {
        auto thread = std::static_pointer_cast<XThread>(obj);
        thread->terminate(exit_code);
    }
}

std::shared_ptr<XEvent> XKernel::create_event(XEventType type, bool initial_state) {
    auto event = std::make_shared<XEvent>(type, initial_state);
    create_handle(event);
    return event;
}

std::shared_ptr<XEvent> XKernel::get_or_create_event(GuestAddr guest_event) {
    if (guest_event == 0) return nullptr;
    
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = guest_object_cache_.find(guest_event);
        if (it != guest_object_cache_.end()) {
            if (auto obj = it->second.lock()) {
                return std::static_pointer_cast<XEvent>(obj);
            }
        }
    }
    
    // Create from guest memory
    auto event = XEvent::create_from_guest(memory_, guest_event);
    if (event) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        guest_object_cache_[guest_event] = event;
    }
    
    return event;
}

void XKernel::set_event(GuestAddr event_addr) {
    // Update guest memory directly
    memory_->write_u32(event_addr + 4, 1);  // SignalState = 1
    
    // Also update XEvent if cached
    auto event = get_or_create_event(event_addr);
    if (event) {
        event->set();
    }
}

void XKernel::reset_event(GuestAddr event_addr) {
    memory_->write_u32(event_addr + 4, 0);  // SignalState = 0
    
    auto event = get_or_create_event(event_addr);
    if (event) {
        event->reset();
    }
}

void XKernel::pulse_event(GuestAddr event_addr) {
    auto event = get_or_create_event(event_addr);
    if (event) {
        event->pulse();
    }
    memory_->write_u32(event_addr + 4, 0);  // Reset after pulse
}

std::shared_ptr<XSemaphore> XKernel::create_semaphore(s32 initial, s32 maximum) {
    auto sem = std::make_shared<XSemaphore>(initial, maximum);
    create_handle(sem);
    return sem;
}

s32 XKernel::release_semaphore(GuestAddr semaphore, s32 count) {
    s32 prev = static_cast<s32>(memory_->read_u32(semaphore + 4));
    s32 limit = static_cast<s32>(memory_->read_u32(semaphore + 16));
    s32 new_count = std::min(prev + count, limit);
    memory_->write_u32(semaphore + 4, static_cast<u32>(new_count));
    return prev;
}

std::shared_ptr<XMutant> XKernel::create_mutant(bool initial_owner) {
    auto mutant = std::make_shared<XMutant>(initial_owner);
    create_handle(mutant);
    return mutant;
}

s32 XKernel::release_mutant(GuestAddr mutant) {
    s32 prev = static_cast<s32>(memory_->read_u32(mutant + 4));
    memory_->write_u32(mutant + 4, static_cast<u32>(prev + 1));
    memory_->write_u32(mutant + 16, 0);  // Clear owner
    return prev;
}

u32 XKernel::wait_for_single_object(GuestAddr object, u64 timeout_100ns) {
    // Read object type
    u8 type = memory_->read_u8(object);
    
    // Check if already signaled
    s32 signal_state = static_cast<s32>(memory_->read_u32(object + 4));
    
    if (signal_state != 0) {
        // Already signaled
        if (type == static_cast<u8>(XObjectType::SynchronizationEvent)) {
            // Auto-reset
            memory_->write_u32(object + 4, 0);
        } else if (type == static_cast<u8>(XObjectType::Semaphore)) {
            // Decrement
            memory_->write_u32(object + 4, static_cast<u32>(signal_state - 1));
        }
        return WAIT_OBJECT_0;
    }
    
    // Not signaled - check timeout
    if (timeout_100ns == 0) {
        return WAIT_TIMEOUT;
    }
    
    // Try proper blocking wait via XThread
    auto current = get_current_thread();
    if (current) {
        auto event = get_or_create_event(object);
        if (event) {
            return current->wait(event.get(), timeout_100ns);
        }
    }

    // Fallback: poll with bounded spin for short timeouts
    if (timeout_100ns != UINT64_MAX) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::microseconds(timeout_100ns / 10);
        while (std::chrono::steady_clock::now() < deadline) {
            s32 sig = static_cast<s32>(memory_->read_u32(object + 4));
            if (sig != 0) {
                if (type == static_cast<u8>(XObjectType::SynchronizationEvent)) {
                    memory_->write_u32(object + 4, 0);
                } else if (type == static_cast<u8>(XObjectType::Semaphore)) {
                    memory_->write_u32(object + 4, static_cast<u32>(sig - 1));
                }
                return WAIT_OBJECT_0;
            }
            std::this_thread::yield();
        }
    }

    return WAIT_TIMEOUT;
}

u32 XKernel::wait_for_multiple_objects(
    u32 count,
    GuestAddr* objects,
    bool wait_all,
    u64 timeout_100ns)
{
    // Simplified implementation
    for (u32 i = 0; i < count; i++) {
        if (!objects[i]) continue;
        
        s32 signal_state = static_cast<s32>(memory_->read_u32(objects[i] + 4));
        
        if (signal_state != 0) {
            if (!wait_all) {
                return WAIT_OBJECT_0 + i;
            }
        } else if (wait_all) {
            return wait_for_single_object(objects[i], timeout_100ns);
        }
    }
    
    if (wait_all) {
        return WAIT_OBJECT_0;
    }
    
    return WAIT_TIMEOUT;
}

void XKernel::run_for(u64 cycles) {
    // Process timer queue first (fires DPCs for expired timers)
    KernelState::instance().process_timer_queue();
    
    // Process pending DPCs (including any just queued by timers)
    KernelState::instance().process_dpcs();
    
    // Also call XScheduler DPC processing for compatibility
    process_dpcs();
    
    // Run scheduled threads
    XScheduler::instance().run_for(cycles);
    
    // Process XScheduler timers
    process_timers();
    
    // Process APCs
    process_apcs();
}

void XKernel::process_timers() {
    XScheduler::instance().process_timers();
}

void XKernel::process_dpcs() {
    XScheduler::instance().process_dpcs();
}

void XKernel::process_apcs() {
    auto current = get_current_thread();
    if (current) {
        current->deliver_apcs();
    }
}

void XKernel::signal_vblank() {
    // Increment VBlank counter
    vblank_count_++;
    
    // Signal VBlank event if one exists
    if (vblank_event_) {
        vblank_event_->set();
    }
    
    // Process timers - this ensures timer-based DPCs fire at frame rate
    KernelState::instance().process_timer_queue();
    
    // Process any pending DPCs after timer processing
    KernelState::instance().process_dpcs();
    
    // Log occasionally
    if ((vblank_count_ % 60) == 0) {
        LOGI("VBlank #%u (1 second elapsed)", vblank_count_);
    }
}

void XKernel::set_gpu_interrupt_event(GuestAddr event_addr) {
    KernelState::instance().set_gpu_interrupt_event(event_addr);
    LOGI("GPU interrupt event set to 0x%08X", event_addr);
}

GuestAddr XKernel::get_kpcr_address(u32 processor) const {
    if (processor >= 6) return 0;
    return kpcr_base_ + (processor * 0x1000);  // KPCR_SIZE = 0x1000
}

GuestAddr XKernel::get_system_process() const {
    return system_process_;
}

//=============================================================================
// Helper functions
//=============================================================================

namespace xkernel {

XThread* get_current_thread() {
    return KernelState::instance().current_thread();
}

u32 get_current_thread_id() {
    auto* thread = get_current_thread();
    return thread ? thread->thread_id() : 0;
}

GuestAddr get_current_thread_handle() {
    auto* thread = get_current_thread();
    return thread ? thread->guest_thread() : 0;
}

u32 nt_wait_for_single_object(GuestAddr handle, u64 timeout) {
    return XKernel::instance().wait_for_single_object(handle, timeout);
}

u32 ke_wait_for_single_object(GuestAddr object, u64 timeout) {
    return XKernel::instance().wait_for_single_object(object, timeout);
}

void ke_initialize_event(GuestAddr event, XEventType type, bool state) {
    auto* memory = XKernel::instance().memory();
    
    memory->write_u8(event + 0, static_cast<u8>(type));
    memory->write_u8(event + 1, 0);  // Absolute
    memory->write_u8(event + 2, sizeof(u32) * 4);  // Size
    memory->write_u8(event + 3, 0);  // Inserted
    memory->write_u32(event + 4, state ? 1 : 0);  // SignalState
}

s32 ke_set_event(GuestAddr event) {
    auto* memory = XKernel::instance().memory();
    s32 prev = static_cast<s32>(memory->read_u32(event + 4));
    XKernel::instance().set_event(event);
    return prev;
}

s32 ke_reset_event(GuestAddr event) {
    auto* memory = XKernel::instance().memory();
    s32 prev = static_cast<s32>(memory->read_u32(event + 4));
    XKernel::instance().reset_event(event);
    return prev;
}

s32 ke_pulse_event(GuestAddr event) {
    auto* memory = XKernel::instance().memory();
    s32 prev = static_cast<s32>(memory->read_u32(event + 4));
    XKernel::instance().pulse_event(event);
    return prev;
}

void ke_initialize_semaphore(GuestAddr semaphore, s32 count, s32 limit) {
    auto* memory = XKernel::instance().memory();
    
    memory->write_u8(semaphore + 0, static_cast<u8>(XObjectType::Semaphore));
    memory->write_u8(semaphore + 2, sizeof(u32) * 8);
    memory->write_u32(semaphore + 4, static_cast<u32>(count));
    memory->write_u32(semaphore + 16, static_cast<u32>(limit));
}

s32 ke_release_semaphore(GuestAddr semaphore, s32 increment) {
    return XKernel::instance().release_semaphore(semaphore, increment);
}

void ke_initialize_mutant(GuestAddr mutant, bool initial_owner) {
    auto* memory = XKernel::instance().memory();
    
    memory->write_u8(mutant + 0, static_cast<u8>(XObjectType::Mutant));
    memory->write_u8(mutant + 2, sizeof(u32) * 8);
    memory->write_u32(mutant + 4, initial_owner ? 0 : 1);  // SignalState
    memory->write_u32(mutant + 16, 0);  // Owner
}

s32 ke_release_mutant(GuestAddr mutant) {
    return XKernel::instance().release_mutant(mutant);
}

void ke_initialize_dpc(GuestAddr dpc, GuestAddr routine, GuestAddr context) {
    auto* memory = XKernel::instance().memory();
    
    memory->write_u32(dpc + 0, 0);  // Type/Number
    memory->write_u32(dpc + 8, routine);  // DeferredRoutine
    memory->write_u32(dpc + 12, context);  // DeferredContext
}

bool ke_insert_queue_dpc(GuestAddr dpc) {
    auto* memory = XKernel::instance().memory();
    
    // Read DPC structure fields per Xbox 360 layout:
    // Offset 0x08 = DeferredRoutine (note: ke_initialize_dpc uses 8, spec says 0x0C)
    // Offset 0x0C = DeferredContext (note: ke_initialize_dpc uses 12, spec says 0x10)
    GuestAddr routine = memory->read_u32(dpc + 8);
    GuestAddr context = memory->read_u32(dpc + 12);
    
    // Read system arguments from standard KDPC offsets
    GuestAddr arg1 = memory->read_u32(dpc + 0x14);
    GuestAddr arg2 = memory->read_u32(dpc + 0x18);
    
    if (routine) {
        KernelState::instance().queue_dpc(dpc, routine, context, arg1, arg2);
        return true;
    }
    return false;
}

void ke_initialize_timer(GuestAddr timer) {
    auto* memory = XKernel::instance().memory();
    
    memory->write_u8(timer + 0, static_cast<u8>(XObjectType::TimerNotification));
    memory->write_u8(timer + 2, sizeof(u32) * 16);
    memory->write_u32(timer + 4, 0);  // SignalState
}

bool ke_set_timer(GuestAddr timer, u64 due_time, GuestAddr dpc) {
    // Queue the timer using KernelState
    // due_time is absolute in 100ns units
    KernelState::instance().queue_timer(timer, due_time, 0, dpc);
    return true;
}

u32 ke_get_current_processor_number() {
    auto* thread = get_current_thread();
    return thread ? thread->cpu_thread_id() : 0;
}

} // namespace xkernel

} // namespace x360mu
