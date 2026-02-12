/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xbox 360 Kernel HLE implementation
 */

#include "kernel.h"
#include "xobject.h"
#include "work_queue.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"
#include "cpu/xenon/threading.h"
#include "xex_loader.h"
#include "filesystem/vfs.h"
#include "input/input_manager.h"
#include <unordered_set>
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-kernel"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[KERNEL] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[KERNEL ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

//=============================================================================
// Kernel Implementation
//=============================================================================

Kernel::Kernel() {
    for (auto& state : input_state_) {
        state = {};
    }
}

Kernel::~Kernel() {
    shutdown();
}

Status Kernel::initialize(Memory* memory, Cpu* cpu, VirtualFileSystem* vfs) {
    memory_ = memory;
    cpu_ = cpu;
    vfs_ = vfs;
    
    // Initialize HLE state
    init_hle_state(vfs);
    
    // Initialize file I/O state
    init_file_io_state(vfs);
    
    // Register all HLE functions
    register_hle_functions();
    
    LOGI("Kernel initialized with HLE functions");
    return Status::Ok;
}

void Kernel::shutdown() {
    // Stop system worker thread
    stop_system_worker();
    
    // Shutdown file I/O state first (closes open handles)
    shutdown_file_io_state();
    
    modules_.clear();
    objects_.clear();
    threads_.clear();
    hle_functions_.clear();
}

void Kernel::stop_system_worker() {
    // System worker thread removed - DPCs are now processed properly via XKernel::run_for()
    // in the main emulation loop, and on event signals
    if (system_worker_running_) {
        system_worker_running_ = false;
        if (system_worker_thread_.joinable()) {
            system_worker_thread_.join();
        }
    }
}

void Kernel::reset() {
    unload();
    for (auto& state : input_state_) {
        state = {};
    }
}

Status Kernel::load_xex(const std::string& path) {
    XexLoader loader;
    Status status;
    
    // Check if this is a VFS path (e.g., \Device\Cdrom0\default.xex)
    bool is_vfs_path = (path.find("\\Device\\") == 0 || path.find("device\\") == 0);
    
    if (is_vfs_path && vfs_) {
        // Read via VFS
        LOGI("Loading XEX via VFS: %s", path.c_str());
        
        u32 handle;
        status = vfs_->open_file(path, FileAccess::Read, handle);
        if (status != Status::Ok) {
            LOGE("Failed to open file: %s", path.c_str());
            return status;
        }
        
        // Get file size
        u64 file_size;
        status = vfs_->get_file_size(handle, file_size);
        if (status != Status::Ok) {
            vfs_->close_file(handle);
            LOGE("Failed to get file size: %s", path.c_str());
            return status;
        }
        
        // Read file data
        std::vector<u8> data(file_size);
        u64 bytes_read;
        status = vfs_->read_file(handle, data.data(), file_size, bytes_read);
        vfs_->close_file(handle);
        
        if (status != Status::Ok || bytes_read != file_size) {
            LOGE("Failed to read file: %s (read %llu of %llu)", 
                 path.c_str(), (unsigned long long)bytes_read, (unsigned long long)file_size);
            return Status::Error;
        }
        
        // Extract filename for module name
        std::string name = path;
        auto pos = name.find_last_of("/\\");
        if (pos != std::string::npos) {
            name = name.substr(pos + 1);
        }
        
        // Load from buffer
        status = loader.load_buffer(data.data(), static_cast<u32>(file_size), name, memory_);
    } else {
        // Load directly from filesystem
        status = loader.load_file(path, memory_);
    }
    
    if (status != Status::Ok) {
        LOGE("Failed to load XEX: %s", path.c_str());
        return status;
    }
    
    const XexModule* xex_module = loader.get_module();
    if (!xex_module) {
        LOGE("No module after load");
        return Status::Error;
    }
    
    LoadedModule module;
    module.name = xex_module->name;
    module.path = path;
    module.base_address = xex_module->base_address;
    module.size = xex_module->image_size;
    module.entry_point = xex_module->entry_point;
    module.is_exe = xex_module->is_title;
    
    modules_.push_back(std::move(module));
    
    LOGI("Loaded XEX: %s at 0x%08X, entry: 0x%08X", 
         modules_.back().name.c_str(),
         modules_.back().base_address,
         modules_.back().entry_point);
    
    // Configure TLS template from XEX info
    // This ensures each thread gets a copy of the game's TLS initial data
    if (xex_module->tls_info.raw_data_address != 0 && xex_module->tls_info.data_size > 0) {
        SetTlsTemplateInfo(
            xex_module->tls_info.raw_data_address,
            xex_module->tls_info.data_size,
            xex_module->tls_info.slot_count
        );
    }
    
    // Install import thunks for syscall handling
    install_import_thunks(*xex_module);

    // Extract game info and analyze import compatibility
    game_info_ = std::make_unique<GameInfo>(
        extract_game_info(*xex_module, hle_functions_,
                          [this](u32 mod, u32 ord) { return make_import_key(mod, ord); }));

    return Status::Ok;
}

void Kernel::unload() {
    modules_.clear();
    // Don't clear HLE functions - they're static
}

void Kernel::prepare_entry() {
    if (modules_.empty()) {
        LOGE("No module loaded");
        return;
    }
    
    const auto& main_module = modules_[0];
    
    // Start main thread at entry point
    // Stack at 0x9FFF0000 (virtual) = 0x1FFF0000 (physical) - near top of 512MB
    cpu_->start_thread(0, main_module.entry_point, 0x9FFF0000);
    
    // Also create a guest thread in the scheduler for thread tracking
    // This allows proper blocking wait behavior
    if (scheduler_) {
        GuestThread* main_thread = scheduler_->create_thread(
            main_module.entry_point,
            0,           // param
            256 * 1024,  // 256KB stack
            0            // flags
        );
        if (main_thread) {
            LOGI("Created main guest thread %u in scheduler", main_thread->thread_id);
        }
        
        // Create system worker guest threads
        // Xbox 360 kernel creates several system threads that handle DPCs, work items, etc.
        // Without these, games get stuck waiting for worker thread responses
        create_system_guest_threads();
    }
    
    // Start system worker thread that monitors events and responds
    // This simulates Xbox 360's system threads that handle initialization synchronization
    start_system_worker();
    
    LOGI("Prepared entry at 0x%08X", main_module.entry_point);
}

void Kernel::create_system_guest_threads() {
    // Create system worker guest threads that process work queues
    // These simulate the Xbox 360 kernel's system threads (ExpWorkerThread)
    //
    // Worker threads process WORK_QUEUE_ITEMs queued via ExQueueWorkItem.
    // Each worker services a specific queue type (Critical, Delayed, HyperCritical).
    
    if (!scheduler_) return;
    
    // Create 3 worker threads - one for each queue type
    // Xbox 360 typically has 3-6 system threads handling different work queues
    for (int i = 0; i < 3; i++) {
        GuestThread* worker = scheduler_->create_thread(
            0,           // Entry point 0 - special marker for worker thread
            i,           // param (queue type)
            64 * 1024,   // 64KB stack
            0            // not suspended
        );
        if (worker) {
            // Mark as system thread AND worker thread
            worker->is_system_thread = true;
            worker->is_worker_thread = true;
            
            // Assign queue type based on worker index
            worker->worker_queue_type = static_cast<WorkQueueType>(i % 3);
            
            // Start in READY state - worker threads actively process work queues
            // The scheduler will call process_worker_thread() when this thread runs
            worker->state = ThreadState::Ready;
            
            LOGI("Created system worker thread %u for queue type %u - starting in Ready state", 
                 worker->thread_id, static_cast<u32>(worker->worker_queue_type));
        }
    }
}

void Kernel::start_system_worker() {
    // System worker thread removed - DPCs are now processed properly via XKernel::run_for()
    // in the main emulation loop, and on event signals via KernelState::process_dpcs()
    //
    // The old worker was a hack that pre-signaled event addresses. Now that DPCs
    // are actually executed, the DPC routines themselves signal the correct events.
    LOGI("System worker thread disabled - DPCs now processed via XKernel::run_for()");
}

void Kernel::input_button(u32 player, u32 button, bool pressed) {
    if (player >= 4) return;

    // Use InputManager which maps Android button indices to XInput flags
    get_input_manager().set_button(player, button, pressed);
    get_input_manager().sync_to_xam();

    // Keep local state in sync
    u16 xinput_flag = android_button_to_xinput(button);
    if (pressed) {
        input_state_[player].buttons |= xinput_flag;
    } else {
        input_state_[player].buttons &= ~xinput_flag;
    }
}

void Kernel::input_trigger(u32 player, u32 trigger, f32 value) {
    if (player >= 4) return;

    get_input_manager().set_trigger(player, trigger, value);
    get_input_manager().sync_to_xam();

    if (trigger == 0) {
        input_state_[player].left_trigger = value;
    } else {
        input_state_[player].right_trigger = value;
    }
}

void Kernel::input_stick(u32 player, u32 stick, f32 x, f32 y) {
    if (player >= 4) return;

    get_input_manager().set_stick(player, stick, x, y);
    get_input_manager().sync_to_xam();

    if (stick == 0) {
        input_state_[player].left_stick_x = x;
        input_state_[player].left_stick_y = y;
    } else {
        input_state_[player].right_stick_x = x;
        input_state_[player].right_stick_y = y;
    }
}

void Kernel::handle_syscall(u32 ordinal, u32 module_ordinal) {
    // === 1:1 THREADING MODEL: Use Thread-Local Storage ===
    // Get the current thread's context via TLS, not a global "context 0"
    GuestThread* current_thread = GetCurrentGuestThread();
    if (!current_thread) {
        // Fallback to context 0 if no TLS (shouldn't happen with 1:1 threading)
        LOGE("handle_syscall called with no current guest thread! Using context 0 as fallback.");
        auto& ctx = cpu_->get_context(0);
        ctx.gpr[3] = 0;  // STATUS_SUCCESS
        return;
    }
    
    ThreadContext& ctx = current_thread->context;
    
    // Log unique syscalls for debugging
    static std::unordered_set<u64> logged_syscalls;
    u64 key = make_import_key(module_ordinal, ordinal);
    
    if (logged_syscalls.find(key) == logged_syscalls.end() && logged_syscalls.size() < 100) {
        LOGI("First call to syscall: module=%u, ordinal=%u (PC=0x%08llX, thread=%u)", 
             module_ordinal, ordinal, ctx.pc, current_thread->thread_id);
        logged_syscalls.insert(key);
    }
    
    auto it = hle_functions_.find(key);
    if (it == hle_functions_.end()) {
        static std::unordered_set<u64> logged_unimpl;
        if (logged_unimpl.find(key) == logged_unimpl.end()) {
            LOGE("UNIMPLEMENTED syscall: module=%u, ordinal=%u at PC=0x%08llX, LR=0x%08llX", 
                 module_ordinal, ordinal, ctx.pc, ctx.lr);
            logged_unimpl.insert(key);
        }
        // FIX: Set return value to STATUS_SUCCESS so game doesn't retry in infinite loop
        ctx.gpr[3] = 0;  // STATUS_SUCCESS
        return;
    }
    
    u64 args[8] = {
        ctx.gpr[3], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6],
        ctx.gpr[7], ctx.gpr[8], ctx.gpr[9], ctx.gpr[10]
    };
    
    u64 result = 0;
    it->second(cpu_, memory_, args, &result);
    ctx.gpr[3] = result;
}

const LoadedModule* Kernel::get_module(const std::string& name) const {
    for (const auto& module : modules_) {
        if (module.name == name) {
            return &module;
        }
    }
    return nullptr;
}

u32 Kernel::create_handle(ObjectType type, void* object, const std::string& name) {
    KernelObject obj;
    obj.type = type;
    obj.handle = next_handle_++;
    obj.native_object = object;
    obj.name = name;
    
    objects_[obj.handle] = obj;
    return obj.handle;
}

void* Kernel::get_object(u32 handle, ObjectType expected_type) {
    auto it = objects_.find(handle);
    if (it == objects_.end() || it->second.type != expected_type) {
        return nullptr;
    }
    return it->second.native_object;
}

void Kernel::close_handle(u32 handle) {
    // Try local objects first
    auto it = objects_.find(handle);
    if (it != objects_.end()) {
        objects_.erase(it);
        return;
    }

    // Fall through to unified ObjectTable
    KernelState::instance().object_table().close_handle(handle);
}

void Kernel::set_scheduler(ThreadScheduler* scheduler) {
    scheduler_ = scheduler;
    // Also set the global scheduler pointer for HLE functions
    set_thread_scheduler(scheduler);
    LOGI("Kernel scheduler set");
}

u32 Kernel::create_thread(GuestAddr entry, GuestAddr stack, u64 stack_size, u32 priority) {
    ThreadInfo info;
    info.handle = next_handle_++;
    info.thread_id = info.handle & 0xFFFF;
    info.entry_point = entry;
    info.stack_base = stack;
    info.stack_size = stack_size;
    info.priority = priority;
    info.suspended = false;
    info.terminated = false;

    threads_[info.handle] = info;

    // Apply priority to the scheduler if available
    if (scheduler_) {
        GuestThread* guest = scheduler_->get_thread_by_handle(info.handle);
        if (guest) {
            // Map Xbox 360 priority (0-31, lower=higher) to our ThreadPriority enum
            if (priority <= 8) {
                guest->priority = ThreadPriority::Highest;
            } else if (priority <= 15) {
                guest->priority = ThreadPriority::AboveNormal;
            } else if (priority == 16) {
                guest->priority = ThreadPriority::Normal;
            } else if (priority <= 23) {
                guest->priority = ThreadPriority::BelowNormal;
            } else {
                guest->priority = ThreadPriority::Lowest;
            }
        }
    }

    return info.handle;
}

void Kernel::terminate_thread(u32 handle) {
    auto it = threads_.find(handle);
    if (it != threads_.end()) {
        it->second.terminated = true;
    }
}

void Kernel::suspend_thread(u32 handle) {
    auto it = threads_.find(handle);
    if (it != threads_.end()) {
        it->second.suspended = true;
    }
}

void Kernel::resume_thread(u32 handle) {
    auto it = threads_.find(handle);
    if (it != threads_.end()) {
        it->second.suspended = false;
    }
}

void Kernel::register_hle_functions() {
    register_xboxkrnl();
    register_xboxkrnl_extended();
    register_xam();

    // Register file I/O HLE functions with proper ordinals
    register_file_io_exports(hle_functions_, [this](u32 module, u32 ordinal) {
        return make_import_key(module, ordinal);
    });

    // Register threading HLE functions LAST so improved implementations
    // (DPC, Timer, IoCompletion, CV-based waits) override older duplicates
    register_xboxkrnl_threading(this);
}

// register_xboxkrnl, register_xboxkrnl_extended, and register_xam
// are implemented in their respective .cpp files

Status Kernel::parse_xex_header(const std::vector<u8>& /*data*/, LoadedModule& /*module*/) {
    return Status::NotImplemented;
}

Status Kernel::load_xex_image(const std::vector<u8>& /*data*/, LoadedModule& /*module*/) {
    return Status::NotImplemented;
}

Status Kernel::resolve_imports(LoadedModule& /*module*/) {
    return Status::NotImplemented;
}

void Kernel::install_import_thunks(const XexModule& module) {
    LOGI("Installing import thunks for %zu libraries", module.imports.size());
    
    // Debug: Check what's at a specific problematic address BEFORE patching
    u32 debug_addr = 0x82612520;
    u32 before1 = memory_->read_u32(debug_addr);
    u32 before2 = memory_->read_u32(debug_addr + 4);
    LOGI("Before patching: [0x%08X]=0x%08X, [0x%08X]=0x%08X", 
         debug_addr, before1, debug_addr + 4, before2);
    
    // Thunk area base - allocate space above the loaded image
    // We'll place generated thunks here if the XEX doesn't specify thunk addresses
    GuestAddr thunk_area_base = module.base_address + module.image_size;
    thunk_area_base = (thunk_area_base + 0xFFF) & ~0xFFFu;  // Align to 4KB
    GuestAddr thunk_ptr = thunk_area_base;
    
    u32 total_thunks = 0;
    
    for (const auto& lib : module.imports) {
        // Determine module ID based on library name
        u32 module_id = 0;
        if (lib.name == "xboxkrnl.exe" || lib.name.find("xboxkrnl") != std::string::npos) {
            module_id = 0;
        } else if (lib.name == "xam.xex" || lib.name.find("xam") != std::string::npos) {
            module_id = 1;
        } else {
            module_id = 2; // Unknown/other
        }
        
        LOGI("  Library: %s (module_id=%u, %zu imports)", 
             lib.name.c_str(), module_id, lib.imports.size());

        for (size_t i = 0; i < lib.imports.size(); i++) {
            u32 thunk_addr = lib.imports[i].thunk_address;
            u32 ordinal = lib.imports[i].ordinal;
            
            // If no thunk address specified, allocate one
            if (thunk_addr == 0) {
                thunk_addr = thunk_ptr;
                thunk_ptr += 16;  // Each thunk needs up to 16 bytes
            }

            // Encode: (module_id << 16) | ordinal
            u32 encoded = (module_id << 16) | (ordinal & 0xFFFF);

            // Write thunk code:
            // For values <= 0x7FFF: li r0, encoded (single instruction)
            // For larger values: lis r0, high16; ori r0, r0, low16
            u32 write_addr = thunk_addr;
            
            if (encoded <= 0x7FFF) {
                // li r0, encoded (addi r0, 0, encoded)
                // PowerPC: addi rD, rA, SIMM -> 001110 DDDDD AAAAA SSSSSSSSSSSSSSSS
                // For li r0, imm: opcode=14, rD=0, rA=0, SIMM=encoded
                u32 li_inst = 0x38000000 | (encoded & 0xFFFF);
                memory_->write_u32(write_addr, li_inst);
                write_addr += 4;
            } else {
                // lis r0, high16 (addis r0, 0, high16)
                // PowerPC: addis rD, rA, SIMM -> 001111 DDDDD AAAAA SSSSSSSSSSSSSSSS
                u32 lis_inst = 0x3C000000 | ((encoded >> 16) & 0xFFFF);
                memory_->write_u32(write_addr, lis_inst);
                write_addr += 4;
                
                // ori r0, r0, low16
                // PowerPC: ori rA, rS, UIMM -> 011000 SSSSS AAAAA UUUUUUUUUUUUUUUU
                u32 ori_inst = 0x60000000 | (encoded & 0xFFFF);
                memory_->write_u32(write_addr, ori_inst);
                write_addr += 4;
            }

            // Write: sc (syscall instruction)
            // PowerPC sc: 010001 00000 00000 0000000000000010 -> 0x44000002
            memory_->write_u32(write_addr, 0x44000002);
            GuestAddr sc_addr = write_addr;  // Remember where sc was written
            write_addr += 4;

            // Write: blr (branch to link register - return)
            // PowerPC blr: 010011 00000 00000 00000 0000010000 0 -> 0x4E800020
            memory_->write_u32(write_addr, 0x4E800020);
            
            // Log first few thunks and any near 0x82612520 for debugging
            if (total_thunks < 5 || (thunk_addr >= 0x82612500 && thunk_addr <= 0x82612540)) {
                LOGI("    Thunk %u: addr=0x%08X, sc_at=0x%08X, ordinal=%u, encoded=0x%08X",
                     total_thunks, thunk_addr, sc_addr, ordinal, encoded);
            }
            
            total_thunks++;
        }
    }
    
    LOGI("Installed %u import thunks (thunk area: 0x%08X - 0x%08X)", 
         total_thunks, thunk_area_base, thunk_ptr);
    
    // Debug: Check what's at the problematic address AFTER patching
    u32 after1 = memory_->read_u32(0x82612520);
    u32 after2 = memory_->read_u32(0x82612524);
    LOGI("After patching: [0x82612520]=0x%08X, [0x82612524]=0x%08X", 
         after1, after2);
}

//=============================================================================
// VirtualFileSystem - implemented in filesystem/vfs.cpp
//=============================================================================

} // namespace x360mu

