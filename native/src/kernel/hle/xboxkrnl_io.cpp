/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Kernel File I/O HLE Implementation
 * 
 * This file implements High-Level Emulation of Xbox 360 kernel file I/O functions:
 * - NtCreateFile - Open/create files
 * - NtReadFile - Read from files
 * - NtWriteFile - Write to files
 * - NtQueryInformationFile - Query file metadata
 * - NtSetInformationFile - Set file metadata
 * - NtQueryDirectoryFile - Directory enumeration
 * - NtQueryFullAttributesFile - Query file attributes by path
 * - NtClose - Close handles
 */

#include "../kernel.h"
#include "../filesystem/vfs.h"
#include "../../cpu/xenon/cpu.h"
#include "../../memory/memory.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <regex>
#include <fnmatch.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-io"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[IO] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#define LOGW(...) printf("[IO WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[IO ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

//=============================================================================
// NTSTATUS Codes
//=============================================================================
namespace nt {
    constexpr u32 STATUS_SUCCESS              = 0x00000000;
    constexpr u32 STATUS_PENDING              = 0x00000103;
    constexpr u32 STATUS_BUFFER_OVERFLOW      = 0x80000005;
    constexpr u32 STATUS_NO_MORE_FILES        = 0x80000006;
    constexpr u32 STATUS_UNSUCCESSFUL         = 0xC0000001;
    constexpr u32 STATUS_NOT_IMPLEMENTED      = 0xC0000002;
    constexpr u32 STATUS_INVALID_HANDLE       = 0xC0000008;
    constexpr u32 STATUS_INVALID_PARAMETER    = 0xC000000D;
    constexpr u32 STATUS_NO_SUCH_FILE         = 0xC000000F;
    constexpr u32 STATUS_END_OF_FILE          = 0xC0000011;
    constexpr u32 STATUS_NO_MEMORY            = 0xC0000017;
    constexpr u32 STATUS_ACCESS_DENIED        = 0xC0000022;
    constexpr u32 STATUS_BUFFER_TOO_SMALL     = 0xC0000023;
    constexpr u32 STATUS_OBJECT_NAME_INVALID  = 0xC0000033;
    constexpr u32 STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
    constexpr u32 STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
    constexpr u32 STATUS_OBJECT_PATH_SYNTAX_BAD = 0xC000003B;
}

//=============================================================================
// File Access Constants
//=============================================================================
namespace file_access {
    constexpr u32 GENERIC_READ    = 0x80000000;
    constexpr u32 GENERIC_WRITE   = 0x40000000;
    constexpr u32 GENERIC_EXECUTE = 0x20000000;
    constexpr u32 GENERIC_ALL     = 0x10000000;
    constexpr u32 FILE_READ_DATA  = 0x0001;
    constexpr u32 FILE_WRITE_DATA = 0x0002;
    constexpr u32 FILE_APPEND_DATA = 0x0004;
    constexpr u32 FILE_LIST_DIRECTORY = 0x0001;
}

//=============================================================================
// File Disposition Constants
//=============================================================================
namespace file_disposition {
    constexpr u32 FILE_SUPERSEDE    = 0;
    constexpr u32 FILE_OPEN         = 1;
    constexpr u32 FILE_CREATE       = 2;
    constexpr u32 FILE_OPEN_IF      = 3;
    constexpr u32 FILE_OVERWRITE    = 4;
    constexpr u32 FILE_OVERWRITE_IF = 5;
}

//=============================================================================
// File Create Options
//=============================================================================
namespace file_options {
    constexpr u32 FILE_DIRECTORY_FILE     = 0x00000001;
    constexpr u32 FILE_NON_DIRECTORY_FILE = 0x00000040;
    constexpr u32 FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;
    constexpr u32 FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;
}

//=============================================================================
// File Attributes
//=============================================================================
namespace file_attr {
    constexpr u32 FILE_ATTRIBUTE_READONLY   = 0x00000001;
    constexpr u32 FILE_ATTRIBUTE_HIDDEN     = 0x00000002;
    constexpr u32 FILE_ATTRIBUTE_SYSTEM     = 0x00000004;
    constexpr u32 FILE_ATTRIBUTE_DIRECTORY  = 0x00000010;
    constexpr u32 FILE_ATTRIBUTE_ARCHIVE    = 0x00000020;
    constexpr u32 FILE_ATTRIBUTE_NORMAL     = 0x00000080;
}

//=============================================================================
// File Information Classes
//=============================================================================
enum class FileInformationClass : u32 {
    FileDirectoryInformation = 1,
    FileFullDirectoryInformation = 2,
    FileBothDirectoryInformation = 3,
    FileBasicInformation = 4,
    FileStandardInformation = 5,
    FileInternalInformation = 6,
    FileEaInformation = 7,
    FileAccessInformation = 8,
    FileNameInformation = 9,
    FileRenameInformation = 10,
    FileLinkInformation = 11,
    FileNamesInformation = 12,
    FileDispositionInformation = 13,
    FilePositionInformation = 14,
    FileFullEaInformation = 15,
    FileModeInformation = 16,
    FileAlignmentInformation = 17,
    FileAllInformation = 18,
    FileAllocationInformation = 19,
    FileEndOfFileInformation = 20,
    FileAlternateNameInformation = 21,
    FileStreamInformation = 22,
    FileNetworkOpenInformation = 34,
    FileAttributeTagInformation = 35,
    FileIdBothDirectoryInformation = 37,
    FileIdFullDirectoryInformation = 38,
};

//=============================================================================
// IO Status Block Information Values
//=============================================================================
namespace io_status {
    constexpr u32 FILE_SUPERSEDED  = 0;
    constexpr u32 FILE_OPENED      = 1;
    constexpr u32 FILE_CREATED     = 2;
    constexpr u32 FILE_OVERWRITTEN = 3;
    constexpr u32 FILE_EXISTS      = 4;
    constexpr u32 FILE_DOES_NOT_EXIST = 5;
}

//=============================================================================
// Global File I/O State
//=============================================================================
struct FileIOState {
    // VFS pointer (set by kernel init)
    VirtualFileSystem* vfs = nullptr;
    
    // Directory enumeration state per handle
    struct DirEnumState {
        std::string path;
        std::string pattern;
        std::vector<DirEntry> entries;
        size_t current_index;
        bool scan_complete;
    };
    std::unordered_map<u32, DirEnumState> dir_enum_states;
    std::mutex dir_mutex;
    
    // File handle to VFS handle mapping
    std::unordered_map<u32, u32> kernel_to_vfs_handles;
    u32 next_kernel_handle = 0x1000;
    std::mutex handle_mutex;
};

static FileIOState g_io_state;

//=============================================================================
// Helper Functions
//=============================================================================

/**
 * Read UNICODE_STRING from guest memory
 * UNICODE_STRING: { u16 Length, u16 MaxLength, u32 Buffer }
 */
static std::string read_unicode_string(Memory* memory, GuestAddr string_ptr) {
    if (string_ptr == 0) return "";
    
    u16 length = memory->read_u16(string_ptr);         // Length in bytes
    u16 max_length = memory->read_u16(string_ptr + 2); // Max length
    GuestAddr buffer = memory->read_u32(string_ptr + 4);
    
    if (buffer == 0 || length == 0) return "";
    
    // Xbox 360 uses wide characters (UTF-16BE due to big-endian)
    std::string result;
    result.reserve(length / 2);
    
    for (u16 i = 0; i < length; i += 2) {
        u16 wchar = memory->read_u16(buffer + i);
        // Simple conversion - just take low byte for ASCII
        if (wchar < 256) {
            result += static_cast<char>(wchar);
        } else {
            result += '?';  // Replace non-ASCII
        }
    }
    
    return result;
}

/**
 * Write string as UNICODE to guest memory
 */
static void write_unicode_string(Memory* memory, GuestAddr buffer, const std::string& str, u32 max_bytes) {
    u32 bytes_to_write = std::min(static_cast<u32>(str.size() * 2), max_bytes);
    
    for (u32 i = 0; i < bytes_to_write / 2 && i < str.size(); i++) {
        memory->write_u16(buffer + i * 2, static_cast<u16>(static_cast<u8>(str[i])));
    }
}

/**
 * Read ANSI_STRING from guest memory  
 * ANSI_STRING: { u16 Length, u16 MaxLength, u32 Buffer }
 */
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

/**
 * Read path from OBJECT_ATTRIBUTES structure
 * OBJECT_ATTRIBUTES: { u32 Length, u32 RootDirectory, u32 ObjectName, u32 Attributes, u32 SecurityDesc, u32 SecurityQoS }
 */
static std::string read_object_attributes_path(Memory* memory, GuestAddr obj_attr_ptr) {
    if (obj_attr_ptr == 0) return "";
    
    // u32 length = memory->read_u32(obj_attr_ptr);
    // GuestAddr root_dir = memory->read_u32(obj_attr_ptr + 4);
    GuestAddr object_name_ptr = memory->read_u32(obj_attr_ptr + 8);
    
    // Try Unicode first, fall back to ANSI
    std::string path = read_unicode_string(memory, object_name_ptr);
    if (path.empty()) {
        path = read_ansi_string(memory, object_name_ptr);
    }
    
    return path;
}

/**
 * Translate Xbox 360 path to VFS path
 */
static std::string translate_xbox_path(const std::string& xbox_path) {
    std::string path = xbox_path;
    
    // Remove leading backslashes
    while (!path.empty() && (path[0] == '\\' || path[0] == '/')) {
        path = path.substr(1);
    }
    
    // Handle device paths: \Device\Cdrom0\, \Device\Harddisk0\, etc.
    if (path.find("Device\\") == 0 || path.find("Device/") == 0) {
        size_t pos = path.find('\\', 7);
        if (pos == std::string::npos) pos = path.find('/', 7);
        
        if (pos != std::string::npos) {
            std::string device = path.substr(7, pos - 7);
            path = path.substr(pos + 1);
            
            // Map known devices
            if (device == "Cdrom0" || device == "cdrom0") {
                path = "game:" + path;
            } else if (device.find("Harddisk") == 0) {
                path = "hdd:" + path;
            } else if (device == "Flash" || device == "flash") {
                path = "flash:" + path;
            }
        }
    }
    
    // Handle common Xbox 360 path prefixes
    auto to_lower = [](const std::string& s, size_t len) {
        std::string result;
        for (size_t i = 0; i < len && i < s.size(); i++) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        }
        return result;
    };
    
    if (to_lower(path, 5) == "game:" || to_lower(path, 4) == "dvd:") {
        // Already has device prefix, normalize it
        size_t colon = path.find(':');
        if (colon != std::string::npos) {
            path = "game:" + path.substr(colon + 1);
        }
    } else if (to_lower(path, 4) == "hdd:") {
        // HDD path - keep as is
    } else if (to_lower(path, 6) == "cache:") {
        // Cache path - keep as is  
    } else if (to_lower(path, 6) == "title:") {
        // Title storage - keep as is
    } else if (!path.empty() && path.find(':') == std::string::npos) {
        // No device prefix - assume game:
        path = "game:" + path;
    }
    
    // Normalize slashes
    std::replace(path.begin(), path.end(), '\\', '/');
    
    // Remove duplicate slashes after device:
    size_t colon = path.find(':');
    if (colon != std::string::npos && colon + 1 < path.size()) {
        while (colon + 1 < path.size() && path[colon + 1] == '/') {
            path.erase(colon + 1, 1);
        }
    }
    
    return path;
}

/**
 * Match filename against wildcard pattern
 */
static bool match_pattern(const std::string& name, const std::string& pattern) {
    if (pattern.empty() || pattern == "*" || pattern == "*.*") {
        return true;
    }
    
    // Use fnmatch for glob-style matching
    return fnmatch(pattern.c_str(), name.c_str(), FNM_CASEFOLD) == 0;
}

/**
 * Convert VFS FileAccess to Xbox access mask
 */
static FileAccess xbox_access_to_vfs(u32 desired_access) {
    u32 result = 0;
    
    if (desired_access & (file_access::GENERIC_READ | file_access::FILE_READ_DATA | 
                          file_access::FILE_LIST_DIRECTORY)) {
        result |= static_cast<u32>(FileAccess::Read);
    }
    if (desired_access & (file_access::GENERIC_WRITE | file_access::FILE_WRITE_DATA | 
                          file_access::FILE_APPEND_DATA)) {
        result |= static_cast<u32>(FileAccess::Write);
    }
    
    return static_cast<FileAccess>(result ? result : static_cast<u32>(FileAccess::Read));
}

/**
 * Convert Xbox disposition to VFS disposition
 */
static FileDisposition xbox_disposition_to_vfs(u32 disposition) {
    switch (disposition) {
        case file_disposition::FILE_SUPERSEDE:    return FileDisposition::Supersede;
        case file_disposition::FILE_OPEN:         return FileDisposition::Open;
        case file_disposition::FILE_CREATE:       return FileDisposition::Create;
        case file_disposition::FILE_OPEN_IF:      return FileDisposition::OpenIf;
        case file_disposition::FILE_OVERWRITE:    return FileDisposition::Overwrite;
        case file_disposition::FILE_OVERWRITE_IF: return FileDisposition::OverwriteIf;
        default:                                  return FileDisposition::Open;
    }
}

//=============================================================================
// NtCreateFile Implementation
//=============================================================================

static void HLE_NtCreateFile_IO(Cpu* /*cpu*/, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtCreateFile(
    //   PHANDLE FileHandle,              // arg[0] - OUT
    //   ACCESS_MASK DesiredAccess,       // arg[1]
    //   POBJECT_ATTRIBUTES ObjectAttr,   // arg[2]
    //   PIO_STATUS_BLOCK IoStatusBlock,  // arg[3] - OUT
    //   PLARGE_INTEGER AllocationSize,   // arg[4]
    //   ULONG FileAttributes,            // arg[5]
    //   ULONG ShareAccess,               // arg[6]
    //   ULONG CreateDisposition,         // arg[7]
    //   ULONG CreateOptions              // from stack
    // );
    
    GuestAddr handle_ptr = static_cast<GuestAddr>(args[0]);
    u32 desired_access = static_cast<u32>(args[1]);
    GuestAddr obj_attr_ptr = static_cast<GuestAddr>(args[2]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[3]);
    // GuestAddr alloc_size_ptr = static_cast<GuestAddr>(args[4]);
    // u32 file_attributes = static_cast<u32>(args[5]);
    // u32 share_access = static_cast<u32>(args[6]);
    u32 create_disposition = static_cast<u32>(args[7]);
    
    // Read path from OBJECT_ATTRIBUTES
    std::string xbox_path = read_object_attributes_path(memory, obj_attr_ptr);
    if (xbox_path.empty()) {
        LOGW("NtCreateFile: empty path");
        *result = nt::STATUS_OBJECT_NAME_INVALID;
        return;
    }
    
    std::string vfs_path = translate_xbox_path(xbox_path);
    
    LOGD("NtCreateFile: '%s' -> '%s', access=0x%08X, disp=%u",
         xbox_path.c_str(), vfs_path.c_str(), desired_access, create_disposition);
    
    if (!g_io_state.vfs) {
        LOGE("NtCreateFile: VFS not initialized");
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Open file via VFS
    FileAccess access = xbox_access_to_vfs(desired_access);
    FileDisposition disposition = xbox_disposition_to_vfs(create_disposition);
    
    u32 vfs_handle;
    Status status = g_io_state.vfs->open_file(vfs_path, access, disposition, vfs_handle);
    
    if (status == Status::Ok) {
        // Create kernel handle
        std::lock_guard<std::mutex> lock(g_io_state.handle_mutex);
        u32 kernel_handle = g_io_state.next_kernel_handle++;
        g_io_state.kernel_to_vfs_handles[kernel_handle] = vfs_handle;
        
        memory->write_u32(handle_ptr, kernel_handle);
        
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, nt::STATUS_SUCCESS);
            // Set Information based on what happened
            u32 info = (create_disposition == file_disposition::FILE_CREATE) 
                       ? io_status::FILE_CREATED : io_status::FILE_OPENED;
            memory->write_u32(io_status_ptr + 4, info);
        }
        
        LOGD("NtCreateFile: success, handle=0x%X", kernel_handle);
        *result = nt::STATUS_SUCCESS;
    } else {
        LOGW("NtCreateFile: failed to open '%s', status=%d", vfs_path.c_str(), static_cast<int>(status));
        
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, nt::STATUS_OBJECT_NAME_NOT_FOUND);
            memory->write_u32(io_status_ptr + 4, 0);
        }
        
        *result = (status == Status::NotFound) ? nt::STATUS_OBJECT_NAME_NOT_FOUND : nt::STATUS_UNSUCCESSFUL;
    }
}

//=============================================================================
// NtReadFile Implementation  
//=============================================================================

static void HLE_NtReadFile_IO(Cpu* /*cpu*/, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtReadFile(
    //   HANDLE FileHandle,               // arg[0]
    //   HANDLE Event,                    // arg[1] - optional
    //   PIO_APC_ROUTINE ApcRoutine,      // arg[2] - optional
    //   PVOID ApcContext,                // arg[3]
    //   PIO_STATUS_BLOCK IoStatusBlock,  // arg[4] - OUT
    //   PVOID Buffer,                    // arg[5] - OUT
    //   ULONG Length,                    // arg[6]
    //   PLARGE_INTEGER ByteOffset,       // arg[7] - optional
    // );
    
    u32 kernel_handle = static_cast<u32>(args[0]);
    GuestAddr event_handle = static_cast<GuestAddr>(args[1]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[4]);
    GuestAddr buffer_ptr = static_cast<GuestAddr>(args[5]);
    u32 length = static_cast<u32>(args[6]);
    GuestAddr offset_ptr = static_cast<GuestAddr>(args[7]);
    
    if (!g_io_state.vfs) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Look up VFS handle
    u32 vfs_handle;
    {
        std::lock_guard<std::mutex> lock(g_io_state.handle_mutex);
        auto it = g_io_state.kernel_to_vfs_handles.find(kernel_handle);
        if (it == g_io_state.kernel_to_vfs_handles.end()) {
            *result = nt::STATUS_INVALID_HANDLE;
            return;
        }
        vfs_handle = it->second;
    }
    
    // Handle seek if offset provided
    if (offset_ptr) {
        s64 offset = static_cast<s64>(memory->read_u64(offset_ptr));
        if (offset >= 0) {
            u64 new_pos;
            g_io_state.vfs->seek_file(vfs_handle, offset, SeekOrigin::Begin, new_pos);
        }
    }
    
    // Read data
    void* host_buffer = memory->get_host_ptr(buffer_ptr);
    if (!host_buffer) {
        *result = nt::STATUS_INVALID_PARAMETER;
        return;
    }
    
    u64 bytes_read = 0;
    Status status = g_io_state.vfs->read_file(vfs_handle, host_buffer, length, bytes_read);
    
    if (status == Status::Ok) {
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, bytes_read > 0 ? nt::STATUS_SUCCESS : nt::STATUS_END_OF_FILE);
            memory->write_u32(io_status_ptr + 4, static_cast<u32>(bytes_read));
        }
        
        // Signal event if provided (for async I/O compatibility)
        if (event_handle) {
            // Would signal the event here
        }
        
        *result = bytes_read > 0 ? nt::STATUS_SUCCESS : nt::STATUS_END_OF_FILE;
    } else {
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, nt::STATUS_UNSUCCESSFUL);
            memory->write_u32(io_status_ptr + 4, 0);
        }
        *result = nt::STATUS_UNSUCCESSFUL;
    }
}

//=============================================================================
// NtWriteFile Implementation
//=============================================================================

static void HLE_NtWriteFile_IO(Cpu* /*cpu*/, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtWriteFile(
    //   HANDLE FileHandle,               // arg[0]
    //   HANDLE Event,                    // arg[1]
    //   PIO_APC_ROUTINE ApcRoutine,      // arg[2]
    //   PVOID ApcContext,                // arg[3]
    //   PIO_STATUS_BLOCK IoStatusBlock,  // arg[4]
    //   PVOID Buffer,                    // arg[5]
    //   ULONG Length,                    // arg[6]
    //   PLARGE_INTEGER ByteOffset,       // arg[7]
    // );
    
    u32 kernel_handle = static_cast<u32>(args[0]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[4]);
    GuestAddr buffer_ptr = static_cast<GuestAddr>(args[5]);
    u32 length = static_cast<u32>(args[6]);
    GuestAddr offset_ptr = static_cast<GuestAddr>(args[7]);
    
    if (!g_io_state.vfs) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Look up VFS handle
    u32 vfs_handle;
    {
        std::lock_guard<std::mutex> lock(g_io_state.handle_mutex);
        auto it = g_io_state.kernel_to_vfs_handles.find(kernel_handle);
        if (it == g_io_state.kernel_to_vfs_handles.end()) {
            *result = nt::STATUS_INVALID_HANDLE;
            return;
        }
        vfs_handle = it->second;
    }
    
    // Handle seek if offset provided
    if (offset_ptr) {
        s64 offset = static_cast<s64>(memory->read_u64(offset_ptr));
        if (offset >= 0) {
            u64 new_pos;
            g_io_state.vfs->seek_file(vfs_handle, offset, SeekOrigin::Begin, new_pos);
        }
    }
    
    // Write data
    const void* host_buffer = memory->get_host_ptr(buffer_ptr);
    if (!host_buffer) {
        *result = nt::STATUS_INVALID_PARAMETER;
        return;
    }
    
    u64 bytes_written = 0;
    Status status = g_io_state.vfs->write_file(vfs_handle, host_buffer, length, bytes_written);
    
    if (status == Status::Ok) {
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, nt::STATUS_SUCCESS);
            memory->write_u32(io_status_ptr + 4, static_cast<u32>(bytes_written));
        }
        *result = nt::STATUS_SUCCESS;
    } else {
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, nt::STATUS_ACCESS_DENIED);
            memory->write_u32(io_status_ptr + 4, 0);
        }
        *result = nt::STATUS_ACCESS_DENIED;
    }
}

//=============================================================================
// NtQueryInformationFile Implementation
//=============================================================================

static void HLE_NtQueryInformationFile_IO(Cpu* /*cpu*/, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtQueryInformationFile(
    //   HANDLE FileHandle,                      // arg[0]
    //   PIO_STATUS_BLOCK IoStatusBlock,         // arg[1]
    //   PVOID FileInformation,                  // arg[2]
    //   ULONG Length,                           // arg[3]
    //   FILE_INFORMATION_CLASS FileInfoClass    // arg[4]
    // );
    
    u32 kernel_handle = static_cast<u32>(args[0]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[2]);
    u32 length = static_cast<u32>(args[3]);
    u32 info_class = static_cast<u32>(args[4]);
    
    if (!g_io_state.vfs) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Look up VFS handle
    u32 vfs_handle;
    {
        std::lock_guard<std::mutex> lock(g_io_state.handle_mutex);
        auto it = g_io_state.kernel_to_vfs_handles.find(kernel_handle);
        if (it == g_io_state.kernel_to_vfs_handles.end()) {
            *result = nt::STATUS_INVALID_HANDLE;
            return;
        }
        vfs_handle = it->second;
    }
    
    // Get file size and position
    u64 file_size = 0;
    u64 file_position = 0;
    g_io_state.vfs->get_file_size(vfs_handle, file_size);
    g_io_state.vfs->get_file_position(vfs_handle, file_position);
    
    auto file_info_class = static_cast<FileInformationClass>(info_class);
    
    switch (file_info_class) {
        case FileInformationClass::FileBasicInformation: {
            // FILE_BASIC_INFORMATION: { CreationTime, LastAccessTime, LastWriteTime, ChangeTime, FileAttributes }
            if (length < 40) {
                *result = nt::STATUS_BUFFER_TOO_SMALL;
                return;
            }
            memory->write_u64(info_ptr + 0, 0);   // CreationTime
            memory->write_u64(info_ptr + 8, 0);   // LastAccessTime
            memory->write_u64(info_ptr + 16, 0);  // LastWriteTime
            memory->write_u64(info_ptr + 24, 0);  // ChangeTime
            memory->write_u32(info_ptr + 32, file_attr::FILE_ATTRIBUTE_NORMAL); // FileAttributes
            break;
        }
        
        case FileInformationClass::FileStandardInformation: {
            // FILE_STANDARD_INFORMATION: { AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory }
            if (length < 24) {
                *result = nt::STATUS_BUFFER_TOO_SMALL;
                return;
            }
            memory->write_u64(info_ptr + 0, file_size);  // AllocationSize
            memory->write_u64(info_ptr + 8, file_size);  // EndOfFile
            memory->write_u32(info_ptr + 16, 1);         // NumberOfLinks
            memory->write_u8(info_ptr + 20, 0);          // DeletePending
            memory->write_u8(info_ptr + 21, 0);          // Directory
            break;
        }
        
        case FileInformationClass::FilePositionInformation: {
            // FILE_POSITION_INFORMATION: { CurrentByteOffset }
            if (length < 8) {
                *result = nt::STATUS_BUFFER_TOO_SMALL;
                return;
            }
            memory->write_u64(info_ptr, file_position);
            break;
        }
        
        case FileInformationClass::FileNetworkOpenInformation: {
            // FILE_NETWORK_OPEN_INFORMATION: { CreationTime, LastAccessTime, LastWriteTime, ChangeTime, 
            //                                  AllocationSize, EndOfFile, FileAttributes }
            if (length < 56) {
                *result = nt::STATUS_BUFFER_TOO_SMALL;
                return;
            }
            memory->write_u64(info_ptr + 0, 0);         // CreationTime
            memory->write_u64(info_ptr + 8, 0);         // LastAccessTime
            memory->write_u64(info_ptr + 16, 0);        // LastWriteTime
            memory->write_u64(info_ptr + 24, 0);        // ChangeTime
            memory->write_u64(info_ptr + 32, file_size); // AllocationSize
            memory->write_u64(info_ptr + 40, file_size); // EndOfFile
            memory->write_u32(info_ptr + 48, file_attr::FILE_ATTRIBUTE_NORMAL); // FileAttributes
            break;
        }
        
        case FileInformationClass::FileNameInformation: {
            // FILE_NAME_INFORMATION: { FileNameLength, FileName[] }
            // We don't track the original name, return minimal info
            memory->write_u32(info_ptr, 0);  // FileNameLength = 0
            break;
        }
        
        default:
            LOGW("NtQueryInformationFile: unhandled info class %u", info_class);
            memory->zero_bytes(info_ptr, length);
            break;
    }
    
    if (io_status_ptr) {
        memory->write_u32(io_status_ptr, nt::STATUS_SUCCESS);
        memory->write_u32(io_status_ptr + 4, length);
    }
    
    *result = nt::STATUS_SUCCESS;
}

//=============================================================================
// NtSetInformationFile Implementation
//=============================================================================

static void HLE_NtSetInformationFile_IO(Cpu* /*cpu*/, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtSetInformationFile(
    //   HANDLE FileHandle,                      // arg[0]
    //   PIO_STATUS_BLOCK IoStatusBlock,         // arg[1]
    //   PVOID FileInformation,                  // arg[2]
    //   ULONG Length,                           // arg[3]
    //   FILE_INFORMATION_CLASS FileInfoClass    // arg[4]
    // );
    
    u32 kernel_handle = static_cast<u32>(args[0]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[1]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[2]);
    // u32 length = static_cast<u32>(args[3]);
    u32 info_class = static_cast<u32>(args[4]);
    
    if (!g_io_state.vfs) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Look up VFS handle
    u32 vfs_handle;
    {
        std::lock_guard<std::mutex> lock(g_io_state.handle_mutex);
        auto it = g_io_state.kernel_to_vfs_handles.find(kernel_handle);
        if (it == g_io_state.kernel_to_vfs_handles.end()) {
            *result = nt::STATUS_INVALID_HANDLE;
            return;
        }
        vfs_handle = it->second;
    }
    
    auto file_info_class = static_cast<FileInformationClass>(info_class);
    
    switch (file_info_class) {
        case FileInformationClass::FilePositionInformation: {
            // Set file position
            u64 new_position = memory->read_u64(info_ptr);
            u64 result_pos;
            g_io_state.vfs->seek_file(vfs_handle, static_cast<s64>(new_position), SeekOrigin::Begin, result_pos);
            break;
        }
        
        case FileInformationClass::FileEndOfFileInformation: {
            // Truncate file
            // u64 new_size = memory->read_u64(info_ptr);
            // Would truncate file - not implemented in VFS yet
            break;
        }
        
        case FileInformationClass::FileDispositionInformation: {
            // Mark for deletion - not implemented
            break;
        }
        
        default:
            LOGW("NtSetInformationFile: unhandled info class %u", info_class);
            break;
    }
    
    if (io_status_ptr) {
        memory->write_u32(io_status_ptr, nt::STATUS_SUCCESS);
        memory->write_u32(io_status_ptr + 4, 0);
    }
    
    *result = nt::STATUS_SUCCESS;
}

//=============================================================================
// NtQueryDirectoryFile Implementation
//=============================================================================

static void HLE_NtQueryDirectoryFile_IO(Cpu* /*cpu*/, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtQueryDirectoryFile(
    //   HANDLE FileHandle,                      // arg[0]
    //   HANDLE Event,                           // arg[1]
    //   PIO_APC_ROUTINE ApcRoutine,             // arg[2]
    //   PVOID ApcContext,                       // arg[3]
    //   PIO_STATUS_BLOCK IoStatusBlock,         // arg[4]
    //   PVOID FileInformation,                  // arg[5] - OUT
    //   ULONG Length,                           // arg[6]
    //   FILE_INFORMATION_CLASS FileInfoClass,   // arg[7]
    //   BOOLEAN ReturnSingleEntry,              // from stack
    //   PUNICODE_STRING FileName,               // from stack - search pattern
    //   BOOLEAN RestartScan                     // from stack
    // );
    
    u32 kernel_handle = static_cast<u32>(args[0]);
    GuestAddr io_status_ptr = static_cast<GuestAddr>(args[4]);
    GuestAddr info_buffer = static_cast<GuestAddr>(args[5]);
    u32 buffer_length = static_cast<u32>(args[6]);
    u32 info_class = static_cast<u32>(args[7]);
    
    // These would be on stack - for now use defaults
    bool return_single = true;
    bool restart_scan = false;
    std::string pattern = "*";
    
    if (!g_io_state.vfs) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Look up VFS handle
    u32 vfs_handle;
    {
        std::lock_guard<std::mutex> lock(g_io_state.handle_mutex);
        auto it = g_io_state.kernel_to_vfs_handles.find(kernel_handle);
        if (it == g_io_state.kernel_to_vfs_handles.end()) {
            *result = nt::STATUS_INVALID_HANDLE;
            return;
        }
        vfs_handle = it->second;
    }
    
    // Get or create directory enumeration state
    std::lock_guard<std::mutex> dir_lock(g_io_state.dir_mutex);
    auto& enum_state = g_io_state.dir_enum_states[kernel_handle];
    
    // Restart scan if requested or first time
    if (restart_scan || enum_state.entries.empty()) {
        enum_state.current_index = 0;
        enum_state.pattern = pattern;
        enum_state.entries.clear();
        enum_state.scan_complete = false;
        
        // Query directory from VFS
        // Note: We need the path - for now assume it's stored somewhere or we query it
        std::string dir_path = "game:";  // Default to root
        
        Status status = g_io_state.vfs->query_directory(dir_path, enum_state.entries);
        if (status != Status::Ok) {
            *result = nt::STATUS_NO_SUCH_FILE;
            return;
        }
        
        // Filter entries by pattern if needed
        if (!pattern.empty() && pattern != "*" && pattern != "*.*") {
            std::vector<DirEntry> filtered;
            for (const auto& entry : enum_state.entries) {
                if (match_pattern(entry.name, pattern)) {
                    filtered.push_back(entry);
                }
            }
            enum_state.entries = std::move(filtered);
        }
    }
    
    // Check if we have more entries
    if (enum_state.current_index >= enum_state.entries.size()) {
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, nt::STATUS_NO_MORE_FILES);
            memory->write_u32(io_status_ptr + 4, 0);
        }
        *result = nt::STATUS_NO_MORE_FILES;
        return;
    }
    
    // Write entries to buffer
    GuestAddr ptr = info_buffer;
    u32 bytes_written = 0;
    GuestAddr prev_entry_ptr = 0;
    
    auto file_info_class = static_cast<FileInformationClass>(info_class);
    
    while (enum_state.current_index < enum_state.entries.size()) {
        const DirEntry& entry = enum_state.entries[enum_state.current_index];
        
        // Calculate entry size based on info class
        u32 name_bytes = static_cast<u32>(entry.name.size()) * 2;  // Unicode
        u32 entry_size = 0;
        
        switch (file_info_class) {
            case FileInformationClass::FileDirectoryInformation:
                // FILE_DIRECTORY_INFORMATION: NextEntryOffset(4) + FileIndex(4) + CreationTime(8) + 
                //   LastAccessTime(8) + LastWriteTime(8) + ChangeTime(8) + EndOfFile(8) + 
                //   AllocationSize(8) + FileAttributes(4) + FileNameLength(4) + FileName[]
                entry_size = 64 + name_bytes;
                break;
                
            case FileInformationClass::FileBothDirectoryInformation:
                // Same as above + ShortNameLength(1) + ShortName[24]
                entry_size = 94 + name_bytes;
                break;
                
            case FileInformationClass::FileNamesInformation:
                // NextEntryOffset(4) + FileIndex(4) + FileNameLength(4) + FileName[]
                entry_size = 12 + name_bytes;
                break;
                
            default:
                entry_size = 64 + name_bytes;
                break;
        }
        
        // Align to 8 bytes
        entry_size = (entry_size + 7) & ~7u;
        
        // Check if fits in buffer
        if (bytes_written + entry_size > buffer_length) {
            break;
        }
        
        // Write entry
        u32 attributes = entry.is_directory ? file_attr::FILE_ATTRIBUTE_DIRECTORY : file_attr::FILE_ATTRIBUTE_NORMAL;
        
        switch (file_info_class) {
            case FileInformationClass::FileDirectoryInformation:
            case FileInformationClass::FileBothDirectoryInformation:
                memory->write_u32(ptr + 0, 0);              // NextEntryOffset (set later)
                memory->write_u32(ptr + 4, static_cast<u32>(enum_state.current_index)); // FileIndex
                memory->write_u64(ptr + 8, entry.creation_time);   // CreationTime
                memory->write_u64(ptr + 16, entry.last_write_time); // LastAccessTime
                memory->write_u64(ptr + 24, entry.last_write_time); // LastWriteTime
                memory->write_u64(ptr + 32, entry.last_write_time); // ChangeTime
                memory->write_u64(ptr + 40, entry.size);    // EndOfFile
                memory->write_u64(ptr + 48, entry.size);    // AllocationSize
                memory->write_u32(ptr + 56, attributes);    // FileAttributes
                memory->write_u32(ptr + 60, name_bytes);    // FileNameLength
                write_unicode_string(memory, ptr + 64, entry.name, name_bytes);
                break;
                
            case FileInformationClass::FileNamesInformation:
                memory->write_u32(ptr + 0, 0);              // NextEntryOffset
                memory->write_u32(ptr + 4, static_cast<u32>(enum_state.current_index)); // FileIndex
                memory->write_u32(ptr + 8, name_bytes);     // FileNameLength
                write_unicode_string(memory, ptr + 12, entry.name, name_bytes);
                break;
                
            default:
                break;
        }
        
        // Update previous entry's NextEntryOffset
        if (prev_entry_ptr != 0) {
            memory->write_u32(prev_entry_ptr, ptr - prev_entry_ptr);
        }
        
        prev_entry_ptr = ptr;
        ptr += entry_size;
        bytes_written += entry_size;
        enum_state.current_index++;
        
        if (return_single) {
            break;
        }
    }
    
    if (bytes_written == 0) {
        if (io_status_ptr) {
            memory->write_u32(io_status_ptr, nt::STATUS_NO_MORE_FILES);
            memory->write_u32(io_status_ptr + 4, 0);
        }
        *result = nt::STATUS_NO_MORE_FILES;
        return;
    }
    
    if (io_status_ptr) {
        memory->write_u32(io_status_ptr, nt::STATUS_SUCCESS);
        memory->write_u32(io_status_ptr + 4, bytes_written);
    }
    
    *result = nt::STATUS_SUCCESS;
}

//=============================================================================
// NtQueryFullAttributesFile Implementation
//=============================================================================

static void HLE_NtQueryFullAttributesFile_IO(Cpu* /*cpu*/, Memory* memory, u64* args, u64* result) {
    // NTSTATUS NtQueryFullAttributesFile(
    //   POBJECT_ATTRIBUTES ObjectAttributes,            // arg[0]
    //   PFILE_NETWORK_OPEN_INFORMATION FileInformation  // arg[1]
    // );
    
    GuestAddr obj_attr_ptr = static_cast<GuestAddr>(args[0]);
    GuestAddr info_ptr = static_cast<GuestAddr>(args[1]);
    
    std::string xbox_path = read_object_attributes_path(memory, obj_attr_ptr);
    if (xbox_path.empty()) {
        *result = nt::STATUS_OBJECT_NAME_INVALID;
        return;
    }
    
    std::string vfs_path = translate_xbox_path(xbox_path);
    
    LOGD("NtQueryFullAttributesFile: '%s' -> '%s'", xbox_path.c_str(), vfs_path.c_str());
    
    if (!g_io_state.vfs) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Check if file exists
    if (!g_io_state.vfs->file_exists(vfs_path)) {
        *result = nt::STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    
    // Get file info
    FileInfo info;
    Status status = g_io_state.vfs->get_file_info(vfs_path, info);
    
    if (status == Status::Ok) {
        // FILE_NETWORK_OPEN_INFORMATION structure
        memory->write_u64(info_ptr + 0, info.creation_time);     // CreationTime
        memory->write_u64(info_ptr + 8, info.last_access_time);  // LastAccessTime
        memory->write_u64(info_ptr + 16, info.last_write_time);  // LastWriteTime
        memory->write_u64(info_ptr + 24, info.last_write_time);  // ChangeTime
        memory->write_u64(info_ptr + 32, info.size);             // AllocationSize
        memory->write_u64(info_ptr + 40, info.size);             // EndOfFile
        
        u32 attributes = file_attr::FILE_ATTRIBUTE_NORMAL;
        if (static_cast<u32>(info.attributes) & static_cast<u32>(FileAttributes::Directory)) {
            attributes = file_attr::FILE_ATTRIBUTE_DIRECTORY;
        }
        if (static_cast<u32>(info.attributes) & static_cast<u32>(FileAttributes::ReadOnly)) {
            attributes |= file_attr::FILE_ATTRIBUTE_READONLY;
        }
        memory->write_u32(info_ptr + 48, attributes);  // FileAttributes
        
        *result = nt::STATUS_SUCCESS;
    } else {
        *result = nt::STATUS_OBJECT_NAME_NOT_FOUND;
    }
}

//=============================================================================
// NtClose Implementation
//=============================================================================

static void HLE_NtClose_IO(Cpu* /*cpu*/, Memory* /*memory*/, u64* args, u64* result) {
    // NTSTATUS NtClose(HANDLE Handle);
    
    u32 kernel_handle = static_cast<u32>(args[0]);
    
    if (!g_io_state.vfs) {
        *result = nt::STATUS_UNSUCCESSFUL;
        return;
    }
    
    // Look up and remove VFS handle
    u32 vfs_handle;
    {
        std::lock_guard<std::mutex> lock(g_io_state.handle_mutex);
        auto it = g_io_state.kernel_to_vfs_handles.find(kernel_handle);
        if (it == g_io_state.kernel_to_vfs_handles.end()) {
            // Handle might be for something other than a file
            *result = nt::STATUS_SUCCESS;
            return;
        }
        vfs_handle = it->second;
        g_io_state.kernel_to_vfs_handles.erase(it);
    }
    
    // Close in VFS
    g_io_state.vfs->close_file(vfs_handle);
    
    // Clean up directory enumeration state if any
    {
        std::lock_guard<std::mutex> dir_lock(g_io_state.dir_mutex);
        g_io_state.dir_enum_states.erase(kernel_handle);
    }
    
    LOGD("NtClose: handle=0x%X", kernel_handle);
    *result = nt::STATUS_SUCCESS;
}

//=============================================================================
// Registration
//=============================================================================

void register_file_io_exports(std::unordered_map<u64, HleFunction>& hle_functions, 
                              std::function<u64(u32, u32)> make_import_key) {
    // File I/O functions - ordinals from Xbox 360 xboxkrnl.exe
    // These ordinals are the standard Xbox 360 kernel ordinals
    
    hle_functions[make_import_key(0, 0x77)]  = HLE_NtCreateFile_IO;           // NtCreateFile (119)
    hle_functions[make_import_key(0, 0xDA)]  = HLE_NtReadFile_IO;             // NtReadFile (218)
    hle_functions[make_import_key(0, 0x112)] = HLE_NtWriteFile_IO;            // NtWriteFile (274)
    hle_functions[make_import_key(0, 0xE0)]  = HLE_NtQueryInformationFile_IO; // NtQueryInformationFile (224)
    hle_functions[make_import_key(0, 0xFC)]  = HLE_NtSetInformationFile_IO;   // NtSetInformationFile (252)
    hle_functions[make_import_key(0, 0xDE)]  = HLE_NtQueryDirectoryFile_IO;   // NtQueryDirectoryFile (222)
    hle_functions[make_import_key(0, 0xE1)]  = HLE_NtQueryFullAttributesFile_IO; // NtQueryFullAttributesFile (225)
    hle_functions[make_import_key(0, 0x19)]  = HLE_NtClose_IO;                // NtClose (25)
    
    LOGI("Registered kernel file I/O HLE functions");
}

/**
 * Initialize file I/O state
 */
void init_file_io_state(VirtualFileSystem* vfs) {
    g_io_state.vfs = vfs;
    g_io_state.kernel_to_vfs_handles.clear();
    g_io_state.dir_enum_states.clear();
    g_io_state.next_kernel_handle = 0x1000;
    
    LOGI("File I/O state initialized");
}

/**
 * Shutdown file I/O state
 */
void shutdown_file_io_state() {
    // Close all open handles
    if (g_io_state.vfs) {
        std::lock_guard<std::mutex> lock(g_io_state.handle_mutex);
        for (const auto& [kernel_handle, vfs_handle] : g_io_state.kernel_to_vfs_handles) {
            g_io_state.vfs->close_file(vfs_handle);
        }
    }
    
    g_io_state.kernel_to_vfs_handles.clear();
    g_io_state.dir_enum_states.clear();
    g_io_state.vfs = nullptr;
}

} // namespace x360mu
