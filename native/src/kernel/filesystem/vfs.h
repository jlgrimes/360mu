/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Virtual File System - Xbox 360 path routing and device management
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>

namespace x360mu {

// Forward declarations
class VfsDevice;

// File access flags (Xbox 360 compatible)
enum class FileAccess : u32 {
    Read        = 0x80000000,
    Write       = 0x40000000,
    ReadWrite   = 0xC0000000,
    Append      = 0x00000004,
    Execute     = 0x00000020,
};

// File creation disposition
enum class FileDisposition : u32 {
    Supersede    = 0,  // If exists, replace; else create
    Open         = 1,  // Must exist
    Create       = 2,  // Must not exist
    OpenIf       = 3,  // If exists, open; else create
    Overwrite    = 4,  // Must exist; truncate
    OverwriteIf  = 5,  // If exists, truncate; else create
};

// File attributes
enum class FileAttributes : u32 {
    None        = 0x00000000,
    ReadOnly    = 0x00000001,
    Hidden      = 0x00000002,
    System      = 0x00000004,
    Directory   = 0x00000010,
    Archive     = 0x00000020,
    Normal      = 0x00000080,
};

// Seek origin
enum class SeekOrigin : u32 {
    Begin   = 0,
    Current = 1,
    End     = 2,
};

// Invalid handle constant
constexpr u32 INVALID_FILE_HANDLE = 0xFFFFFFFF;

/**
 * Directory entry information
 */
struct DirEntry {
    std::string name;
    u64 size;
    u64 creation_time;
    u64 last_write_time;
    FileAttributes attributes;
    bool is_directory;
};

/**
 * File information
 */
struct FileInfo {
    u64 size;
    u64 creation_time;
    u64 last_access_time;
    u64 last_write_time;
    FileAttributes attributes;
};

/**
 * Open file handle info
 */
struct FileHandle {
    u32 handle;
    std::string path;
    std::string device_name;
    u32 device_handle;  // Handle from the underlying device
    FileAccess access;
    u64 position;
    bool valid;
};

/**
 * Base class for VFS devices (ISO, STFS, host folders)
 */
class VfsDevice {
public:
    virtual ~VfsDevice() = default;
    
    /**
     * Mount the device
     */
    virtual Status mount(const std::string& source_path) = 0;
    
    /**
     * Unmount the device
     */
    virtual void unmount() = 0;
    
    /**
     * Check if a path exists
     */
    virtual bool exists(const std::string& path) = 0;
    
    /**
     * Check if path is a directory
     */
    virtual bool is_directory(const std::string& path) = 0;
    
    /**
     * Open a file
     * @return Device-specific handle, or INVALID_FILE_HANDLE on error
     */
    virtual u32 open(const std::string& path, FileAccess access, FileDisposition disposition) = 0;
    
    /**
     * Close a file handle
     */
    virtual void close(u32 handle) = 0;
    
    /**
     * Read from file
     * @return Number of bytes read, or -1 on error
     */
    virtual s64 read(u32 handle, void* buffer, u64 size) = 0;
    
    /**
     * Write to file
     * @return Number of bytes written, or -1 on error
     */
    virtual s64 write(u32 handle, const void* buffer, u64 size) = 0;
    
    /**
     * Seek in file
     * @return New position, or -1 on error
     */
    virtual s64 seek(u32 handle, s64 offset, SeekOrigin origin) = 0;
    
    /**
     * Get current position
     */
    virtual u64 tell(u32 handle) = 0;
    
    /**
     * Get file size
     */
    virtual u64 get_file_size(u32 handle) = 0;
    
    /**
     * Get file info
     */
    virtual Status get_file_info(const std::string& path, FileInfo& info) = 0;
    
    /**
     * List directory contents
     */
    virtual Status list_directory(const std::string& path, std::vector<DirEntry>& entries) = 0;
    
    /**
     * Create directory (if supported)
     */
    virtual Status create_directory(const std::string& path) {
        return Status::NotImplemented;
    }
    
    /**
     * Delete file/directory (if supported)
     */
    virtual Status remove(const std::string& path) {
        return Status::NotImplemented;
    }
    
    /**
     * Check if device is read-only
     */
    virtual bool is_read_only() const = 0;
    
    /**
     * Get device name/type
     */
    virtual const std::string& get_type() const = 0;
};

/**
 * Host folder device - maps to actual filesystem
 */
class HostDevice : public VfsDevice {
public:
    HostDevice();
    ~HostDevice() override;
    
    Status mount(const std::string& host_path) override;
    void unmount() override;
    
    bool exists(const std::string& path) override;
    bool is_directory(const std::string& path) override;
    
    u32 open(const std::string& path, FileAccess access, FileDisposition disposition) override;
    void close(u32 handle) override;
    s64 read(u32 handle, void* buffer, u64 size) override;
    s64 write(u32 handle, const void* buffer, u64 size) override;
    s64 seek(u32 handle, s64 offset, SeekOrigin origin) override;
    u64 tell(u32 handle) override;
    u64 get_file_size(u32 handle) override;
    Status get_file_info(const std::string& path, FileInfo& info) override;
    Status list_directory(const std::string& path, std::vector<DirEntry>& entries) override;
    Status create_directory(const std::string& path) override;
    Status remove(const std::string& path) override;
    
    bool is_read_only() const override { return false; }
    const std::string& get_type() const override { return type_; }
    
private:
    std::string host_base_path_;
    std::string type_ = "host";
    bool mounted_ = false;
    
    struct OpenHostFile {
        FILE* file;
        std::string path;
    };
    
    std::unordered_map<u32, OpenHostFile> open_files_;
    u32 next_handle_ = 1;
    std::mutex mutex_;
    
    std::string resolve_path(const std::string& path);
};

/**
 * Virtual File System - manages device mounts and path routing
 */
class VirtualFileSystem {
public:
    VirtualFileSystem();
    ~VirtualFileSystem();
    
    /**
     * Initialize the VFS
     */
    Status initialize(const std::string& data_path, const std::string& save_path);
    
    /**
     * Shutdown and unmount all devices
     */
    void shutdown();
    
    /**
     * Mount an ISO disc image
     */
    Status mount_iso(const std::string& mount_point, const std::string& iso_path);
    
    /**
     * Mount an STFS package (LIVE/PIRS/CON)
     */
    Status mount_stfs(const std::string& mount_point, const std::string& stfs_path);
    
    /**
     * Mount a host folder
     */
    Status mount_folder(const std::string& mount_point, const std::string& host_path);
    
    /**
     * Unmount a device
     */
    void unmount(const std::string& mount_point);
    
    /**
     * Unmount all devices
     */
    void unmount_all();
    
    /**
     * Translate Xbox path to device + relative path
     */
    std::string translate_path(const std::string& xbox_path);
    
    /**
     * Open a file
     */
    Status open_file(const std::string& path, FileAccess access, u32& handle_out);
    
    /**
     * Open a file with disposition
     */
    Status open_file(const std::string& path, FileAccess access, 
                     FileDisposition disposition, u32& handle_out);
    
    /**
     * Close a file
     */
    Status close_file(u32 handle);
    
    /**
     * Read from file
     */
    Status read_file(u32 handle, void* buffer, u64 size, u64& bytes_read);
    
    /**
     * Write to file
     */
    Status write_file(u32 handle, const void* buffer, u64 size, u64& bytes_written);
    
    /**
     * Seek in file
     */
    Status seek_file(u32 handle, s64 offset, SeekOrigin origin, u64& new_position);
    
    /**
     * Get file size
     */
    Status get_file_size(u32 handle, u64& size_out);
    
    /**
     * Get file position
     */
    Status get_file_position(u32 handle, u64& position_out);
    
    /**
     * Check if file exists
     */
    bool file_exists(const std::string& path);
    
    /**
     * Get file info
     */
    Status get_file_info(const std::string& path, FileInfo& info);
    
    /**
     * List directory contents
     */
    Status query_directory(const std::string& path, std::vector<DirEntry>& entries);
    
    /**
     * Create directory
     */
    Status create_directory(const std::string& path);
    
    /**
     * Get data path (read-only game data)
     */
    const std::string& get_data_path() const { return data_path_; }
    
    /**
     * Get save path (writable save data)
     */
    const std::string& get_save_path() const { return save_path_; }
    
private:
    struct Mount {
        std::string mount_point;
        std::unique_ptr<VfsDevice> device;
    };
    
    std::vector<Mount> mounts_;
    std::unordered_map<u32, FileHandle> open_files_;
    u32 next_handle_ = 1;
    
    std::string data_path_;
    std::string save_path_;
    
    std::mutex mutex_;
    bool initialized_ = false;
    
    /**
     * Find device for path
     */
    VfsDevice* find_device(const std::string& path, std::string& relative_path);
    
    /**
     * Normalize path (handle backslashes, remove leading slash)
     */
    std::string normalize_path(const std::string& path);
    
    /**
     * Parse device name from path (e.g., "game:" from "game:\\file.xex")
     */
    bool parse_device(const std::string& path, std::string& device, std::string& relative);
};

} // namespace x360mu
