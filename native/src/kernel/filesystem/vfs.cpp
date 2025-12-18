/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Virtual File System implementation
 */

#include "vfs.h"
#include "iso_device.h"
#include "stfs_device.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "x360mu-vfs"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf(__VA_ARGS__); printf("\n")
#define LOGD(...) printf(__VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

// ============================================================================
// HostDevice Implementation
// ============================================================================

HostDevice::HostDevice() = default;

HostDevice::~HostDevice() {
    unmount();
}

Status HostDevice::mount(const std::string& host_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (mounted_) {
        LOGE("HostDevice: Already mounted");
        return Status::Error;
    }
    
    // Verify path exists and is a directory
    struct stat st;
    if (stat(host_path.c_str(), &st) != 0) {
        LOGE("HostDevice: Path does not exist: %s", host_path.c_str());
        return Status::NotFound;
    }
    
    if (!S_ISDIR(st.st_mode)) {
        LOGE("HostDevice: Path is not a directory: %s", host_path.c_str());
        return Status::InvalidArgument;
    }
    
    host_base_path_ = host_path;
    // Ensure trailing slash
    if (!host_base_path_.empty() && host_base_path_.back() != '/') {
        host_base_path_ += '/';
    }
    
    mounted_ = true;
    LOGI("HostDevice: Mounted %s", host_base_path_.c_str());
    return Status::Ok;
}

void HostDevice::unmount() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Close all open files
    for (auto& [handle, file] : open_files_) {
        if (file.file) {
            fclose(file.file);
        }
    }
    open_files_.clear();
    
    host_base_path_.clear();
    mounted_ = false;
}

std::string HostDevice::resolve_path(const std::string& path) {
    std::string resolved = host_base_path_;
    
    // Remove leading slash/backslash
    size_t start = 0;
    while (start < path.size() && (path[start] == '/' || path[start] == '\\')) {
        start++;
    }
    
    // Convert backslashes to forward slashes
    for (size_t i = start; i < path.size(); i++) {
        if (path[i] == '\\') {
            resolved += '/';
        } else {
            resolved += path[i];
        }
    }
    
    return resolved;
}

bool HostDevice::exists(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return false;
    
    std::string full_path = resolve_path(path);
    struct stat st;
    return stat(full_path.c_str(), &st) == 0;
}

bool HostDevice::is_directory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return false;
    
    std::string full_path = resolve_path(path);
    struct stat st;
    if (stat(full_path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

u32 HostDevice::open(const std::string& path, FileAccess access, FileDisposition disposition) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return INVALID_FILE_HANDLE;
    
    std::string full_path = resolve_path(path);
    
    // Determine fopen mode based on access and disposition
    const char* mode = "rb";
    bool must_exist = false;
    bool must_not_exist = false;
    bool truncate = false;
    
    switch (disposition) {
        case FileDisposition::Open:
            must_exist = true;
            break;
        case FileDisposition::Create:
            must_not_exist = true;
            break;
        case FileDisposition::OpenIf:
            // Open if exists, create otherwise
            break;
        case FileDisposition::Overwrite:
            must_exist = true;
            truncate = true;
            break;
        case FileDisposition::OverwriteIf:
        case FileDisposition::Supersede:
            truncate = true;
            break;
    }
    
    // Check existence requirements
    struct stat st;
    bool file_exists = (stat(full_path.c_str(), &st) == 0);
    
    if (must_exist && !file_exists) {
        return INVALID_FILE_HANDLE;
    }
    if (must_not_exist && file_exists) {
        return INVALID_FILE_HANDLE;
    }
    
    // Determine mode string
    u32 access_val = static_cast<u32>(access);
    bool read = (access_val & 0x80000000) != 0;
    bool write = (access_val & 0x40000000) != 0;
    
    if (write && truncate) {
        mode = read ? "w+b" : "wb";
    } else if (write && !file_exists) {
        mode = read ? "w+b" : "wb";
    } else if (write) {
        mode = read ? "r+b" : "r+b";  // Need read for positioning
    } else {
        mode = "rb";
    }
    
    FILE* file = fopen(full_path.c_str(), mode);
    if (!file) {
        return INVALID_FILE_HANDLE;
    }
    
    u32 handle = next_handle_++;
    OpenHostFile& of = open_files_[handle];
    of.file = file;
    of.path = full_path;
    
    return handle;
}

void HostDevice::close(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it != open_files_.end()) {
        if (it->second.file) {
            fclose(it->second.file);
        }
        open_files_.erase(it);
    }
}

s64 HostDevice::read(u32 handle, void* buffer, u64 size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) return -1;
    
    return static_cast<s64>(fread(buffer, 1, size, it->second.file));
}

s64 HostDevice::write(u32 handle, const void* buffer, u64 size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) return -1;
    
    return static_cast<s64>(fwrite(buffer, 1, size, it->second.file));
}

s64 HostDevice::seek(u32 handle, s64 offset, SeekOrigin origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) return -1;
    
    int whence;
    switch (origin) {
        case SeekOrigin::Begin:   whence = SEEK_SET; break;
        case SeekOrigin::Current: whence = SEEK_CUR; break;
        case SeekOrigin::End:     whence = SEEK_END; break;
        default: return -1;
    }
    
    if (fseeko(it->second.file, offset, whence) != 0) {
        return -1;
    }
    
    return static_cast<s64>(ftello(it->second.file));
}

u64 HostDevice::tell(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) return 0;
    
    return static_cast<u64>(ftello(it->second.file));
}

u64 HostDevice::get_file_size(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) return 0;
    
    // Save position
    long pos = ftell(it->second.file);
    
    // Seek to end
    fseek(it->second.file, 0, SEEK_END);
    u64 size = static_cast<u64>(ftell(it->second.file));
    
    // Restore position
    fseek(it->second.file, pos, SEEK_SET);
    
    return size;
}

Status HostDevice::get_file_info(const std::string& path, FileInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return Status::Error;
    
    std::string full_path = resolve_path(path);
    struct stat st;
    if (stat(full_path.c_str(), &st) != 0) {
        return Status::NotFound;
    }
    
    info.size = static_cast<u64>(st.st_size);
    info.creation_time = static_cast<u64>(st.st_ctime);
    info.last_access_time = static_cast<u64>(st.st_atime);
    info.last_write_time = static_cast<u64>(st.st_mtime);
    
    info.attributes = FileAttributes::None;
    if (S_ISDIR(st.st_mode)) {
        info.attributes = FileAttributes::Directory;
    } else {
        info.attributes = FileAttributes::Normal;
    }
    if (!(st.st_mode & S_IWUSR)) {
        info.attributes = static_cast<FileAttributes>(
            static_cast<u32>(info.attributes) | static_cast<u32>(FileAttributes::ReadOnly));
    }
    
    return Status::Ok;
}

Status HostDevice::list_directory(const std::string& path, std::vector<DirEntry>& entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return Status::Error;
    
    std::string full_path = resolve_path(path);
    
    DIR* dir = opendir(full_path.c_str());
    if (!dir) {
        return Status::NotFound;
    }
    
    entries.clear();
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        DirEntry entry;
        entry.name = ent->d_name;
        
        // Get full stat info
        std::string entry_path = full_path;
        if (!entry_path.empty() && entry_path.back() != '/') {
            entry_path += '/';
        }
        entry_path += ent->d_name;
        
        struct stat st;
        if (stat(entry_path.c_str(), &st) == 0) {
            entry.size = static_cast<u64>(st.st_size);
            entry.creation_time = static_cast<u64>(st.st_ctime);
            entry.last_write_time = static_cast<u64>(st.st_mtime);
            entry.is_directory = S_ISDIR(st.st_mode);
            entry.attributes = entry.is_directory ? FileAttributes::Directory : FileAttributes::Normal;
        } else {
            entry.size = 0;
            entry.creation_time = 0;
            entry.last_write_time = 0;
            entry.is_directory = (ent->d_type == DT_DIR);
            entry.attributes = entry.is_directory ? FileAttributes::Directory : FileAttributes::Normal;
        }
        
        entries.push_back(std::move(entry));
    }
    
    closedir(dir);
    return Status::Ok;
}

Status HostDevice::create_directory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return Status::Error;
    
    std::string full_path = resolve_path(path);
    
    // Create directory with standard permissions
    if (mkdir(full_path.c_str(), 0755) != 0) {
        if (errno == EEXIST) {
            return Status::Ok;  // Already exists
        }
        return Status::IoError;
    }
    
    return Status::Ok;
}

Status HostDevice::remove(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return Status::Error;
    
    std::string full_path = resolve_path(path);
    
    struct stat st;
    if (stat(full_path.c_str(), &st) != 0) {
        return Status::NotFound;
    }
    
    if (S_ISDIR(st.st_mode)) {
        if (rmdir(full_path.c_str()) != 0) {
            return Status::IoError;
        }
    } else {
        if (unlink(full_path.c_str()) != 0) {
            return Status::IoError;
        }
    }
    
    return Status::Ok;
}

// ============================================================================
// VirtualFileSystem Implementation
// ============================================================================

VirtualFileSystem::VirtualFileSystem() = default;

VirtualFileSystem::~VirtualFileSystem() {
    shutdown();
}

Status VirtualFileSystem::initialize(const std::string& data_path, const std::string& save_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_) {
        return Status::Error;
    }
    
    data_path_ = data_path;
    save_path_ = save_path;
    
    // Create default mount points for common Xbox paths
    // Mount cache: to temp storage
    auto cache_device = std::make_unique<HostDevice>();
    std::string cache_path = save_path_ + "/cache";
    
    // Create cache directory if it doesn't exist
    mkdir(cache_path.c_str(), 0755);
    
    if (cache_device->mount(cache_path) == Status::Ok) {
        Mount mount;
        mount.mount_point = "cache:";
        mount.device = std::move(cache_device);
        mounts_.push_back(std::move(mount));
    }
    
    // Mount hdd: to save storage
    auto hdd_device = std::make_unique<HostDevice>();
    std::string hdd_path = save_path_ + "/hdd";
    mkdir(hdd_path.c_str(), 0755);
    
    if (hdd_device->mount(hdd_path) == Status::Ok) {
        Mount mount;
        mount.mount_point = "hdd:";
        mount.device = std::move(hdd_device);
        mounts_.push_back(std::move(mount));
    }
    
    // Mount title: to save storage (per-game save data)
    auto title_device = std::make_unique<HostDevice>();
    std::string title_path = save_path_ + "/title";
    mkdir(title_path.c_str(), 0755);
    
    if (title_device->mount(title_path) == Status::Ok) {
        Mount mount;
        mount.mount_point = "title:";
        mount.device = std::move(title_device);
        mounts_.push_back(std::move(mount));
    }
    
    initialized_ = true;
    LOGI("VFS initialized: data=%s save=%s", data_path_.c_str(), save_path_.c_str());
    
    return Status::Ok;
}

void VirtualFileSystem::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Close all open files
    open_files_.clear();
    
    // Unmount all devices
    for (auto& mount : mounts_) {
        mount.device->unmount();
    }
    mounts_.clear();
    
    initialized_ = false;
}

Status VirtualFileSystem::mount_iso(const std::string& mount_point, const std::string& iso_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Remove existing mount at this point
    auto it = std::remove_if(mounts_.begin(), mounts_.end(),
        [&mount_point](const Mount& m) { return m.mount_point == mount_point; });
    mounts_.erase(it, mounts_.end());
    
    auto device = std::make_unique<IsoDevice>();
    Status status = device->mount(iso_path);
    if (status != Status::Ok) {
        LOGE("Failed to mount ISO %s: %s", iso_path.c_str(), status_to_string(status));
        return status;
    }
    
    Mount mount;
    mount.mount_point = mount_point;
    mount.device = std::move(device);
    mounts_.push_back(std::move(mount));
    
    LOGI("Mounted ISO %s at %s", iso_path.c_str(), mount_point.c_str());
    return Status::Ok;
}

Status VirtualFileSystem::mount_stfs(const std::string& mount_point, const std::string& stfs_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Remove existing mount at this point
    auto it = std::remove_if(mounts_.begin(), mounts_.end(),
        [&mount_point](const Mount& m) { return m.mount_point == mount_point; });
    mounts_.erase(it, mounts_.end());
    
    auto device = std::make_unique<StfsDevice>();
    Status status = device->mount(stfs_path);
    if (status != Status::Ok) {
        LOGE("Failed to mount STFS %s: %s", stfs_path.c_str(), status_to_string(status));
        return status;
    }
    
    Mount mount;
    mount.mount_point = mount_point;
    mount.device = std::move(device);
    mounts_.push_back(std::move(mount));
    
    LOGI("Mounted STFS %s at %s", stfs_path.c_str(), mount_point.c_str());
    return Status::Ok;
}

Status VirtualFileSystem::mount_folder(const std::string& mount_point, const std::string& host_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Remove existing mount at this point
    auto it = std::remove_if(mounts_.begin(), mounts_.end(),
        [&mount_point](const Mount& m) { return m.mount_point == mount_point; });
    mounts_.erase(it, mounts_.end());
    
    auto device = std::make_unique<HostDevice>();
    Status status = device->mount(host_path);
    if (status != Status::Ok) {
        LOGE("Failed to mount folder %s: %s", host_path.c_str(), status_to_string(status));
        return status;
    }
    
    Mount mount;
    mount.mount_point = mount_point;
    mount.device = std::move(device);
    mounts_.push_back(std::move(mount));
    
    LOGI("Mounted folder %s at %s", host_path.c_str(), mount_point.c_str());
    return Status::Ok;
}

void VirtualFileSystem::unmount(const std::string& mount_point) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = std::remove_if(mounts_.begin(), mounts_.end(),
        [&mount_point](const Mount& m) { return m.mount_point == mount_point; });
    
    if (it != mounts_.end()) {
        for (auto i = it; i != mounts_.end(); ++i) {
            i->device->unmount();
        }
        mounts_.erase(it, mounts_.end());
    }
}

void VirtualFileSystem::unmount_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& mount : mounts_) {
        mount.device->unmount();
    }
    mounts_.clear();
}

std::string VirtualFileSystem::normalize_path(const std::string& path) {
    std::string result;
    result.reserve(path.size());
    
    for (char c : path) {
        if (c == '\\') {
            result += '/';
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    
    return result;
}

bool VirtualFileSystem::parse_device(const std::string& path, std::string& device, std::string& relative) {
    // Handle NT-style paths: \Device\Cdrom0\path or \\Device\Cdrom0\path
    if (path.size() > 1 && (path[0] == '\\' || path[0] == '/')) {
        std::string lower_path = path;
        for (auto& c : lower_path) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        
        // Check for \device\ prefix
        size_t device_start = 0;
        if (lower_path.find("\\device\\") == 0 || lower_path.find("/device/") == 0) {
            device_start = 8; // Skip "\device\"
        } else if (lower_path.find("\\\\device\\") == 0 || lower_path.find("//device/") == 0) {
            device_start = 9; // Skip "\\device\"
        }
        
        if (device_start > 0) {
            // Find the end of device name (next slash)
            size_t device_end = path.find_first_of("\\/", device_start);
            if (device_end == std::string::npos) {
                // Device name only, no file
                device = path.substr(device_start);
                relative = "";
            } else {
                device = path.substr(device_start, device_end - device_start);
                relative = path.substr(device_end + 1);
            }
            
            // Convert device name to lowercase
            for (auto& c : device) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            
            // Normalize slashes in relative path
            for (auto& c : relative) {
                if (c == '\\') {
                    c = '/';
                }
            }
            
            LOGD("VFS parse_device (NT-style): path='%s' -> device='%s', relative='%s'", 
                 path.c_str(), device.c_str(), relative.c_str());
            return true;
        }
    }
    
    // Handle DOS-style paths: Device:\path
    size_t colon = path.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    
    device = path.substr(0, colon);
    
    // Convert device name to lowercase
    for (auto& c : device) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    
    // Get relative path (after device:)
    size_t start = colon + 1;
    
    // Skip leading slashes/backslashes
    while (start < path.size() && (path[start] == '/' || path[start] == '\\')) {
        start++;
    }
    
    relative = path.substr(start);
    
    // Normalize slashes in relative path
    for (auto& c : relative) {
        if (c == '\\') {
            c = '/';
        }
    }
    
    LOGD("VFS parse_device (DOS-style): path='%s' -> device='%s', relative='%s'", 
         path.c_str(), device.c_str(), relative.c_str());
    return true;
}

VfsDevice* VirtualFileSystem::find_device(const std::string& path, std::string& relative_path) {
    std::string device_name;
    if (!parse_device(path, device_name, relative_path)) {
        LOGD("VFS find_device: parse_device failed for '%s'", path.c_str());
        return nullptr;
    }
    
    // Find matching mount
    for (auto& mount : mounts_) {
        // Extract just the device name from mount point for comparison
        // Mount point can be: \Device\Cdrom0, Cdrom0:, etc.
        std::string mount_device;
        std::string mount_unused;
        
        if (parse_device(mount.mount_point, mount_device, mount_unused)) {
            // Both parsed - compare device names
            if (mount_device == device_name) {
                LOGD("VFS find_device: matched '%s' to mount '%s'", device_name.c_str(), mount.mount_point.c_str());
                return mount.device.get();
            }
        } else {
            // Mount point doesn't parse - try direct comparison
            std::string mount_lower = mount.mount_point;
            for (auto& c : mount_lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            
            // Try comparing with and without prefix
            if (mount_lower == device_name || 
                mount_lower == "\\device\\" + device_name ||
                mount_lower == "/device/" + device_name) {
                LOGD("VFS find_device: matched '%s' to mount '%s' (direct)", device_name.c_str(), mount.mount_point.c_str());
                return mount.device.get();
            }
        }
    }
    
    LOGD("VFS find_device: no match for device '%s'", device_name.c_str());
    return nullptr;
}

std::string VirtualFileSystem::translate_path(const std::string& xbox_path) {
    std::string device, relative;
    if (!parse_device(xbox_path, device, relative)) {
        return "";
    }
    return device + relative;
}

Status VirtualFileSystem::open_file(const std::string& path, FileAccess access, u32& handle_out) {
    return open_file(path, access, FileDisposition::Open, handle_out);
}

Status VirtualFileSystem::open_file(const std::string& path, FileAccess access, 
                                    FileDisposition disposition, u32& handle_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    handle_out = INVALID_FILE_HANDLE;
    
    std::string relative_path;
    VfsDevice* device = find_device(path, relative_path);
    if (!device) {
        LOGE("VFS: No device found for path: %s", path.c_str());
        return Status::NotFound;
    }
    
    // Check if trying to write to read-only device
    u32 access_val = static_cast<u32>(access);
    bool wants_write = (access_val & 0x40000000) != 0;
    if (wants_write && device->is_read_only()) {
        LOGE("VFS: Cannot write to read-only device: %s", path.c_str());
        return Status::InvalidArgument;
    }
    
    u32 device_handle = device->open(relative_path, access, disposition);
    if (device_handle == INVALID_FILE_HANDLE) {
        return Status::NotFound;
    }
    
    // Create VFS handle
    u32 vfs_handle = next_handle_++;
    FileHandle& fh = open_files_[vfs_handle];
    fh.handle = vfs_handle;
    fh.path = path;
    fh.device_handle = device_handle;
    fh.access = access;
    fh.position = 0;
    fh.valid = true;
    
    // Parse device name
    std::string device_name, unused;
    parse_device(path, device_name, unused);
    fh.device_name = device_name;
    
    handle_out = vfs_handle;
    return Status::Ok;
}

Status VirtualFileSystem::close_file(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::InvalidArgument;
    }
    
    // Find device and close
    std::string relative;
    VfsDevice* device = find_device(it->second.path, relative);
    if (device) {
        device->close(it->second.device_handle);
    }
    
    open_files_.erase(it);
    return Status::Ok;
}

Status VirtualFileSystem::read_file(u32 handle, void* buffer, u64 size, u64& bytes_read) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    bytes_read = 0;
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::InvalidArgument;
    }
    
    std::string relative;
    VfsDevice* device = find_device(it->second.path, relative);
    if (!device) {
        return Status::Error;
    }
    
    s64 result = device->read(it->second.device_handle, buffer, size);
    if (result < 0) {
        return Status::IoError;
    }
    
    bytes_read = static_cast<u64>(result);
    it->second.position = device->tell(it->second.device_handle);
    
    return Status::Ok;
}

Status VirtualFileSystem::write_file(u32 handle, const void* buffer, u64 size, u64& bytes_written) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    bytes_written = 0;
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::InvalidArgument;
    }
    
    std::string relative;
    VfsDevice* device = find_device(it->second.path, relative);
    if (!device) {
        return Status::Error;
    }
    
    if (device->is_read_only()) {
        return Status::InvalidArgument;
    }
    
    s64 result = device->write(it->second.device_handle, buffer, size);
    if (result < 0) {
        return Status::IoError;
    }
    
    bytes_written = static_cast<u64>(result);
    it->second.position = device->tell(it->second.device_handle);
    
    return Status::Ok;
}

Status VirtualFileSystem::seek_file(u32 handle, s64 offset, SeekOrigin origin, u64& new_position) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    new_position = 0;
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::InvalidArgument;
    }
    
    std::string relative;
    VfsDevice* device = find_device(it->second.path, relative);
    if (!device) {
        return Status::Error;
    }
    
    s64 result = device->seek(it->second.device_handle, offset, origin);
    if (result < 0) {
        return Status::IoError;
    }
    
    new_position = static_cast<u64>(result);
    it->second.position = new_position;
    
    return Status::Ok;
}

Status VirtualFileSystem::get_file_size(u32 handle, u64& size_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_out = 0;
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::InvalidArgument;
    }
    
    std::string relative;
    VfsDevice* device = find_device(it->second.path, relative);
    if (!device) {
        return Status::Error;
    }
    
    size_out = device->get_file_size(it->second.device_handle);
    return Status::Ok;
}

Status VirtualFileSystem::get_file_position(u32 handle, u64& position_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    position_out = 0;
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return Status::InvalidArgument;
    }
    
    position_out = it->second.position;
    return Status::Ok;
}

bool VirtualFileSystem::file_exists(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string relative;
    VfsDevice* device = find_device(path, relative);
    if (!device) {
        return false;
    }
    
    return device->exists(relative);
}

Status VirtualFileSystem::get_file_info(const std::string& path, FileInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string relative;
    VfsDevice* device = find_device(path, relative);
    if (!device) {
        return Status::NotFound;
    }
    
    return device->get_file_info(relative, info);
}

Status VirtualFileSystem::query_directory(const std::string& path, std::vector<DirEntry>& entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Handle wildcard patterns like "game:\\*"
    std::string clean_path = path;
    size_t wildcard_pos = path.find('*');
    if (wildcard_pos != std::string::npos) {
        clean_path = path.substr(0, wildcard_pos);
        // Remove trailing backslash
        while (!clean_path.empty() && (clean_path.back() == '\\' || clean_path.back() == '/')) {
            clean_path.pop_back();
        }
    }
    
    std::string relative;
    VfsDevice* device = find_device(clean_path.empty() ? path : clean_path, relative);
    if (!device) {
        return Status::NotFound;
    }
    
    return device->list_directory(relative, entries);
}

Status VirtualFileSystem::create_directory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string relative;
    VfsDevice* device = find_device(path, relative);
    if (!device) {
        return Status::NotFound;
    }
    
    if (device->is_read_only()) {
        return Status::InvalidArgument;
    }
    
    return device->create_directory(relative);
}

} // namespace x360mu
