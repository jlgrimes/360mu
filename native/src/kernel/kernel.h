/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Xbox 360 kernel HLE (High-Level Emulation)
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <array>

namespace x360mu {

class Memory;
class Cpu;
class VirtualFileSystem;

/**
 * XEX2 file header structure
 */
struct Xex2Header {
    be_u32 magic;              // 'XEX2'
    be_u32 module_flags;
    be_u32 exe_offset;
    be_u32 reserved;
    be_u32 security_offset;
    be_u32 header_count;
};

/**
 * XEX optional header types
 */
enum class XexHeaderType : u32 {
    ResourceInfo = 0x000002FF,
    BaseFileFormat = 0x000003FF,
    BaseReference = 0x00000405,
    DeltaPatchDescriptor = 0x000005FF,
    KeyVaultPrivs = 0x000040FF,
    TimeRange = 0x000041FF,
    ConsoleIdTable = 0x000042FF,
    BoundingPath = 0x000080FF,
    DeviceId = 0x00008105,
    OriginalBaseAddress = 0x00010001,
    EntryPoint = 0x00010100,
    ImageBaseAddress = 0x00010201,
    ImportLibraries = 0x000103FF,
    ChecksumTimestamp = 0x00018002,
    EnabledForCallcap = 0x00018102,
    EnabledForFastcap = 0x00018200,
    OriginalPEName = 0x000183FF,
    StaticLibraries = 0x000200FF,
    TLSInfo = 0x00020104,
    DefaultStackSize = 0x00020200,
    DefaultFilesystemCacheSize = 0x00020301,
    DefaultHeapSize = 0x00020401,
    PageHeapSizeAndFlags = 0x00028002,
    SystemFlags = 0x00030000,
    ExecutionId = 0x00040006,
    ServiceIdList = 0x000401FF,
    TitleWorkspaceSize = 0x00040201,
    GameRatings = 0x00040310,
    LANKey = 0x00040404,
    Xbox360Logo = 0x000405FF,
    MultidiscMediaIds = 0x000406FF,
    AlternateTitleIds = 0x000407FF,
    AdditionalTitleMemory = 0x00040801,
    ExportsToExportsByName = 0x00E10402,
};

// XexImportLibrary is defined in xex_loader.h

/**
 * Loaded module info
 */
struct LoadedModule {
    std::string name;
    std::string path;
    GuestAddr base_address;
    u64 size;
    GuestAddr entry_point;
    bool is_exe;
};

/**
 * Kernel object types
 */
enum class ObjectType : u32 {
    None = 0,
    Thread,
    Event,
    Semaphore,
    Mutant,
    Timer,
    File,
    Directory,
    Section,
    Device,
};

/**
 * Kernel object handle
 */
struct KernelObject {
    ObjectType type;
    u32 handle;
    void* native_object;  // Platform-specific object
    std::string name;
};

/**
 * Thread state
 */
struct ThreadInfo {
    u32 handle;
    u32 thread_id;
    GuestAddr entry_point;
    GuestAddr stack_base;
    u64 stack_size;
    u32 priority;
    bool suspended;
    bool terminated;
};

/**
 * HLE function signature
 */
using HleFunction = std::function<void(Cpu*, Memory*, u64* args, u64* result)>;

/**
 * Kernel HLE implementation
 */
class Kernel {
public:
    Kernel();
    ~Kernel();
    
    /**
     * Initialize kernel subsystem
     */
    Status initialize(Memory* memory, Cpu* cpu, VirtualFileSystem* vfs);
    
    /**
     * Shutdown and release resources
     */
    void shutdown();
    
    /**
     * Reset kernel state
     */
    void reset();
    
    /**
     * Load XEX executable
     */
    Status load_xex(const std::string& path);
    
    /**
     * Unload current executable
     */
    void unload();
    
    /**
     * Prepare entry point for execution
     */
    void prepare_entry();
    
    // Input handling (forwarded to game)
    void input_button(u32 player, u32 button, bool pressed);
    void input_trigger(u32 player, u32 trigger, f32 value);
    void input_stick(u32 player, u32 stick, f32 x, f32 y);
    
    /**
     * Handle syscall from CPU
     */
    void handle_syscall(u32 ordinal, u32 module_ordinal);
    
    /**
     * Get loaded module info
     */
    const LoadedModule* get_module(const std::string& name) const;
    
    // Object management
    u32 create_handle(ObjectType type, void* object, const std::string& name = "");
    void* get_object(u32 handle, ObjectType expected_type);
    void close_handle(u32 handle);
    
    // Thread management
    u32 create_thread(GuestAddr entry, GuestAddr stack, u64 stack_size, u32 priority);
    void terminate_thread(u32 handle);
    void suspend_thread(u32 handle);
    void resume_thread(u32 handle);
    
private:
    Memory* memory_ = nullptr;
    Cpu* cpu_ = nullptr;
    VirtualFileSystem* vfs_ = nullptr;
    
    // Loaded modules
    std::vector<LoadedModule> modules_;
    
    // Kernel objects
    std::unordered_map<u32, KernelObject> objects_;
    u32 next_handle_ = 0x10000;
    
    // Threads
    std::unordered_map<u32, ThreadInfo> threads_;
    
    // Input state
    struct InputState {
        u32 buttons;
        f32 left_trigger;
        f32 right_trigger;
        f32 left_stick_x, left_stick_y;
        f32 right_stick_x, right_stick_y;
    };
    std::array<InputState, 4> input_state_;
    
    // HLE function table
    std::unordered_map<u64, HleFunction> hle_functions_;
    
    // XEX loading
    Status parse_xex_header(const std::vector<u8>& data, LoadedModule& module);
    Status load_xex_image(const std::vector<u8>& data, LoadedModule& module);
    Status resolve_imports(LoadedModule& module);
    
    // HLE registration
    void register_hle_functions();
    void register_xboxkrnl();
    void register_xboxkrnl_extended();
    void register_xam();
    
    // Import lookup key
    u64 make_import_key(u32 module, u32 ordinal) {
        return (static_cast<u64>(module) << 32) | ordinal;
    }
};

/**
 * Virtual file system for Xbox 360 paths
 */
class VirtualFileSystem {
public:
    VirtualFileSystem();
    ~VirtualFileSystem();
    
    Status initialize(const std::string& data_path, const std::string& save_path);
    void shutdown();
    
    // Mount/unmount
    Status mount_iso(const std::string& device, const std::string& iso_path);
    Status mount_folder(const std::string& device, const std::string& host_path);
    void unmount(const std::string& device);
    void unmount_all();
    
    // Path translation
    std::string translate_path(const std::string& xbox_path);
    
    // File operations
    Status open_file(const std::string& path, u32 access, u32& handle_out);
    Status close_file(u32 handle);
    Status read_file(u32 handle, void* buffer, u64 size, u64& bytes_read);
    Status write_file(u32 handle, const void* buffer, u64 size, u64& bytes_written);
    Status seek_file(u32 handle, s64 offset, u32 origin, u64& new_position);
    Status get_file_size(u32 handle, u64& size_out);
    
    // Directory operations
    Status create_directory(const std::string& path);
    Status query_directory(const std::string& path, std::vector<std::string>& entries);
    
private:
    struct Mount {
        std::string device;
        std::string host_path;
        bool is_iso;
    };
    
    struct OpenFile {
        u32 handle;
        std::string path;
        void* native_handle;
        u32 access;
    };
    
    std::vector<Mount> mounts_;
    std::unordered_map<u32, OpenFile> open_files_;
    u32 next_file_handle_ = 1;
    
    std::string data_path_;
    std::string save_path_;
};

} // namespace x360mu

