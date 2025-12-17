/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * ISO 9660 Device implementation
 */

#include "iso_device.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "x360mu-iso"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf(__VA_ARGS__); printf("\n")
#define LOGD(...) 
#define LOGE(...) fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n")
#endif

namespace x360mu {

IsoDevice::IsoDevice() = default;

IsoDevice::~IsoDevice() {
    unmount();
}

Status IsoDevice::mount(const std::string& iso_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (mounted_) {
        LOGE("IsoDevice: Already mounted");
        return Status::Error;
    }
    
    iso_file_ = fopen(iso_path.c_str(), "rb");
    if (!iso_file_) {
        LOGE("IsoDevice: Failed to open ISO file: %s", iso_path.c_str());
        return Status::NotFound;
    }
    
    iso_path_ = iso_path;
    
    // Read primary volume descriptor
    Status status = read_pvd();
    if (status != Status::Ok) {
        fclose(iso_file_);
        iso_file_ = nullptr;
        return status;
    }
    
    // Parse root directory
    status = parse_directory(root_dir_lba_, root_dir_size_, "");
    if (status != Status::Ok) {
        LOGE("IsoDevice: Failed to parse root directory");
        fclose(iso_file_);
        iso_file_ = nullptr;
        return status;
    }
    
    mounted_ = true;
    LOGI("IsoDevice: Mounted %s (Volume: %s)", iso_path.c_str(), volume_id_.c_str());
    
    return Status::Ok;
}

void IsoDevice::unmount() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Close all open files
    open_files_.clear();
    
    // Clear caches
    file_cache_.clear();
    dir_cache_.clear();
    
    if (iso_file_) {
        fclose(iso_file_);
        iso_file_ = nullptr;
    }
    
    mounted_ = false;
}

Status IsoDevice::read_pvd() {
    u8 sector[SECTOR_SIZE];
    
    // Volume descriptors start at sector 16
    // Try sectors 16-31 to find PVD
    for (u32 sec = PVD_SECTOR; sec < PVD_SECTOR + 16; sec++) {
        Status status = read_sector(sec, sector);
        if (status != Status::Ok) {
            return status;
        }
        
        // Check for volume descriptor identifier "CD001"
        if (memcmp(sector + 1, "CD001", 5) != 0) {
            continue;
        }
        
        u8 type = sector[0];
        
        if (type == 0xFF) {
            // Volume descriptor set terminator
            break;
        }
        
        if (type == 1) {
            // Primary Volume Descriptor found
            IsoPrimaryVolumeDescriptor* pvd = reinterpret_cast<IsoPrimaryVolumeDescriptor*>(sector);
            
            // Extract volume space size (little-endian)
            volume_space_size_ = pvd->volume_space_size_le;
            
            // Extract logical block size (should be 2048)
            logical_block_size_ = pvd->logical_block_size_le;
            if (logical_block_size_ != SECTOR_SIZE) {
                LOGE("IsoDevice: Unsupported block size: %u", logical_block_size_);
                return Status::InvalidFormat;
            }
            
            // Extract volume ID (trim trailing spaces)
            char vol_id[33];
            memcpy(vol_id, pvd->system_id + 32, 32);  // volume_id is after system_id
            // Actually volume_id is at offset 40 in the PVD
            memcpy(vol_id, sector + 40, 32);
            vol_id[32] = '\0';
            
            // Trim trailing spaces
            for (int i = 31; i >= 0; i--) {
                if (vol_id[i] == ' ' || vol_id[i] == '\0') {
                    vol_id[i] = '\0';
                } else {
                    break;
                }
            }
            volume_id_ = vol_id;
            
            // Parse root directory record (at offset 156 in PVD)
            const u8* root_record = sector + 156;
            IsoDirectoryRecord* root_dir = reinterpret_cast<IsoDirectoryRecord*>(const_cast<u8*>(root_record));
            
            root_dir_lba_ = root_dir->extent_lba_le;
            root_dir_size_ = root_dir->data_length_le;
            
            LOGI("IsoDevice: PVD found - Volume: %s, Size: %u sectors, Root LBA: %u",
                 volume_id_.c_str(), volume_space_size_, root_dir_lba_);
            
            return Status::Ok;
        }
    }
    
    LOGE("IsoDevice: No primary volume descriptor found");
    return Status::InvalidFormat;
}

Status IsoDevice::read_sector(u32 lba, void* buffer) {
    if (!iso_file_) {
        return Status::Error;
    }
    
    u64 offset = static_cast<u64>(lba) * SECTOR_SIZE;
    if (fseeko(iso_file_, offset, SEEK_SET) != 0) {
        return Status::IoError;
    }
    
    if (fread(buffer, 1, SECTOR_SIZE, iso_file_) != SECTOR_SIZE) {
        return Status::IoError;
    }
    
    return Status::Ok;
}

Status IsoDevice::read_sectors(u32 lba, u32 count, void* buffer) {
    if (!iso_file_) {
        return Status::Error;
    }
    
    u64 offset = static_cast<u64>(lba) * SECTOR_SIZE;
    u64 size = static_cast<u64>(count) * SECTOR_SIZE;
    
    if (fseeko(iso_file_, offset, SEEK_SET) != 0) {
        return Status::IoError;
    }
    
    if (fread(buffer, 1, size, iso_file_) != size) {
        return Status::IoError;
    }
    
    return Status::Ok;
}

Status IsoDevice::read_bytes(u64 offset, u64 size, void* buffer) {
    if (!iso_file_) {
        return Status::Error;
    }
    
    if (fseeko(iso_file_, offset, SEEK_SET) != 0) {
        return Status::IoError;
    }
    
    size_t read = fread(buffer, 1, size, iso_file_);
    if (read != size) {
        // Partial read is okay at end of file
        if (read == 0) {
            return Status::IoError;
        }
    }
    
    return Status::Ok;
}

u64 IsoDevice::iso_date_to_timestamp(const u8* date) {
    // ISO 9660 directory recording date format:
    // [0] = years since 1900
    // [1] = month (1-12)
    // [2] = day (1-31)
    // [3] = hour (0-23)
    // [4] = minute (0-59)
    // [5] = second (0-59)
    // [6] = timezone offset from GMT in 15-minute intervals
    
    struct tm tm = {};
    tm.tm_year = date[0];  // Years since 1900
    tm.tm_mon = date[1] - 1;  // Month (0-11)
    tm.tm_mday = date[2];
    tm.tm_hour = date[3];
    tm.tm_min = date[4];
    tm.tm_sec = date[5];
    
    time_t time = mktime(&tm);
    
    // Apply timezone offset (15 minute intervals, signed)
    s8 tz_offset = static_cast<s8>(date[6]);
    time -= tz_offset * 15 * 60;
    
    return static_cast<u64>(time);
}

bool IsoDevice::parse_directory_record(const u8* data, IsoCachedEntry& entry) {
    const IsoDirectoryRecord* record = reinterpret_cast<const IsoDirectoryRecord*>(data);
    
    if (record->length == 0) {
        return false;
    }
    
    entry.lba = record->extent_lba_le;
    entry.size = record->data_length_le;
    entry.is_directory = (record->flags & static_cast<u8>(IsoFileFlags::Directory)) != 0;
    entry.creation_time = iso_date_to_timestamp(record->recording_date);
    
    // Extract name
    u8 name_length = record->name_length;
    const char* name_ptr = reinterpret_cast<const char*>(data + sizeof(IsoDirectoryRecord));
    
    // Handle special directory entries
    if (name_length == 1) {
        if (name_ptr[0] == 0x00) {
            entry.name = ".";
            return true;
        } else if (name_ptr[0] == 0x01) {
            entry.name = "..";
            return true;
        }
    }
    
    // Copy name and process
    std::string name(name_ptr, name_length);
    
    // ISO 9660 names may have version suffix (;1) - remove it
    size_t semicolon = name.find(';');
    if (semicolon != std::string::npos) {
        name = name.substr(0, semicolon);
    }
    
    // Remove trailing dots (some ISOs have files like "FILE.")
    while (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    
    entry.name = name;
    return true;
}

Status IsoDevice::parse_directory(u32 lba, u32 size, const std::string& parent_path) {
    if (size == 0) {
        return Status::Ok;
    }
    
    // Calculate number of sectors
    u32 sector_count = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    
    // Read directory data
    std::vector<u8> dir_data(sector_count * SECTOR_SIZE);
    Status status = read_sectors(lba, sector_count, dir_data.data());
    if (status != Status::Ok) {
        return status;
    }
    
    std::vector<IsoCachedEntry> entries;
    
    // Parse directory records
    u32 offset = 0;
    while (offset < size) {
        // Check for sector boundary padding
        if (dir_data[offset] == 0) {
            // Skip to next sector
            u32 next_sector = ((offset / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            if (next_sector >= dir_data.size()) {
                break;
            }
            offset = next_sector;
            continue;
        }
        
        IsoCachedEntry entry;
        if (!parse_directory_record(dir_data.data() + offset, entry)) {
            break;
        }
        
        // Skip . and .. entries
        if (entry.name != "." && entry.name != "..") {
            // Build full path
            if (parent_path.empty()) {
                entry.full_path = entry.name;
            } else {
                entry.full_path = parent_path + "/" + entry.name;
            }
            
            // Normalize for lookup (lowercase)
            std::string lookup_path = normalize_path(entry.full_path);
            
            // Add to caches
            file_cache_[lookup_path] = entry;
            entries.push_back(entry);
            
            LOGD("IsoDevice: Found %s: %s (LBA: %u, Size: %u)",
                 entry.is_directory ? "DIR" : "FILE",
                 entry.full_path.c_str(), entry.lba, entry.size);
        }
        
        // Move to next record
        const IsoDirectoryRecord* record = reinterpret_cast<const IsoDirectoryRecord*>(dir_data.data() + offset);
        offset += record->length;
    }
    
    // Cache directory listing
    std::string dir_path = normalize_path(parent_path.empty() ? "/" : parent_path);
    dir_cache_[dir_path] = entries;
    
    // Recursively parse subdirectories
    for (const auto& entry : entries) {
        if (entry.is_directory) {
            status = parse_directory(entry.lba, entry.size, entry.full_path);
            if (status != Status::Ok) {
                LOGE("IsoDevice: Failed to parse subdirectory: %s", entry.full_path.c_str());
                // Continue anyway - don't fail entire mount
            }
        }
    }
    
    return Status::Ok;
}

std::string IsoDevice::normalize_path(const std::string& path) {
    std::string result;
    result.reserve(path.size());
    
    for (char c : path) {
        if (c == '\\') {
            result += '/';
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    
    // Remove leading slash for lookup
    if (!result.empty() && result[0] == '/') {
        result = result.substr(1);
    }
    
    // Remove trailing slash
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    
    return result;
}

const IsoCachedEntry* IsoDevice::lookup_entry(const std::string& path) {
    std::string normalized = normalize_path(path);
    
    auto it = file_cache_.find(normalized);
    if (it != file_cache_.end()) {
        return &it->second;
    }
    
    return nullptr;
}

Status IsoDevice::ensure_directory_parsed(const std::string& path) {
    // Root is always parsed during mount
    if (path.empty() || path == "/" || path == "\\") {
        return Status::Ok;
    }
    
    std::string normalized = normalize_path(path);
    
    // Check if already parsed
    if (dir_cache_.find(normalized) != dir_cache_.end()) {
        return Status::Ok;
    }
    
    // Find the entry
    const IsoCachedEntry* entry = lookup_entry(path);
    if (!entry || !entry->is_directory) {
        return Status::NotFound;
    }
    
    // Parse the directory
    return parse_directory(entry->lba, entry->size, entry->full_path);
}

bool IsoDevice::exists(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return false;
    
    // Empty path or root always exists
    if (path.empty() || path == "/" || path == "\\") {
        return true;
    }
    
    return lookup_entry(path) != nullptr;
}

bool IsoDevice::is_directory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return false;
    
    // Empty path or root is directory
    if (path.empty() || path == "/" || path == "\\") {
        return true;
    }
    
    const IsoCachedEntry* entry = lookup_entry(path);
    return entry && entry->is_directory;
}

u32 IsoDevice::open(const std::string& path, FileAccess access, FileDisposition disposition) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return INVALID_FILE_HANDLE;
    
    // ISO is read-only
    u32 access_val = static_cast<u32>(access);
    if (access_val & 0x40000000) {  // Write access requested
        LOGE("IsoDevice: Write access denied (read-only)");
        return INVALID_FILE_HANDLE;
    }
    
    // Only support Open disposition
    if (disposition != FileDisposition::Open && disposition != FileDisposition::OpenIf) {
        LOGE("IsoDevice: Create/overwrite not supported (read-only)");
        return INVALID_FILE_HANDLE;
    }
    
    const IsoCachedEntry* entry = lookup_entry(path);
    if (!entry) {
        LOGE("IsoDevice: File not found: %s", path.c_str());
        return INVALID_FILE_HANDLE;
    }
    
    if (entry->is_directory) {
        LOGE("IsoDevice: Cannot open directory as file: %s", path.c_str());
        return INVALID_FILE_HANDLE;
    }
    
    u32 handle = next_handle_++;
    IsoOpenFile& file = open_files_[handle];
    file.handle = handle;
    file.path = path;
    file.lba = entry->lba;
    file.size = entry->size;
    file.position = 0;
    
    LOGD("IsoDevice: Opened file: %s (handle=%u, lba=%u, size=%u)",
         path.c_str(), handle, file.lba, file.size);
    
    return handle;
}

void IsoDevice::close(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    open_files_.erase(handle);
}

s64 IsoDevice::read(u32 handle, void* buffer, u64 size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return -1;
    }
    
    IsoOpenFile& file = it->second;
    
    // Check bounds
    if (file.position >= file.size) {
        return 0;  // EOF
    }
    
    // Limit read size
    u64 remaining = file.size - file.position;
    u64 to_read = std::min(size, remaining);
    
    if (to_read == 0) {
        return 0;
    }
    
    // Calculate file offset
    u64 file_offset = static_cast<u64>(file.lba) * SECTOR_SIZE + file.position;
    
    // Read data
    Status status = read_bytes(file_offset, to_read, buffer);
    if (status != Status::Ok) {
        return -1;
    }
    
    file.position += to_read;
    return static_cast<s64>(to_read);
}

s64 IsoDevice::write(u32 /*handle*/, const void* /*buffer*/, u64 /*size*/) {
    // ISO is read-only
    return -1;
}

s64 IsoDevice::seek(u32 handle, s64 offset, SeekOrigin origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return -1;
    }
    
    IsoOpenFile& file = it->second;
    
    s64 new_position;
    switch (origin) {
        case SeekOrigin::Begin:
            new_position = offset;
            break;
        case SeekOrigin::Current:
            new_position = static_cast<s64>(file.position) + offset;
            break;
        case SeekOrigin::End:
            new_position = static_cast<s64>(file.size) + offset;
            break;
        default:
            return -1;
    }
    
    if (new_position < 0) {
        return -1;
    }
    
    file.position = static_cast<u64>(new_position);
    return new_position;
}

u64 IsoDevice::tell(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return 0;
    }
    
    return it->second.position;
}

u64 IsoDevice::get_file_size(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return 0;
    }
    
    return it->second.size;
}

Status IsoDevice::get_file_info(const std::string& path, FileInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return Status::Error;
    
    // Handle root
    if (path.empty() || path == "/" || path == "\\") {
        info.size = 0;
        info.creation_time = 0;
        info.last_access_time = 0;
        info.last_write_time = 0;
        info.attributes = FileAttributes::Directory;
        return Status::Ok;
    }
    
    const IsoCachedEntry* entry = lookup_entry(path);
    if (!entry) {
        return Status::NotFound;
    }
    
    info.size = entry->size;
    info.creation_time = entry->creation_time;
    info.last_access_time = entry->creation_time;
    info.last_write_time = entry->creation_time;
    info.attributes = entry->is_directory ? FileAttributes::Directory : 
                      static_cast<FileAttributes>(static_cast<u32>(FileAttributes::ReadOnly) | 
                                                   static_cast<u32>(FileAttributes::Normal));
    
    return Status::Ok;
}

Status IsoDevice::list_directory(const std::string& path, std::vector<DirEntry>& entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return Status::Error;
    
    entries.clear();
    
    // Handle root
    std::string normalized;
    if (path.empty() || path == "/" || path == "\\") {
        normalized = "";
    } else {
        normalized = normalize_path(path);
    }
    
    // Look up in dir cache
    auto it = dir_cache_.find(normalized.empty() ? "/" : normalized);
    if (it == dir_cache_.end()) {
        // Try without leading slash
        it = dir_cache_.find(normalized);
    }
    
    if (it == dir_cache_.end()) {
        return Status::NotFound;
    }
    
    // Convert to DirEntry format
    for (const auto& cached : it->second) {
        DirEntry entry;
        entry.name = cached.name;
        entry.size = cached.size;
        entry.creation_time = cached.creation_time;
        entry.last_write_time = cached.creation_time;
        entry.is_directory = cached.is_directory;
        entry.attributes = cached.is_directory ? FileAttributes::Directory : FileAttributes::Normal;
        entries.push_back(std::move(entry));
    }
    
    return Status::Ok;
}

} // namespace x360mu
