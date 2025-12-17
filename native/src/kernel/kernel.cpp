/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xbox 360 Kernel HLE implementation
 */

#include "kernel.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"
#include "xex_loader.h"

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
    
    // Register all HLE functions
    register_hle_functions();
    
    LOGI("Kernel initialized with HLE functions");
    return Status::Ok;
}

void Kernel::shutdown() {
    modules_.clear();
    objects_.clear();
    threads_.clear();
    hle_functions_.clear();
}

void Kernel::reset() {
    unload();
    for (auto& state : input_state_) {
        state = {};
    }
}

Status Kernel::load_xex(const std::string& path) {
    XexLoader loader;
    
    Status status = loader.load_file(path, memory_);
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
    cpu_->start_thread(0, main_module.entry_point, 
                       memory::MAIN_MEMORY_BASE + 512 * MB - 0x10000);
    
    LOGI("Prepared entry at 0x%08X", main_module.entry_point);
}

void Kernel::input_button(u32 player, u32 button, bool pressed) {
    if (player >= 4) return;
    
    if (pressed) {
        input_state_[player].buttons |= button;
    } else {
        input_state_[player].buttons &= ~button;
    }
}

void Kernel::input_trigger(u32 player, u32 trigger, f32 value) {
    if (player >= 4) return;
    
    if (trigger == 0) {
        input_state_[player].left_trigger = value;
    } else {
        input_state_[player].right_trigger = value;
    }
}

void Kernel::input_stick(u32 player, u32 stick, f32 x, f32 y) {
    if (player >= 4) return;
    
    if (stick == 0) {
        input_state_[player].left_stick_x = x;
        input_state_[player].left_stick_y = y;
    } else {
        input_state_[player].right_stick_x = x;
        input_state_[player].right_stick_y = y;
    }
}

void Kernel::handle_syscall(u32 ordinal, u32 module_ordinal) {
    u64 key = make_import_key(module_ordinal, ordinal);
    
    auto it = hle_functions_.find(key);
    if (it == hle_functions_.end()) {
        LOGE("Unimplemented syscall: module=%u, ordinal=%u", module_ordinal, ordinal);
        return;
    }
    
    auto& ctx = cpu_->get_context(0);
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
    objects_.erase(handle);
}

u32 Kernel::create_thread(GuestAddr entry, GuestAddr stack, u64 stack_size, u32 priority) {
    (void)priority; // TODO: use priority
    
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

//=============================================================================
// VirtualFileSystem Implementation
//=============================================================================

VirtualFileSystem::VirtualFileSystem() = default;

VirtualFileSystem::~VirtualFileSystem() {
    shutdown();
}

Status VirtualFileSystem::initialize(const std::string& data_path, const std::string& save_path) {
    data_path_ = data_path;
    save_path_ = save_path;
    return Status::Ok;
}

void VirtualFileSystem::shutdown() {
    unmount_all();
}

Status VirtualFileSystem::mount_iso(const std::string& device, const std::string& iso_path) {
    Mount mount;
    mount.device = device;
    mount.host_path = iso_path;
    mount.is_iso = true;
    
    mounts_.push_back(std::move(mount));
    LOGI("Mounted ISO %s as %s", iso_path.c_str(), device.c_str());
    return Status::Ok;
}

Status VirtualFileSystem::mount_folder(const std::string& device, const std::string& host_path) {
    Mount mount;
    mount.device = device;
    mount.host_path = host_path;
    mount.is_iso = false;
    
    mounts_.push_back(std::move(mount));
    LOGI("Mounted folder %s as %s", host_path.c_str(), device.c_str());
    return Status::Ok;
}

void VirtualFileSystem::unmount(const std::string& device) {
    mounts_.erase(
        std::remove_if(mounts_.begin(), mounts_.end(),
                       [&](const Mount& m) { return m.device == device; }),
        mounts_.end());
}

void VirtualFileSystem::unmount_all() {
    mounts_.clear();
    
    // Close all open files
    for (auto& [handle, file] : open_files_) {
        if (file.native_handle) {
            fclose(static_cast<FILE*>(file.native_handle));
        }
    }
    open_files_.clear();
}

std::string VirtualFileSystem::translate_path(const std::string& xbox_path) {
    // Simple path translation - find the mount point and replace it
    for (const auto& mount : mounts_) {
        if (xbox_path.find(mount.device) == 0) {
            return mount.host_path + xbox_path.substr(mount.device.length());
        }
    }
    
    // Default to data path
    return data_path_ + "/" + xbox_path;
}

Status VirtualFileSystem::open_file(const std::string& path, u32 access, u32& handle_out) {
    std::string host_path = translate_path(path);
    
    const char* mode = (access & 2) ? "r+b" : "rb";
    FILE* fp = fopen(host_path.c_str(), mode);
    if (!fp) {
        return Status::NotFound;
    }
    
    OpenFile file;
    file.handle = next_file_handle_++;
    file.path = path;
    file.native_handle = fp;
    file.access = access;
    
    open_files_[file.handle] = file;
    handle_out = file.handle;
    
    return Status::Ok;
}

Status VirtualFileSystem::close_file(u32 handle) {
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::NotFound;
    }
    
    if (it->second.native_handle) {
        fclose(static_cast<FILE*>(it->second.native_handle));
    }
    
    open_files_.erase(it);
    return Status::Ok;
}

Status VirtualFileSystem::read_file(u32 handle, void* buffer, u64 size, u64& bytes_read) {
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::NotFound;
    }
    
    FILE* fp = static_cast<FILE*>(it->second.native_handle);
    bytes_read = fread(buffer, 1, size, fp);
    
    return Status::Ok;
}

Status VirtualFileSystem::write_file(u32 handle, const void* buffer, u64 size, u64& bytes_written) {
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::NotFound;
    }
    
    FILE* fp = static_cast<FILE*>(it->second.native_handle);
    bytes_written = fwrite(buffer, 1, size, fp);
    
    return Status::Ok;
}

Status VirtualFileSystem::seek_file(u32 handle, s64 offset, u32 origin, u64& new_position) {
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::NotFound;
    }
    
    FILE* fp = static_cast<FILE*>(it->second.native_handle);
    int whence = (origin == 0) ? SEEK_SET : (origin == 1) ? SEEK_CUR : SEEK_END;
    
    if (fseek(fp, offset, whence) != 0) {
        return Status::IoError;
    }
    
    new_position = ftell(fp);
    return Status::Ok;
}

Status VirtualFileSystem::get_file_size(u32 handle, u64& size_out) {
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::NotFound;
    }
    
    FILE* fp = static_cast<FILE*>(it->second.native_handle);
    long current = ftell(fp);
    fseek(fp, 0, SEEK_END);
    size_out = ftell(fp);
    fseek(fp, current, SEEK_SET);
    
    return Status::Ok;
}

Status VirtualFileSystem::create_directory(const std::string& /*path*/) {
    return Status::NotImplemented;
}

Status VirtualFileSystem::query_directory(const std::string& /*path*/, std::vector<std::string>& /*entries*/) {
    return Status::NotImplemented;
}

} // namespace x360mu

