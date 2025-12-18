/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xbox Kernel Manager Implementation
 */

#include "xkernel.h"
#include "kernel.h"
#include "../cpu/xenon/cpu.h"
#include "../memory/memory.h"

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
    
    // Initialize subsystems
    KernelState::instance().initialize(memory);
    XScheduler::instance().initialize(cpu, memory);
    
    LOGI("XKernel subsystems initialized");
    
    // Perform full system initialization
    perform_system_init();
}

void XKernel::shutdown() {
    XScheduler::instance().shutdown();
    KernelState::instance().shutdown();
    
    system_ready_event_.reset();
    video_ready_event_.reset();
    
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
    
    LOGI("System initialization complete - all subsystems ready");
}

void XKernel::init_system_structures() {
    // Use addresses in physical memory range that's already mapped
    // The 0x80000000+ range is virtual and maps to physical 0x00000000+
    // Use a safe range in physical memory that won't conflict with game code
    
    constexpr GuestAddr SYSTEM_BASE = 0x00100000;  // 1MB mark, safe area
    
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
    constexpr u32 KPCR_SIZE = 0x1000;  // Smaller KPCR for safety
    kpcr_base_ = 0x00110000;  // Right after system structures
    
    for (u32 i = 0; i < 6; i++) {
        GuestAddr kpcr = kpcr_base_ + (i * KPCR_SIZE);
        
        // Zero manually
        for (u32 j = 0; j < KPCR_SIZE; j += 4) {
            memory_->write_u32(kpcr + j, 0);
        }
        
        // KPCR structure offsets (approximate, based on Xbox 360)
        // 0x00: Self pointer
        memory_->write_u32(kpcr + 0x00, kpcr);
        
        // 0x04: PRCB (Processor Control Block) pointer
        memory_->write_u32(kpcr + 0x04, kpcr + 0x100);
        
        // 0x08: Interrupt stack
        memory_->write_u32(kpcr + 0x08, kpcr + 0x2000);
        
        // 0x10: Current thread (starts as idle thread)
        memory_->write_u32(kpcr + 0x10, idle_thread_);
        
        // 0x14: Processor number
        memory_->write_u32(kpcr + 0x14, i);
        
        // PRCB at offset 0x100
        GuestAddr prcb = kpcr + 0x100;
        
        // 0x100 + 0x00: Current thread
        memory_->write_u32(prcb + 0x00, idle_thread_);
        
        // 0x100 + 0x04: Next thread
        memory_->write_u32(prcb + 0x04, 0);
        
        // 0x100 + 0x08: Idle thread
        memory_->write_u32(prcb + 0x08, idle_thread_);
        
        // 0x100 + 0x0C: Processor number
        memory_->write_u8(prcb + 0x0C, i);
        
        // 0x100 + 0x80: DPC data
        // Initialize DPC queue as empty
        
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
    
    // Set system initialization flags in a safe location
    // Use the first KPCR + 0x100 offset for flags
    GuestAddr flags_addr = kpcr_base_ + 0x100;
    memory_->write_u32(flags_addr + 0, 1);  // System ready flag
    memory_->write_u32(flags_addr + 4, 1);  // Video ready flag  
    memory_->write_u32(flags_addr + 8, 1);  // XAM ready flag
    
    LOGD("System events created, flags at 0x%08X", flags_addr);
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
    
    // For now, do a simple poll/yield approach
    // TODO: Implement proper blocking wait via XThread
    auto current = get_current_thread();
    if (current) {
        // Use the thread's wait mechanism
        auto event = get_or_create_event(object);
        if (event) {
            return current->wait(event.get(), timeout_100ns);
        }
    }
    
    // Fallback - brief yield then timeout
    std::this_thread::yield();
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
    // Process pending DPCs first
    process_dpcs();
    
    // Run scheduled threads
    XScheduler::instance().run_for(cycles);
    
    // Process timers
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

GuestAddr XKernel::get_kpcr_address(u32 processor) const {
    if (processor >= 6) return 0;
    return kpcr_base_ + (processor * 0x3000);
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
    
    GuestAddr routine = memory->read_u32(dpc + 8);
    GuestAddr context = memory->read_u32(dpc + 12);
    
    if (routine) {
        KernelState::instance().queue_dpc(routine, context);
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
    // TODO: Implement timer queue
    (void)timer;
    (void)due_time;
    (void)dpc;
    return true;
}

u32 ke_get_current_processor_number() {
    auto* thread = get_current_thread();
    return thread ? thread->cpu_thread_id() : 0;
}

} // namespace xkernel

} // namespace x360mu
