/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * xboxkrnl.exe HLE (High-Level Emulation) functions
 * These are the core Xbox 360 kernel functions that games call
 * 
 * This implementation provides:
 * - Memory management (NtAllocateVirtualMemory, NtFreeVirtualMemory, etc.)
 * - Threading primitives
 * - Synchronization objects (Events, Semaphores, Critical Sections)
 * - File I/O through VFS
 * - String operations
 * - Debug support
 */

#include "../kernel.h"
#include "../filesystem/vfs.h"
#include "../../cpu/xenon/cpu.h"
#include "../../memory/memory.h"
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <thread>
#include <chrono>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-hle"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[HLE] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[HLE WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[HLE ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

//=============================================================================
// NTSTATUS codes
//=============================================================================
constexpr u32 STATUS_SUCCESS = 0x00000000;
constexpr u32 STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr u32 STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr u32 STATUS_INVALID_HANDLE = 0xC0000008;
constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr u32 STATUS_NO_MEMORY = 0xC0000017;
constexpr u32 STATUS_CONFLICTING_ADDRESSES = 0xC0000018;
constexpr u32 STATUS_BUFFER_TOO_SMALL = 0xC0000023;
constexpr u32 STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr u32 STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr u32 STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr u32 STATUS_END_OF_FILE = 0xC0000011;
constexpr u32 STATUS_ACCESS_DENIED = 0xC0000022;
constexpr u32 STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
constexpr u32 STATUS_PENDING = 0x00000103;
constexpr u32 STATUS_TIMEOUT = 0x00000102;
constexpr u32 STATUS_WAIT_0 = 0x00000000;

// Memory allocation types
constexpr u32 MEM_COMMIT = 0x1000;
constexpr u32 MEM_RESERVE = 0x2000;
constexpr u32 MEM_DECOMMIT = 0x4000;
constexpr u32 MEM_RELEASE = 0x8000;
constexpr u32 MEM_RESET = 0x80000;
constexpr u32 MEM_TOP_DOWN = 0x100000;
constexpr u32 MEM_PHYSICAL = 0x400000;
constexpr u32 MEM_LARGE_PAGES = 0x20000000;
constexpr u32 MEM_PRIVATE = 0x20000;

// Memory protection flags
constexpr u32 PAGE_NOACCESS = 0x01;
constexpr u32 PAGE_READONLY = 0x02;
constexpr u32 PAGE_READWRITE = 0x04;
constexpr u32 PAGE_WRITECOPY = 0x08;
constexpr u32 PAGE_EXECUTE = 0x10;
constexpr u32 PAGE_EXECUTE_READ = 0x20;
constexpr u32 PAGE_EXECUTE_READWRITE = 0x40;
constexpr u32 PAGE_GUARD = 0x100;
constexpr u32 PAGE_NOCACHE = 0x200;

// File access rights
constexpr u32 GENERIC_READ = 0x80000000;
constexpr u32 GENERIC_WRITE = 0x40000000;
constexpr u32 GENERIC_EXECUTE = 0x20000000;
constexpr u32 GENERIC_ALL = 0x10000000;
constexpr u32 FILE_READ_DATA = 0x0001;
constexpr u32 FILE_WRITE_DATA = 0x0002;
constexpr u32 FILE_APPEND_DATA = 0x0004;

// File share modes
constexpr u32 FILE_SHARE_READ = 0x0001;
constexpr u32 FILE_SHARE_WRITE = 0x0002;
constexpr u32 FILE_SHARE_DELETE = 0x0004;

// File creation disposition
constexpr u32 FILE_SUPERSEDE = 0x00000000;
constexpr u32 FILE_OPEN = 0x00000001;
constexpr u32 FILE_CREATE = 0x00000002;
constexpr u32 FILE_OPEN_IF = 0x00000003;
constexpr u32 FILE_OVERWRITE = 0x00000004;
constexpr u32 FILE_OVERWRITE_IF = 0x00000005;

// File information classes
enum FileInformationClass : u32 {
    FileDirectoryInformation = 1,
    FileFullDirectoryInformation = 2,
    FileBothDirectoryInformation = 3,
    FileBasicInformation = 4,
    FileStandardInformation = 5,
    FileInternalInformation = 6,
    FileEaInformation = 7,
    FileAccessInformation = 8,
    FileNameInformation = 9,
    FilePositionInformation = 14,
    FileEndOfFileInformation = 20,
    FileNetworkOpenInformation = 34,
};

//=============================================================================
// Global HLE State
//=============================================================================
static struct HleState {
    // Virtual memory allocator state
    struct VirtualAllocation {
        GuestAddr base;
        u64 size;
        u32 alloc_type;
        u32 protect;
        bool committed;
    };
    std::unordered_map<GuestAddr, VirtualAllocation> virtual_allocations;
    GuestAddr next_virtual_addr = 0x10000000;
    std::mutex alloc_mutex;
    
    // File handle state
    struct FileHandle {
        std::string host_path;
        std::fstream file;
        u32 access;
        u64 position;
        bool is_directory;
    };
    std::unordered_map<u32, FileHandle> file_handles;
    u32 next_file_handle = 0x100;
    std::mutex file_mutex;
    
    // Path mapping
    std::unordered_map<std::string, std::string> path_mappings;
    
    // VFS pointer (set by kernel init)
    VirtualFileSystem* vfs = nullptr;
    
} g_hle;

//=============================================================================
// Helper Functions
//=============================================================================

// Convert Windows protection flags to emulator memory flags
static u32 protection_to_flags(u32 protect) {
    u32 flags = 0;
    if (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | 
                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) {
        flags |= MemoryRegion::Read;
    }
    if (protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) {
        flags |= MemoryRegion::Write;
    }
    if (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) {
        flags |= MemoryRegion::Execute;
    }
    return flags;
}

// Read ANSI_STRING from guest memory
static std::string read_ansi_string(Memory* memory, GuestAddr string_ptr) {
    if (string_ptr == 0) return "";
    
    u16 length = memory->read_u16(string_ptr);
    GuestAddr buffer = memory->read_u32(string_ptr + 4);
    
    if (buffer == 0 || length == 0) return "";
    
    std::string result;
    result.reserve(length);
    for (u16 i = 0; i < length; i++) {
        char c = static_cast<char>(memory->read_u8(buffer + i));
        if (c == 0) break;
        result += c;
    }
    return result;
}

// Read null-terminated string from guest memory
static std::string read_cstring(Memory* memory, GuestAddr ptr, u32 max_len = 256) {
    if (ptr == 0) return "";
    
    std::string result;
    for (u32 i = 0; i < max_len; i++) {
        char c = static_cast<char>(memory->read_u8(ptr + i));
        if (c == 0) break;
        result += c;
    }
    return result;
}

// Translate Xbox path to host path
static std::string translate_xbox_path(const std::string& xbox_path) {
    std::string path = xbox_path;
    
    // Remove leading backslashes
    while (!path.empty() && (path[0] == '\\' || path[0] == '/')) {
        path = path.substr(1);
    }
    
    // Check path mappings
    for (const auto& [xbox_prefix, host_prefix] : g_hle.path_mappings) {
        if (path.compare(0, xbox_prefix.length(), xbox_prefix) == 0) {
            std::string result = host_prefix + path.substr(xbox_prefix.length());
            // Convert backslashes to forward slashes
            std::replace(result.begin(), result.end(), '\\', '/');
            return result;
        }
    }
    
    // Handle common Xbox 360 paths
    if (path.compare(0, 5, "game:") == 0 || path.compare(0, 5, "Game:") == 0) {
        path = path.substr(5);
    } else if (path.compare(0, 4, "dvd:") == 0 || path.compare(0, 4, "DVD:") == 0) {
        path = path.substr(4);
    } else if (path.compare(0, 4, "hdd:") == 0 || path.compare(0, 4, "HDD:") == 0) {
        path = "save/" + path.substr(4);
    } else if (path.compare(0, 6, "cache:") == 0) {
        path = "cache/" + path.substr(6);
    } else if (path.find("\\Device\\Harddisk0\\") == 0) {
        path = "hdd/" + path.substr(18);
    } else if (path.find("\\Device\\") == 0) {
        // Generic device path - extract everything after device name
        size_t pos = path.find('\\', 8);
        if (pos != std::string::npos) {
            path = path.substr(pos + 1);
        }
    }
    
    // Remove leading slashes again after prefix removal
    while (!path.empty() && (path[0] == '\\' || path[0] == '/')) {
        path = path.substr(1);
    }
    
    // Convert remaining backslashes
    std::replace(path.begin(), path.end(), '\\', '/');
    
    return path;
}

// Read OBJECT_ATTRIBUTES and extract path
static std::string read_object_attributes_path(Memory* memory, GuestAddr obj_attr_ptr) {
    if (obj_attr_ptr == 0) return "";
    
    // OBJECT_ATTRIBUTES structure:
    // u32 RootDirectory
    // u32 ObjectName (ptr to ANSI_STRING)
    // u32 Attributes
    GuestAddr object_name_ptr = memory->read_u32(obj_attr_ptr + 4);
    
    return read_ansi_string(memory, object_name_ptr);
}

//=============================================================================
// Memory Management Functions
//=============================================================================

static void HLE_NtAllocateVirtualMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtAllocateVirtualMemory(
    //   HANDLE ProcessHandle,       // arg[0] - ignored, always current process
    //   PVOID *BaseAddress,         // arg[1] - in/out base address pointer
    //   ULONG_PTR ZeroBits,         // arg[2] - ignored
    //   PSIZE_T RegionSize,         // arg[3] - in/out region size pointer  
    //   ULONG AllocationType,       // arg[4]
    //   ULONG Protect               // arg[5]
    // );
    
    GuestAddr base_addr_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr region_size_ptr = static_cast<GuestAddr>(args[3]);
    u32 alloc_type = static_cast<u32>(args[4]);
    u32 protect = static_cast<u32>(args[5]);
    
    GuestAddr requested_base = memory->read_u32(base_addr_ptr);
    u32 requested_size = memory->read_u32(region_size_ptr);
    
    // Align size to page boundary (64KB on Xbox 360)
    u32 aligned_size = align_up(requested_size, static_cast<u32>(memory::MEM_PAGE_SIZE));
    if (aligned_size == 0) aligned_size = memory::MEM_PAGE_SIZE;
    
    std::lock_guard<std::mutex> lock(g_hle.alloc_mutex);
    
    GuestAddr base_addr;
    
    if (requested_base != 0) {
        // Caller requested specific address
        base_addr = align_down(requested_base, static_cast<GuestAddr>(memory::MEM_PAGE_SIZE));
        
        // Check if already allocated
        auto it = g_hle.virtual_allocations.find(base_addr);
        if (it != g_hle.virtual_allocations.end()) {
            if (alloc_type & MEM_COMMIT) {
                // Committing previously reserved memory
                it->second.committed = true;
                it->second.protect = protect;
                memory->write_u32(base_addr_ptr, base_addr);
                memory->write_u32(region_size_ptr, aligned_size);
                *result = STATUS_SUCCESS;
                LOGD("NtAllocateVirtualMemory: commit 0x%08X, size=0x%X", base_addr, aligned_size);
                return;
            } else {
                // Address conflict
                *result = STATUS_CONFLICTING_ADDRESSES;
                return;
            }
        }
    } else {
        // Find free address
        if (alloc_type & MEM_TOP_DOWN) {
            base_addr = 0x7FFF0000 - aligned_size;  // Top-down allocation
        } else {
            base_addr = g_hle.next_virtual_addr;
            g_hle.next_virtual_addr += aligned_size + memory::MEM_PAGE_SIZE;  // Leave gap
        }
    }
    
    // Ensure address is within valid range
    if (base_addr + aligned_size > memory::MAIN_MEMORY_SIZE) {
        // Try a different region
        base_addr = 0x40000000 + (g_hle.virtual_allocations.size() * memory::MEM_PAGE_SIZE);
    }
    
    // Perform allocation
    u32 flags = protection_to_flags(protect);
    Status status = memory->allocate(base_addr, aligned_size, flags);
    
    if (status == Status::Ok) {
        // Track allocation
        g_hle.virtual_allocations[base_addr] = {
            .base = base_addr,
            .size = aligned_size,
            .alloc_type = alloc_type,
            .protect = protect,
            .committed = (alloc_type & MEM_COMMIT) != 0
        };
        
        // Zero memory if committed
        if (alloc_type & MEM_COMMIT) {
            memory->zero_bytes(base_addr, aligned_size);
        }
        
        // Write back results
        memory->write_u32(base_addr_ptr, base_addr);
        memory->write_u32(region_size_ptr, aligned_size);
        
        *result = STATUS_SUCCESS;
        LOGD("NtAllocateVirtualMemory: 0x%08X, size=0x%X, type=0x%X, prot=0x%X", 
             base_addr, aligned_size, alloc_type, protect);
    } else {
        *result = STATUS_NO_MEMORY;
        LOGW("NtAllocateVirtualMemory: FAILED, size=0x%X", requested_size);
    }
}

static void HLE_NtFreeVirtualMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtFreeVirtualMemory(
    //   HANDLE ProcessHandle,
    //   PVOID *BaseAddress,
    //   PSIZE_T RegionSize,
    //   ULONG FreeType
    // );
    
    GuestAddr base_addr_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr region_size_ptr = static_cast<GuestAddr>(args[2]);
    u32 free_type = static_cast<u32>(args[3]);
    
    GuestAddr base_addr = memory->read_u32(base_addr_ptr);
    
    std::lock_guard<std::mutex> lock(g_hle.alloc_mutex);
    
    auto it = g_hle.virtual_allocations.find(base_addr);
    if (it != g_hle.virtual_allocations.end()) {
        if (free_type & MEM_RELEASE) {
            // Full release
            memory->free(base_addr);
            g_hle.virtual_allocations.erase(it);
            LOGD("NtFreeVirtualMemory: released 0x%08X", base_addr);
        } else if (free_type & MEM_DECOMMIT) {
            // Just decommit (keep reservation)
            it->second.committed = false;
            LOGD("NtFreeVirtualMemory: decommitted 0x%08X", base_addr);
        }
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_NtQueryVirtualMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtQueryVirtualMemory(
    //   HANDLE ProcessHandle,
    //   PVOID BaseAddress,
    //   MEMORY_INFORMATION_CLASS MemoryInformationClass,
    //   PVOID MemoryInformation,
    //   SIZE_T MemoryInformationLength,
    //   PSIZE_T ReturnLength
    // );
    
    GuestAddr base_addr = static_cast<GuestAddr>(args[1]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[3]);
    
    std::lock_guard<std::mutex> lock(g_hle.alloc_mutex);
    
    // Find containing allocation
    for (const auto& [addr, alloc] : g_hle.virtual_allocations) {
        if (base_addr >= addr && base_addr < addr + alloc.size) {
            // MEMORY_BASIC_INFORMATION structure
            memory->write_u32(info_ptr + 0, addr);           // BaseAddress
            memory->write_u32(info_ptr + 4, addr);           // AllocationBase
            memory->write_u32(info_ptr + 8, alloc.protect);  // AllocationProtect
            memory->write_u32(info_ptr + 12, static_cast<u32>(alloc.size)); // RegionSize
            memory->write_u32(info_ptr + 16, alloc.committed ? MEM_COMMIT : MEM_RESERVE); // State
            memory->write_u32(info_ptr + 20, alloc.protect); // Protect
            memory->write_u32(info_ptr + 24, MEM_PRIVATE);   // Type
            
            *result = STATUS_SUCCESS;
            return;
        }
    }
    
    // Not found - return free memory info
    memory->write_u32(info_ptr + 0, base_addr);
    memory->write_u32(info_ptr + 4, 0);
    memory->write_u32(info_ptr + 8, 0);
    memory->write_u32(info_ptr + 12, memory::MEM_PAGE_SIZE);
    memory->write_u32(info_ptr + 16, 0);  // MEM_FREE
    memory->write_u32(info_ptr + 20, 0);
    memory->write_u32(info_ptr + 24, 0);
    
    *result = STATUS_SUCCESS;
}

static void HLE_NtProtectVirtualMemory(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtProtectVirtualMemory(
    //   HANDLE ProcessHandle,
    //   PVOID *BaseAddress,
    //   PSIZE_T NumberOfBytesToProtect,
    //   ULONG NewAccessProtection,
    //   PULONG OldAccessProtection
    // );
    
    GuestAddr base_addr_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr size_ptr = static_cast<GuestAddr>(args[2]);
    u32 new_protect = static_cast<u32>(args[3]);
    GuestAddr old_protect_ptr = static_cast<GuestAddr>(args[4]);
    
    GuestAddr base_addr = memory->read_u32(base_addr_ptr);
    u32 size = memory->read_u32(size_ptr);
    
    std::lock_guard<std::mutex> lock(g_hle.alloc_mutex);
    
    auto it = g_hle.virtual_allocations.find(base_addr);
    if (it != g_hle.virtual_allocations.end()) {
        if (old_protect_ptr) {
            memory->write_u32(old_protect_ptr, it->second.protect);
        }
        it->second.protect = new_protect;
        
        // Update memory protection
        u32 flags = protection_to_flags(new_protect);
        memory->protect(base_addr, size, flags);
    } else {
        if (old_protect_ptr) {
            memory->write_u32(old_protect_ptr, PAGE_READWRITE);
        }
    }
    
    *result = STATUS_SUCCESS;
}

//=============================================================================
// Thread Functions
//=============================================================================

static void HLE_KeGetCurrentProcessType(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return process type (0 = system, 1 = title)
    *result = 1;  // Title process
}

static void HLE_KeSetAffinityThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Set thread affinity mask - return old affinity
    *result = 0x3F;  // All 6 hardware threads
}

static void HLE_KeQueryPerformanceCounter(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return high-resolution performance counter
    // Xbox 360 uses a 50 MHz counter
    static u64 counter = 0;
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    counter = std::chrono::duration_cast<std::chrono::microseconds>(duration).count() * 50;
    *result = counter;
}

static void HLE_KeQueryPerformanceFrequency(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Return performance counter frequency (50 MHz)
    *result = 50000000;
}

static void HLE_KeDelayExecutionThread(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Sleep the thread
    // arg[0] = processor mode (ignored)
    // arg[1] = alertable (ignored)
    // arg[2] = interval pointer (100ns units, negative = relative)
    
    GuestAddr interval_ptr = static_cast<GuestAddr>(args[2]);
    if (interval_ptr) {
        s64 interval = static_cast<s64>(memory->read_u64(interval_ptr));
        if (interval < 0) {
            // Relative time in 100ns units
            u64 microseconds = static_cast<u64>(-interval) / 10;
            if (microseconds > 0 && microseconds < 1000000) {
                std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
            }
        }
    }
    
    *result = STATUS_SUCCESS;
}

//=============================================================================
// Synchronization Functions
//=============================================================================

static void HLE_KeInitializeSemaphore(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr semaphore = static_cast<GuestAddr>(args[0]);
    s32 count = static_cast<s32>(args[1]);
    s32 limit = static_cast<s32>(args[2]);
    
    // DISPATCHER_HEADER + semaphore specific fields
    memory->write_u8(semaphore + 0, 5);  // Type = SemaphoreObject
    memory->write_u8(semaphore + 1, 0);  // Absolute
    memory->write_u8(semaphore + 2, sizeof(u32) * 6); // Size
    memory->write_u8(semaphore + 3, 0);  // Inserted
    memory->write_u32(semaphore + 4, static_cast<u32>(count));  // SignalState (current count)
    // Wait list would go at offset 8
    memory->write_u32(semaphore + 16, static_cast<u32>(limit)); // Limit
    
    *result = 0;
    LOGD("KeInitializeSemaphore: 0x%08X, count=%d, limit=%d", semaphore, count, limit);
}

static void HLE_KeInitializeEvent(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr event = static_cast<GuestAddr>(args[0]);
    u32 type = static_cast<u32>(args[1]);  // 0 = notification, 1 = synchronization
    u32 state = static_cast<u32>(args[2]); // Initial state
    
    // DISPATCHER_HEADER
    memory->write_u8(event + 0, type);  // Type (0 or 1)
    memory->write_u8(event + 1, 0);     // Absolute
    memory->write_u8(event + 2, sizeof(u32) * 4); // Size
    memory->write_u8(event + 3, 0);     // Inserted
    memory->write_u32(event + 4, state); // SignalState
    
    *result = 0;
    LOGD("KeInitializeEvent: 0x%08X, type=%u, state=%u", event, type, state);
}

static void HLE_KeSetEvent(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr event = static_cast<GuestAddr>(args[0]);
    s32 increment = static_cast<s32>(args[1]);
    u32 wait = static_cast<u32>(args[2]);
    
    u32 prev_state = memory->read_u32(event + 4);
    memory->write_u32(event + 4, 1);  // Set signaled
    
    // TODO: Wake waiting threads
    
    *result = prev_state;
    LOGD("KeSetEvent: 0x%08X, prev=%u", event, prev_state);
}

static void HLE_KeResetEvent(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr event = static_cast<GuestAddr>(args[0]);
    
    u32 prev_state = memory->read_u32(event + 4);
    memory->write_u32(event + 4, 0);  // Clear
    
    *result = prev_state;
}

static void HLE_KePulseEvent(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr event = static_cast<GuestAddr>(args[0]);
    
    u32 prev_state = memory->read_u32(event + 4);
    // Pulse: set then immediately reset
    memory->write_u32(event + 4, 0);
    
    // TODO: Wake one waiting thread
    
    *result = prev_state;
}

static void HLE_KeWaitForSingleObject(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS KeWaitForSingleObject(
    //   PVOID Object,
    //   KWAIT_REASON WaitReason,
    //   KPROCESSOR_MODE WaitMode,
    //   BOOLEAN Alertable,
    //   PLARGE_INTEGER Timeout
    // );
    
    GuestAddr object = static_cast<GuestAddr>(args[0]);
    GuestAddr timeout_ptr = static_cast<GuestAddr>(args[4]);
    
    // Read dispatcher header
    u8 object_type = memory->read_u8(object);
    u32 signal_state = memory->read_u32(object + 4);
    
    // Check if already signaled
    if (signal_state != 0) {
        // For synchronization event/semaphore, decrement
        if (object_type == 1) {  // Synchronization event
            memory->write_u32(object + 4, 0);
        } else if (object_type == 5) {  // Semaphore
            memory->write_u32(object + 4, signal_state - 1);
        }
        *result = STATUS_WAIT_0;
        return;
    }
    
    // Not signaled - check timeout
    if (timeout_ptr) {
        s64 timeout = static_cast<s64>(memory->read_u64(timeout_ptr));
        if (timeout == 0) {
            // Zero timeout - return immediately
            *result = STATUS_TIMEOUT;
            return;
        }
        // TODO: Implement actual waiting with timeout
    }
    
    // TODO: Block thread and wait
    // For now, just return success (busy wait behavior)
    *result = STATUS_SUCCESS;
}

static void HLE_KeWaitForMultipleObjects(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Simplified - just return success
    *result = STATUS_SUCCESS;
}

static void HLE_KeReleaseSemaphore(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr semaphore = static_cast<GuestAddr>(args[0]);
    s32 increment = static_cast<s32>(args[1]);
    u32 wait = static_cast<u32>(args[2]);
    
    s32 prev_count = static_cast<s32>(memory->read_u32(semaphore + 4));
    s32 limit = static_cast<s32>(memory->read_u32(semaphore + 16));
    
    s32 new_count = prev_count + increment;
    if (new_count > limit) {
        new_count = limit;
    }
    
    memory->write_u32(semaphore + 4, static_cast<u32>(new_count));
    
    // TODO: Wake waiting threads
    
    *result = prev_count;
}

//=============================================================================
// Critical Sections
//=============================================================================

// Critical section structure offsets
// RTL_CRITICAL_SECTION:
// +0: DebugInfo (ptr, ignored)
// +4: LockCount (s32)
// +8: RecursionCount (s32)
// +12: OwningThread (handle)
// +16: LockSemaphore (handle)
// +20: SpinCount (u32)

static void HLE_RtlInitializeCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs = static_cast<GuestAddr>(args[0]);
    
    memory->write_u32(cs + 0, 0);   // DebugInfo
    memory->write_u32(cs + 4, -1);  // LockCount (-1 = unlocked)
    memory->write_u32(cs + 8, 0);   // RecursionCount
    memory->write_u32(cs + 12, 0);  // OwningThread
    memory->write_u32(cs + 16, 0);  // LockSemaphore
    memory->write_u32(cs + 20, 0);  // SpinCount
    
    *result = STATUS_SUCCESS;
}

static void HLE_RtlInitializeCriticalSectionAndSpinCount(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs = static_cast<GuestAddr>(args[0]);
    u32 spin_count = static_cast<u32>(args[1]);
    
    memory->write_u32(cs + 0, 0);
    memory->write_u32(cs + 4, -1);
    memory->write_u32(cs + 8, 0);
    memory->write_u32(cs + 12, 0);
    memory->write_u32(cs + 16, 0);
    memory->write_u32(cs + 20, spin_count);
    
    *result = STATUS_SUCCESS;
}

static void HLE_RtlEnterCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs = static_cast<GuestAddr>(args[0]);
    
    s32 lock_count = static_cast<s32>(memory->read_u32(cs + 4));
    s32 recursion_count = static_cast<s32>(memory->read_u32(cs + 8));
    u32 owning_thread = memory->read_u32(cs + 12);
    
    // Get current thread ID (simplified)
    u32 current_thread = 1;  // TODO: Get actual thread ID
    
    if (lock_count == -1) {
        // Unlocked - acquire
        memory->write_u32(cs + 4, 0);
        memory->write_u32(cs + 8, 1);
        memory->write_u32(cs + 12, current_thread);
    } else if (owning_thread == current_thread) {
        // Already own it - increment recursion
        memory->write_u32(cs + 4, lock_count + 1);
        memory->write_u32(cs + 8, recursion_count + 1);
    } else {
        // Need to wait - simplified: just spin
        // In real implementation, would block the thread
        memory->write_u32(cs + 4, lock_count + 1);
        memory->write_u32(cs + 8, 1);
        memory->write_u32(cs + 12, current_thread);
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_RtlLeaveCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs = static_cast<GuestAddr>(args[0]);
    
    s32 lock_count = static_cast<s32>(memory->read_u32(cs + 4));
    s32 recursion_count = static_cast<s32>(memory->read_u32(cs + 8));
    
    if (recursion_count > 1) {
        // Still have recursive locks
        memory->write_u32(cs + 4, lock_count - 1);
        memory->write_u32(cs + 8, recursion_count - 1);
    } else {
        // Release lock
        memory->write_u32(cs + 4, -1);
        memory->write_u32(cs + 8, 0);
        memory->write_u32(cs + 12, 0);
        
        // TODO: Wake waiting thread if any
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_RtlTryEnterCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs = static_cast<GuestAddr>(args[0]);
    
    s32 lock_count = static_cast<s32>(memory->read_u32(cs + 4));
    u32 owning_thread = memory->read_u32(cs + 12);
    u32 current_thread = 1;  // TODO: Get actual thread ID
    
    if (lock_count == -1) {
        // Unlocked - acquire
        memory->write_u32(cs + 4, 0);
        memory->write_u32(cs + 8, 1);
        memory->write_u32(cs + 12, current_thread);
        *result = 1;  // TRUE - acquired
    } else if (owning_thread == current_thread) {
        // Already own it
        memory->write_u32(cs + 4, lock_count + 1);
        s32 recursion = static_cast<s32>(memory->read_u32(cs + 8));
        memory->write_u32(cs + 8, recursion + 1);
        *result = 1;  // TRUE - acquired
    } else {
        // Locked by another thread
        *result = 0;  // FALSE - not acquired
    }
}

static void HLE_RtlDeleteCriticalSection(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr cs = static_cast<GuestAddr>(args[0]);
    
    // Zero out the structure
    for (u32 i = 0; i < 24; i += 4) {
        memory->write_u32(cs + i, 0);
    }
    
    *result = STATUS_SUCCESS;
}

//=============================================================================
// File I/O Functions
//=============================================================================

static void HLE_NtCreateFile(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtCreateFile(
    //   PHANDLE FileHandle,               // arg[0]
    //   ACCESS_MASK DesiredAccess,        // arg[1]
    //   POBJECT_ATTRIBUTES ObjectAttributes, // arg[2]
    //   PIO_STATUS_BLOCK IoStatusBlock,   // arg[3]
    //   PLARGE_INTEGER AllocationSize,    // arg[4]
    //   ULONG FileAttributes,             // arg[5]
    //   ULONG ShareAccess,                // arg[6]
    //   ULONG CreateDisposition,          // arg[7]
    //   ULONG CreateOptions               // [stack+0]
    // );
    
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 desired_access = static_cast<u32>(args[1]);
    GuestAddr obj_attr_ptr = static_cast<GuestAddr>(args[2]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[3]);
    u32 create_disposition = static_cast<u32>(args[7]);
    
    // Get path from OBJECT_ATTRIBUTES
    std::string xbox_path = read_object_attributes_path(memory, obj_attr_ptr);
    std::string host_path = translate_xbox_path(xbox_path);
    
    LOGD("NtCreateFile: '%s' -> '%s', access=0x%X, disp=%u", 
         xbox_path.c_str(), host_path.c_str(), desired_access, create_disposition);
    
    // Check if VFS is available
    if (g_hle.vfs) {
        u32 vfs_handle;
        // Convert Xbox access flags to VFS FileAccess
        FileAccess vfs_access = (desired_access & (GENERIC_WRITE | FILE_WRITE_DATA)) 
                                ? FileAccess::ReadWrite 
                                : FileAccess::Read;
        Status status = g_hle.vfs->open_file(host_path, vfs_access, vfs_handle);
        
        if (status == Status::Ok) {
            memory->write_u32(handle_ptr, vfs_handle);
            if (io_status_ptr) {
                memory->write_u32(io_status_ptr, STATUS_SUCCESS);
                memory->write_u32(io_status_ptr + 4, 0);  // Information
            }
            *result = STATUS_SUCCESS;
            return;
        }
    }
    
    // Fallback: try direct file access
    std::lock_guard<std::mutex> lock(g_hle.file_mutex);
    
    std::ios::openmode mode = std::ios::binary;
    if (desired_access & (GENERIC_READ | FILE_READ_DATA)) {
        mode |= std::ios::in;
    }
    if (desired_access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) {
        mode |= std::ios::out;
    }
    
    // Handle create disposition
    switch (create_disposition) {
        case FILE_OPEN:
            // Open existing only
            break;
        case FILE_CREATE:
        case FILE_OPEN_IF:
        case FILE_OVERWRITE_IF:
            mode |= std::ios::out;
            if (create_disposition == FILE_OVERWRITE_IF) {
                mode |= std::ios::trunc;
            }
            break;
    }
    
    u32 handle = g_hle.next_file_handle++;
    auto& file_handle = g_hle.file_handles[handle];
    file_handle.host_path = host_path;
    file_handle.access = desired_access;
    file_handle.position = 0;
    file_handle.is_directory = false;
    
    file_handle.file.open(host_path, mode);
    
    if (file_handle.file.is_open() || (create_disposition >= FILE_CREATE)) {
        memory->write_u32(handle_ptr, handle);
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, STATUS_SUCCESS);
            memory->write_u32(io_status_ptr + 4, FILE_OPEN);
        }
        *result = STATUS_SUCCESS;
        LOGD("NtCreateFile: opened handle=%u for '%s'", handle, host_path.c_str());
    } else {
        g_hle.file_handles.erase(handle);
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, STATUS_OBJECT_NAME_NOT_FOUND);
            memory->write_u32(io_status_ptr + 4, 0);
        }
        *result = STATUS_OBJECT_NAME_NOT_FOUND;
        LOGW("NtCreateFile: FAILED to open '%s'", host_path.c_str());
    }
}

static void HLE_NtOpenFile(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // Same as NtCreateFile with FILE_OPEN disposition
    // Reuse NtCreateFile implementation
    args[7] = FILE_OPEN;
    HLE_NtCreateFile(cpu, memory, args, result);
}

static void HLE_NtReadFile(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtReadFile(
    //   HANDLE FileHandle,           // arg[0]
    //   HANDLE Event,                // arg[1]
    //   PIO_APC_ROUTINE ApcRoutine,  // arg[2]
    //   PVOID ApcContext,            // arg[3]
    //   PIO_STATUS_BLOCK IoStatusBlock, // arg[4]
    //   PVOID Buffer,                // arg[5]
    //   ULONG Length,                // arg[6]
    //   PLARGE_INTEGER ByteOffset,   // arg[7]
    //   PULONG Key                   // [stack]
    // );
    
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[4]);
    GuestAddr buffer = static_cast<GuestAddr>(args[5]);
    u32 length = static_cast<u32>(args[6]);
    GuestAddr byte_offset_ptr = static_cast<GuestAddr>(args[7]);
    
    // Try VFS first
    if (g_hle.vfs) {
        u64 bytes_read = 0;
        void* host_buffer = memory->get_host_ptr(buffer);
        if (host_buffer) {
            Status status = g_hle.vfs->read_file(handle, host_buffer, length, bytes_read);
            if (status == Status::Ok) {
                if (io_status_ptr) {
                    memory->write_u32(io_status_ptr, STATUS_SUCCESS);
                    memory->write_u32(io_status_ptr + 4, static_cast<u32>(bytes_read));
                }
                *result = (bytes_read == 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
                return;
            }
        }
    }
    
    // Fallback to internal handles
    std::lock_guard<std::mutex> lock(g_hle.file_mutex);
    
    auto it = g_hle.file_handles.find(handle);
    if (it == g_hle.file_handles.end()) {
        *result = STATUS_INVALID_HANDLE;
        return;
    }
    
    auto& fh = it->second;
    
    // Seek if offset provided
    if (byte_offset_ptr) {
        s64 offset = static_cast<s64>(memory->read_u64(byte_offset_ptr));
        if (offset >= 0) {
            fh.file.seekg(offset);
            fh.position = static_cast<u64>(offset);
        }
    }
    
    // Read data
    std::vector<char> temp_buffer(length);
    fh.file.read(temp_buffer.data(), length);
    std::streamsize bytes_read = fh.file.gcount();
    
    if (bytes_read > 0) {
        memory->write_bytes(buffer, temp_buffer.data(), static_cast<u64>(bytes_read));
        fh.position += static_cast<u64>(bytes_read);
    }
    
    if (io_status_ptr) {
        memory->write_u32(io_status_ptr, bytes_read > 0 ? STATUS_SUCCESS : STATUS_END_OF_FILE);
        memory->write_u32(io_status_ptr + 4, static_cast<u32>(bytes_read));
    }
    
    *result = (bytes_read > 0) ? STATUS_SUCCESS : STATUS_END_OF_FILE;
    LOGD("NtReadFile: handle=%u, len=%u, read=%ld", handle, length, bytes_read);
}

static void HLE_NtWriteFile(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[4]);
    GuestAddr buffer = static_cast<GuestAddr>(args[5]);
    u32 length = static_cast<u32>(args[6]);
    GuestAddr byte_offset_ptr = static_cast<GuestAddr>(args[7]);
    
    // Try VFS first
    if (g_hle.vfs) {
        u64 bytes_written = 0;
        const void* host_buffer = memory->get_host_ptr(buffer);
        if (host_buffer) {
            Status status = g_hle.vfs->write_file(handle, host_buffer, length, bytes_written);
            if (status == Status::Ok) {
                if (io_status_ptr) {
                    memory->write_u32(io_status_ptr, STATUS_SUCCESS);
                    memory->write_u32(io_status_ptr + 4, static_cast<u32>(bytes_written));
                }
                *result = STATUS_SUCCESS;
                return;
            }
        }
    }
    
    // Fallback
    std::lock_guard<std::mutex> lock(g_hle.file_mutex);
    
    auto it = g_hle.file_handles.find(handle);
    if (it == g_hle.file_handles.end()) {
        *result = STATUS_INVALID_HANDLE;
        return;
    }
    
    auto& fh = it->second;
    
    // Seek if offset provided
    if (byte_offset_ptr) {
        s64 offset = static_cast<s64>(memory->read_u64(byte_offset_ptr));
        if (offset >= 0) {
            fh.file.seekp(offset);
            fh.position = static_cast<u64>(offset);
        }
    }
    
    // Write data
    std::vector<char> temp_buffer(length);
    memory->read_bytes(buffer, temp_buffer.data(), length);
    
    fh.file.write(temp_buffer.data(), length);
    u32 bytes_written = fh.file.good() ? length : 0;
    fh.position += bytes_written;
    
    if (io_status_ptr) {
        memory->write_u32(io_status_ptr, STATUS_SUCCESS);
        memory->write_u32(io_status_ptr + 4, bytes_written);
    }
    
    *result = STATUS_SUCCESS;
    LOGD("NtWriteFile: handle=%u, len=%u, written=%u", handle, length, bytes_written);
}

static void HLE_NtQueryInformationFile(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtQueryInformationFile(
    //   HANDLE FileHandle,
    //   PIO_STATUS_BLOCK IoStatusBlock,
    //   PVOID FileInformation,
    //   ULONG Length,
    //   FILE_INFORMATION_CLASS FileInformationClass
    // );
    
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[2]);
    u32 length = static_cast<u32>(args[3]);
    u32 info_class = static_cast<u32>(args[4]);
    
    // Try VFS first
    if (g_hle.vfs) {
        u64 file_size = 0;
        if (g_hle.vfs->get_file_size(handle, file_size) == Status::Ok) {
            switch (info_class) {
                case FileStandardInformation:
                    // FILE_STANDARD_INFORMATION
                    memory->write_u64(info_ptr + 0, file_size);  // AllocationSize
                    memory->write_u64(info_ptr + 8, file_size);  // EndOfFile
                    memory->write_u32(info_ptr + 16, 1);         // NumberOfLinks
                    memory->write_u8(info_ptr + 20, 0);          // DeletePending
                    memory->write_u8(info_ptr + 21, 0);          // Directory
                    break;
                    
                case FilePositionInformation:
                    // FILE_POSITION_INFORMATION - would need to track position
                    memory->write_u64(info_ptr, 0);
                    break;
                    
                case FileBasicInformation:
                    // FILE_BASIC_INFORMATION
                    memory->write_u64(info_ptr + 0, 0);   // CreationTime
                    memory->write_u64(info_ptr + 8, 0);   // LastAccessTime
                    memory->write_u64(info_ptr + 16, 0);  // LastWriteTime
                    memory->write_u64(info_ptr + 24, 0);  // ChangeTime
                    memory->write_u32(info_ptr + 32, 0x80); // FileAttributes (NORMAL)
                    break;
                    
                default:
                    break;
            }
            
            if (io_status_ptr) {
                memory->write_u32(io_status_ptr, STATUS_SUCCESS);
                memory->write_u32(io_status_ptr + 4, length);
            }
            *result = STATUS_SUCCESS;
            return;
        }
    }
    
    // Fallback
    std::lock_guard<std::mutex> lock(g_hle.file_mutex);
    
    auto it = g_hle.file_handles.find(handle);
    if (it == g_hle.file_handles.end()) {
        *result = STATUS_INVALID_HANDLE;
        return;
    }
    
    auto& fh = it->second;
    
    switch (info_class) {
        case FileStandardInformation: {
            // Get file size
            auto current = fh.file.tellg();
            fh.file.seekg(0, std::ios::end);
            u64 file_size = static_cast<u64>(fh.file.tellg());
            fh.file.seekg(current);
            
            memory->write_u64(info_ptr + 0, file_size);
            memory->write_u64(info_ptr + 8, file_size);
            memory->write_u32(info_ptr + 16, 1);
            memory->write_u8(info_ptr + 20, 0);
            memory->write_u8(info_ptr + 21, fh.is_directory ? 1 : 0);
            break;
        }
        
        case FilePositionInformation:
            memory->write_u64(info_ptr, fh.position);
            break;
            
        default:
            // Zero the buffer
            memory->zero_bytes(info_ptr, length);
            break;
    }
    
    if (io_status_ptr) {
        memory->write_u32(io_status_ptr, STATUS_SUCCESS);
        memory->write_u32(io_status_ptr + 4, length);
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_NtSetInformationFile(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[2]);
    u32 length = static_cast<u32>(args[3]);
    u32 info_class = static_cast<u32>(args[4]);
    
    std::lock_guard<std::mutex> lock(g_hle.file_mutex);
    
    auto it = g_hle.file_handles.find(handle);
    if (it == g_hle.file_handles.end()) {
        *result = STATUS_INVALID_HANDLE;
        return;
    }
    
    auto& fh = it->second;
    
    switch (info_class) {
        case FilePositionInformation: {
            u64 new_position = memory->read_u64(info_ptr);
            fh.position = new_position;
            fh.file.seekg(static_cast<std::streamoff>(new_position));
            fh.file.seekp(static_cast<std::streamoff>(new_position));
            break;
        }
        
        case FileEndOfFileInformation: {
            // Truncate/extend file
            u64 new_size = memory->read_u64(info_ptr);
            // Would need to actually truncate the file
            break;
        }
        
        default:
            break;
    }
    
    if (io_status_ptr) {
        memory->write_u32(io_status_ptr, STATUS_SUCCESS);
        memory->write_u32(io_status_ptr + 4, length);
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_NtClose(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 handle = static_cast<u32>(args[0]);
    
    // Try VFS first
    if (g_hle.vfs) {
        if (g_hle.vfs->close_file(handle) == Status::Ok) {
            *result = STATUS_SUCCESS;
            return;
        }
    }
    
    // Fallback
    std::lock_guard<std::mutex> lock(g_hle.file_mutex);
    
    auto it = g_hle.file_handles.find(handle);
    if (it != g_hle.file_handles.end()) {
        it->second.file.close();
        g_hle.file_handles.erase(it);
        LOGD("NtClose: closed handle=%u", handle);
    }
    
    *result = STATUS_SUCCESS;
}

static void HLE_NtQueryFullAttributesFile(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr obj_attr_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[1]);
    
    std::string xbox_path = read_object_attributes_path(memory, obj_attr_ptr);
    std::string host_path = translate_xbox_path(xbox_path);
    
    // Check if file exists
    std::ifstream test(host_path);
    if (test.good()) {
        test.seekg(0, std::ios::end);
        u64 file_size = static_cast<u64>(test.tellg());
        test.close();
        
        // FILE_NETWORK_OPEN_INFORMATION
        memory->write_u64(info_ptr + 0, 0);         // CreationTime
        memory->write_u64(info_ptr + 8, 0);         // LastAccessTime
        memory->write_u64(info_ptr + 16, 0);        // LastWriteTime
        memory->write_u64(info_ptr + 24, 0);        // ChangeTime
        memory->write_u64(info_ptr + 32, file_size); // AllocationSize
        memory->write_u64(info_ptr + 40, file_size); // EndOfFile
        memory->write_u32(info_ptr + 48, 0x80);     // FileAttributes (NORMAL)
        
        *result = STATUS_SUCCESS;
    } else {
        *result = STATUS_OBJECT_NAME_NOT_FOUND;
    }
}

//=============================================================================
// String Functions
//=============================================================================

static void HLE_RtlInitAnsiString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr ansi_string = static_cast<GuestAddr>(args[0]);
    GuestAddr source = static_cast<GuestAddr>(args[1]);
    
    u16 length = 0;
    if (source) {
        while (memory->read_u8(source + length) != 0 && length < 0xFFFE) {
            length++;
        }
    }
    
    // ANSI_STRING structure: Length (2), MaximumLength (2), Buffer (4)
    memory->write_u16(ansi_string, length);
    memory->write_u16(ansi_string + 2, length + 1);
    memory->write_u32(ansi_string + 4, source);
    
    *result = 0;
}

static void HLE_RtlInitUnicodeString(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr unicode_string = static_cast<GuestAddr>(args[0]);
    GuestAddr source = static_cast<GuestAddr>(args[1]);
    
    u16 length = 0;
    if (source) {
        while (memory->read_u16(source + length) != 0 && length < 0xFFFE) {
            length += 2;
        }
    }
    
    memory->write_u16(unicode_string, length);
    memory->write_u16(unicode_string + 2, length + 2);
    memory->write_u32(unicode_string + 4, source);
    
    *result = 0;
}

static void HLE_RtlNtStatusToDosError(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    u32 status = static_cast<u32>(args[0]);
    
    // Simple mapping
    if (status == STATUS_SUCCESS) {
        *result = 0;
    } else {
        *result = status & 0xFFFF;
    }
}

//=============================================================================
// Debug Functions
//=============================================================================

static void HLE_DbgPrint(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr format_ptr = static_cast<GuestAddr>(args[0]);
    
    std::string format = read_cstring(memory, format_ptr);
    LOGI("DbgPrint: %s", format.c_str());
    
    *result = STATUS_SUCCESS;
}

//=============================================================================
// Exception Handling
//=============================================================================

static void HLE_RtlRaiseException(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr exception_record = static_cast<GuestAddr>(args[0]);
    
    u32 exception_code = memory->read_u32(exception_record);
    LOGW("RtlRaiseException: code=0x%08X", exception_code);
    
    *result = 0;
}

//=============================================================================
// TLS (Thread Local Storage)
//=============================================================================

static void HLE_RtlGetStackLimits(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr low_limit_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr high_limit_ptr = static_cast<GuestAddr>(args[1]);
    
    // Return approximate stack limits (in virtual address range 0x8E000000+)
    memory->write_u32(low_limit_ptr, 0x8E000000);
    memory->write_u32(high_limit_ptr, 0x8F000000);
    
    *result = 0;
}

//=============================================================================
// Mutex Functions
//=============================================================================

static void HLE_KeInitializeMutant(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr mutant = static_cast<GuestAddr>(args[0]);
    u32 initial_owner = static_cast<u32>(args[1]);
    
    // DISPATCHER_HEADER + mutant fields
    memory->write_u8(mutant + 0, 2);  // Type = Mutant
    memory->write_u8(mutant + 1, 0);
    memory->write_u8(mutant + 2, sizeof(u32) * 6);
    memory->write_u8(mutant + 3, 0);
    memory->write_u32(mutant + 4, initial_owner ? 0 : 1);  // SignalState (1 = available)
    memory->write_u32(mutant + 16, 0);  // OwnerThread
    memory->write_u32(mutant + 20, 0);  // AbandonedState
    
    *result = 0;
}

static void HLE_KeReleaseMutant(Cpu* cpu, Memory* memory, u64* args, u64* result) {
    GuestAddr mutant = static_cast<GuestAddr>(args[0]);
    s32 increment = static_cast<s32>(args[1]);
    u32 abandoned = static_cast<u32>(args[2]);
    u32 wait = static_cast<u32>(args[3]);
    
    s32 prev_state = static_cast<s32>(memory->read_u32(mutant + 4));
    
    // Release - increment signal state
    memory->write_u32(mutant + 4, static_cast<u32>(prev_state + 1));
    memory->write_u32(mutant + 16, 0);  // Clear owner
    
    // TODO: Wake waiting threads
    
    *result = prev_state;
}

//=============================================================================
// Registration
//=============================================================================

void Kernel::register_xboxkrnl() {
    // Memory management
    hle_functions_[make_import_key(0, 186)] = HLE_NtAllocateVirtualMemory;
    hle_functions_[make_import_key(0, 199)] = HLE_NtFreeVirtualMemory;
    hle_functions_[make_import_key(0, 206)] = HLE_NtQueryVirtualMemory;
    hle_functions_[make_import_key(0, 205)] = HLE_NtProtectVirtualMemory;
    
    // Thread management
    hle_functions_[make_import_key(0, 55)] = HLE_KeGetCurrentProcessType;
    hle_functions_[make_import_key(0, 84)] = HLE_KeSetAffinityThread;
    hle_functions_[make_import_key(0, 102)] = HLE_KeQueryPerformanceCounter;
    hle_functions_[make_import_key(0, 103)] = HLE_KeQueryPerformanceFrequency;
    hle_functions_[make_import_key(0, 40)] = HLE_KeDelayExecutionThread;
    
    // Synchronization - Events
    hle_functions_[make_import_key(0, 58)] = HLE_KeInitializeEvent;
    hle_functions_[make_import_key(0, 82)] = HLE_KeSetEvent;
    hle_functions_[make_import_key(0, 77)] = HLE_KeResetEvent;
    hle_functions_[make_import_key(0, 99)] = HLE_KePulseEvent;
    hle_functions_[make_import_key(0, 94)] = HLE_KeWaitForSingleObject;
    hle_functions_[make_import_key(0, 95)] = HLE_KeWaitForMultipleObjects;
    
    // Synchronization - Semaphores
    hle_functions_[make_import_key(0, 60)] = HLE_KeInitializeSemaphore;
    hle_functions_[make_import_key(0, 108)] = HLE_KeReleaseSemaphore;
    
    // Synchronization - Mutexes
    hle_functions_[make_import_key(0, 59)] = HLE_KeInitializeMutant;
    hle_functions_[make_import_key(0, 107)] = HLE_KeReleaseMutant;
    
    // Critical sections
    hle_functions_[make_import_key(0, 277)] = HLE_RtlInitializeCriticalSection;
    hle_functions_[make_import_key(0, 278)] = HLE_RtlInitializeCriticalSectionAndSpinCount;
    hle_functions_[make_import_key(0, 274)] = HLE_RtlEnterCriticalSection;
    hle_functions_[make_import_key(0, 285)] = HLE_RtlLeaveCriticalSection;
    hle_functions_[make_import_key(0, 290)] = HLE_RtlTryEnterCriticalSection;
    hle_functions_[make_import_key(0, 272)] = HLE_RtlDeleteCriticalSection;
    
    // File I/O
    hle_functions_[make_import_key(0, 190)] = HLE_NtCreateFile;
    hle_functions_[make_import_key(0, 202)] = HLE_NtOpenFile;
    hle_functions_[make_import_key(0, 207)] = HLE_NtReadFile;
    hle_functions_[make_import_key(0, 218)] = HLE_NtWriteFile;
    hle_functions_[make_import_key(0, 204)] = HLE_NtQueryInformationFile;
    hle_functions_[make_import_key(0, 211)] = HLE_NtSetInformationFile;
    hle_functions_[make_import_key(0, 187)] = HLE_NtClose;
    hle_functions_[make_import_key(0, 203)] = HLE_NtQueryFullAttributesFile;
    
    // Strings
    hle_functions_[make_import_key(0, 276)] = HLE_RtlInitAnsiString;
    hle_functions_[make_import_key(0, 279)] = HLE_RtlInitUnicodeString;
    hle_functions_[make_import_key(0, 280)] = HLE_RtlNtStatusToDosError;
    
    // Debug
    hle_functions_[make_import_key(0, 7)] = HLE_DbgPrint;
    
    // Exception handling
    hle_functions_[make_import_key(0, 284)] = HLE_RtlRaiseException;
    
    // TLS/Stack
    hle_functions_[make_import_key(0, 275)] = HLE_RtlGetStackLimits;
    
    LOGI("Registered xboxkrnl.exe HLE functions (basic)");
}

// Initialize HLE state
void init_hle_state(VirtualFileSystem* vfs) {
    g_hle.vfs = vfs;
    
    // Set up default path mappings
    g_hle.path_mappings["game:"] = "./";
    g_hle.path_mappings["dvd:"] = "./";
    g_hle.path_mappings["hdd:"] = "./save/";
    g_hle.path_mappings["cache:"] = "./cache/";
}

} // namespace x360mu
