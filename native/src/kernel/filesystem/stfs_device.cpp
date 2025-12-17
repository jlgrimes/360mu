/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * STFS Device implementation - Xbox 360 package format (LIVE/PIRS/CON)
 */

#include "stfs_device.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "x360mu-stfs"
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

// STFS constants
static constexpr u32 STFS_BLOCK_SIZE = 0x1000;              // 4KB data blocks
static constexpr u32 STFS_HASH_BLOCK_SPACING = 0xAA;        // Blocks between hash tables (170)
static constexpr u32 STFS_BLOCKS_PER_L0 = 170;              // Data blocks per L0 hash table
static constexpr u32 STFS_BLOCKS_PER_L1 = 170 * 170;        // Data blocks per L1 hash table
static constexpr u32 STFS_BLOCKS_PER_L2 = 170 * 170 * 170;  // Data blocks per L2 hash table

// Package offsets
static constexpr u32 STFS_METADATA_OFFSET = 0x344;
static constexpr u32 STFS_VOLUME_DESC_OFFSET = 0x379;

StfsDevice::StfsDevice() = default;

StfsDevice::~StfsDevice() {
    unmount();
}

Status StfsDevice::mount(const std::string& stfs_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (mounted_) {
        LOGE("StfsDevice: Already mounted");
        return Status::Error;
    }
    
    stfs_file_ = fopen(stfs_path.c_str(), "rb");
    if (!stfs_file_) {
        LOGE("StfsDevice: Failed to open STFS file: %s", stfs_path.c_str());
        return Status::NotFound;
    }
    
    stfs_path_ = stfs_path;
    
    // Read and validate header
    Status status = read_header();
    if (status != Status::Ok) {
        fclose(stfs_file_);
        stfs_file_ = nullptr;
        return status;
    }
    
    // Read volume descriptor
    status = read_volume_descriptor();
    if (status != Status::Ok) {
        fclose(stfs_file_);
        stfs_file_ = nullptr;
        return status;
    }
    
    // Parse file table
    status = parse_file_table();
    if (status != Status::Ok) {
        fclose(stfs_file_);
        stfs_file_ = nullptr;
        return status;
    }
    
    // Build path index
    build_path_index();
    
    mounted_ = true;
    LOGI("StfsDevice: Mounted %s (TitleID: %08X, Files: %zu)",
         stfs_path.c_str(), title_id_, file_table_.size());
    
    return Status::Ok;
}

void StfsDevice::unmount() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Close all open files
    open_files_.clear();
    
    // Clear caches
    file_table_.clear();
    path_to_entry_.clear();
    
    if (stfs_file_) {
        fclose(stfs_file_);
        stfs_file_ = nullptr;
    }
    
    mounted_ = false;
}

Status StfsDevice::read_header() {
    // Read magic
    u8 magic_bytes[4];
    if (fread(magic_bytes, 1, 4, stfs_file_) != 4) {
        LOGE("StfsDevice: Failed to read magic");
        return Status::IoError;
    }
    
    u32 magic_val = (magic_bytes[0] << 24) | (magic_bytes[1] << 16) | 
                   (magic_bytes[2] << 8) | magic_bytes[3];
    
    switch (magic_val) {
        case static_cast<u32>(StfsMagic::CON):
            magic_ = StfsMagic::CON;
            header_size_ = 0xB000;  // CON packages have larger header
            break;
        case static_cast<u32>(StfsMagic::LIVE):
            magic_ = StfsMagic::LIVE;
            header_size_ = 0xA000;
            break;
        case static_cast<u32>(StfsMagic::PIRS):
            magic_ = StfsMagic::PIRS;
            header_size_ = 0xA000;
            break;
        default:
            LOGE("StfsDevice: Invalid magic: %08X", magic_val);
            return Status::InvalidFormat;
    }
    
    // Read metadata at offset 0x344
    if (fseek(stfs_file_, STFS_METADATA_OFFSET, SEEK_SET) != 0) {
        return Status::IoError;
    }
    
    // Content type (big-endian u32)
    u8 ct_bytes[4];
    if (fread(ct_bytes, 1, 4, stfs_file_) != 4) {
        return Status::IoError;
    }
    u32 ct = (ct_bytes[0] << 24) | (ct_bytes[1] << 16) | (ct_bytes[2] << 8) | ct_bytes[3];
    content_type_ = static_cast<StfsContentType>(ct);
    
    // Skip metadata version (4 bytes)
    fseek(stfs_file_, 4, SEEK_CUR);
    
    // Content size (big-endian u64)
    u8 cs_bytes[8];
    if (fread(cs_bytes, 1, 8, stfs_file_) != 8) {
        return Status::IoError;
    }
    content_size_ = 0;
    for (int i = 0; i < 8; i++) {
        content_size_ = (content_size_ << 8) | cs_bytes[i];
    }
    
    // Execution info: media ID (4), version (4), base version (4), title ID (4)
    // Title ID is at offset 0x360
    if (fseek(stfs_file_, 0x360, SEEK_SET) != 0) {
        return Status::IoError;
    }
    
    u8 tid_bytes[4];
    if (fread(tid_bytes, 1, 4, stfs_file_) != 4) {
        return Status::IoError;
    }
    title_id_ = (tid_bytes[0] << 24) | (tid_bytes[1] << 16) | (tid_bytes[2] << 8) | tid_bytes[3];
    
    LOGI("StfsDevice: Magic=%s, ContentType=%08X, TitleID=%08X, Size=%llu",
         magic_ == StfsMagic::CON ? "CON" : (magic_ == StfsMagic::LIVE ? "LIVE" : "PIRS"),
         static_cast<u32>(content_type_), title_id_, static_cast<unsigned long long>(content_size_));
    
    return Status::Ok;
}

Status StfsDevice::read_volume_descriptor() {
    // Volume descriptor is at offset 0x379
    if (fseek(stfs_file_, STFS_VOLUME_DESC_OFFSET, SEEK_SET) != 0) {
        return Status::IoError;
    }
    
    u8 desc[0x24];
    if (fread(desc, 1, sizeof(desc), stfs_file_) != sizeof(desc)) {
        return Status::IoError;
    }
    
    // Parse volume descriptor
    // [0] = descriptor size (usually 0x24)
    // [1] = reserved
    // [2] = block separation (0 = single, 1 = split for LIVE/PIRS)
    block_separation_ = desc[2];
    
    // [3-4] = file table block count (big-endian u16)
    file_table_block_count_ = (desc[3] << 8) | desc[4];
    
    // [5-7] = file table starting block (24-bit)
    file_table_start_block_ = read_u24(&desc[5]);
    
    // [8-27] = top hash table hash (SHA1)
    // [28-31] = total allocated block count (big-endian u32)
    total_allocated_blocks_ = (desc[28] << 24) | (desc[29] << 16) | (desc[30] << 8) | desc[31];
    
    // Calculate data offset based on header size
    // For LIVE/PIRS, data starts after 0xA000 byte header
    // For CON, data starts after 0xB000 byte header
    data_offset_ = header_size_;
    
    LOGI("StfsDevice: FileTableStart=%u, FileTableBlocks=%u, TotalBlocks=%u, BlockSep=%u",
         file_table_start_block_, file_table_block_count_, total_allocated_blocks_, block_separation_);
    
    return Status::Ok;
}

u64 StfsDevice::block_to_offset(u32 block_num) {
    // STFS uses hash tables interspersed with data blocks
    // For every 170 data blocks, there's a hash table block
    
    u32 hash_blocks_before;
    
    if (block_separation_ == 0) {
        // Single-level hash tables (small packages)
        hash_blocks_before = block_num / STFS_BLOCKS_PER_L0;
    } else {
        // Multi-level hash tables
        // L0 hash tables appear every 170 blocks
        // L1 hash tables appear every 170*170 blocks
        // L2 hash tables appear every 170*170*170 blocks
        
        u32 l0_tables = block_num / STFS_BLOCKS_PER_L0;
        u32 l1_tables = block_num / STFS_BLOCKS_PER_L1;
        u32 l2_tables = block_num / STFS_BLOCKS_PER_L2;
        
        hash_blocks_before = l0_tables + l1_tables + l2_tables;
    }
    
    // Calculate actual file offset
    u64 offset = data_offset_ + static_cast<u64>(block_num + hash_blocks_before) * BLOCK_SIZE;
    
    return offset;
}

Status StfsDevice::read_block(u32 block_num, void* buffer) {
    u64 offset = block_to_offset(block_num);
    
    if (fseeko(stfs_file_, offset, SEEK_SET) != 0) {
        LOGE("StfsDevice: Failed to seek to block %u (offset %llu)", 
             block_num, static_cast<unsigned long long>(offset));
        return Status::IoError;
    }
    
    if (fread(buffer, 1, BLOCK_SIZE, stfs_file_) != BLOCK_SIZE) {
        LOGE("StfsDevice: Failed to read block %u", block_num);
        return Status::IoError;
    }
    
    return Status::Ok;
}

u32 StfsDevice::get_hash_block(u32 data_block) {
    // Calculate which hash block contains the entry for this data block
    u32 l0_index = data_block / STFS_BLOCKS_PER_L0;
    return l0_index;
}

u32 StfsDevice::get_next_block(u32 block_num) {
    // Read the hash entry for this block to get the next block pointer
    
    u32 hash_block = get_hash_block(block_num);
    u32 entry_index = block_num % STFS_BLOCKS_PER_L0;
    
    // Calculate hash table offset
    // Hash tables come before data blocks at specific intervals
    u64 hash_offset;
    
    if (block_separation_ == 0) {
        // Simple case: hash tables at beginning
        hash_offset = data_offset_ + static_cast<u64>(hash_block) * BLOCK_SIZE;
    } else {
        // Multi-level: need to account for hierarchy
        // This is a simplified version - full implementation would need
        // to properly traverse L0/L1/L2 tables
        u32 base = hash_block * (STFS_BLOCKS_PER_L0 + 1);
        hash_offset = data_offset_ + static_cast<u64>(base) * BLOCK_SIZE;
    }
    
    // Each hash entry is 24 bytes: 20 byte SHA1 + 3 byte next block + 1 byte status
    u64 entry_offset = hash_offset + entry_index * 24 + 20;  // Skip SHA1
    
    if (fseeko(stfs_file_, entry_offset, SEEK_SET) != 0) {
        return 0xFFFFFF;  // Invalid block
    }
    
    u8 next_bytes[3];
    if (fread(next_bytes, 1, 3, stfs_file_) != 3) {
        return 0xFFFFFF;
    }
    
    u32 next_block = read_u24(next_bytes);
    
    // 0xFFFFFF indicates end of chain
    return next_block;
}

Status StfsDevice::build_block_chain(u32 start_block, u32 file_size, std::vector<u32>& chain) {
    chain.clear();
    
    if (file_size == 0) {
        return Status::Ok;
    }
    
    u32 blocks_needed = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    u32 current_block = start_block;
    
    chain.reserve(blocks_needed);
    
    for (u32 i = 0; i < blocks_needed && current_block != 0xFFFFFF; i++) {
        chain.push_back(current_block);
        
        if (i + 1 < blocks_needed) {
            current_block = get_next_block(current_block);
        }
    }
    
    return Status::Ok;
}

Status StfsDevice::parse_file_table() {
    file_table_.clear();
    
    if (file_table_block_count_ == 0) {
        LOGI("StfsDevice: Empty file table");
        return Status::Ok;
    }
    
    // Read file table blocks
    std::vector<u8> table_data(file_table_block_count_ * BLOCK_SIZE);
    
    u32 current_block = file_table_start_block_;
    for (u32 i = 0; i < file_table_block_count_ && current_block != 0xFFFFFF; i++) {
        Status status = read_block(current_block, table_data.data() + i * BLOCK_SIZE);
        if (status != Status::Ok) {
            LOGE("StfsDevice: Failed to read file table block %u", current_block);
            return status;
        }
        
        // For consecutive file tables, just increment
        // For non-consecutive, we'd need to follow the chain
        current_block++;
    }
    
    // Parse file entries
    // Each entry is 0x40 (64) bytes
    static constexpr u32 ENTRY_SIZE = 0x40;
    u32 entry_count = (file_table_block_count_ * BLOCK_SIZE) / ENTRY_SIZE;
    
    for (u32 i = 0; i < entry_count; i++) {
        const u8* entry_data = table_data.data() + i * ENTRY_SIZE;
        
        // Check if entry is valid (name not empty)
        if (entry_data[0] == 0) {
            continue;
        }
        
        StfsCachedEntry entry;
        
        // File name (40 bytes, null-terminated)
        char name[0x29];
        memcpy(name, entry_data, 0x28);
        name[0x28] = '\0';
        entry.name = name;
        
        // Flags (1 byte at offset 0x28)
        u8 flags = entry_data[0x28];
        entry.is_directory = (flags & static_cast<u8>(StfsEntryFlags::Directory)) != 0;
        entry.blocks_consecutive = (flags & static_cast<u8>(StfsEntryFlags::Consecutive)) != 0;
        
        // Starting block (3 bytes at offset 0x29, plus high byte at 0x2C)
        entry.starting_block = read_u24(entry_data + 0x29);
        entry.starting_block |= (static_cast<u32>(entry_data[0x2C]) << 24);
        
        // File size (4 bytes big-endian at offset 0x34)
        entry.file_size = (entry_data[0x34] << 24) | (entry_data[0x35] << 16) |
                         (entry_data[0x36] << 8) | entry_data[0x37];
        
        // Timestamps (4 bytes each, big-endian)
        entry.update_time = (entry_data[0x38] << 24) | (entry_data[0x39] << 16) |
                           (entry_data[0x3A] << 8) | entry_data[0x3B];
        entry.access_time = (entry_data[0x3C] << 24) | (entry_data[0x3D] << 16) |
                           (entry_data[0x3E] << 8) | entry_data[0x3F];
        
        // Path index (2 bytes big-endian at offset 0x32) - parent directory index
        s32 path_index = static_cast<s16>((entry_data[0x32] << 8) | entry_data[0x33]);
        entry.path_index = path_index;
        
        file_table_.push_back(entry);
        
        LOGD("StfsDevice: Entry %u: '%s' %s (block=%u, size=%u, parent=%d)",
             i, entry.name.c_str(), entry.is_directory ? "DIR" : "FILE",
             entry.starting_block, entry.file_size, entry.path_index);
    }
    
    LOGI("StfsDevice: Parsed %zu file entries", file_table_.size());
    return Status::Ok;
}

void StfsDevice::build_path_index() {
    path_to_entry_.clear();
    
    // Build full paths for each entry
    for (size_t i = 0; i < file_table_.size(); i++) {
        StfsCachedEntry& entry = file_table_[i];
        
        // Build full path by traversing parent indices
        std::string path = entry.name;
        s32 parent_idx = entry.path_index;
        
        while (parent_idx >= 0 && static_cast<size_t>(parent_idx) < file_table_.size()) {
            path = file_table_[parent_idx].name + "/" + path;
            parent_idx = file_table_[parent_idx].path_index;
        }
        
        entry.full_path = path;
        
        // Normalize for lookup
        std::string lookup = normalize_path(path);
        path_to_entry_[lookup] = i;
        
        LOGD("StfsDevice: Path mapping: '%s' -> %zu", lookup.c_str(), i);
    }
}

std::string StfsDevice::normalize_path(const std::string& path) {
    std::string result;
    result.reserve(path.size());
    
    for (char c : path) {
        if (c == '\\') {
            result += '/';
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    
    // Remove leading slash
    if (!result.empty() && result[0] == '/') {
        result = result.substr(1);
    }
    
    // Remove trailing slash
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    
    return result;
}

const StfsCachedEntry* StfsDevice::lookup_entry(const std::string& path) {
    std::string normalized = normalize_path(path);
    
    auto it = path_to_entry_.find(normalized);
    if (it != path_to_entry_.end()) {
        return &file_table_[it->second];
    }
    
    return nullptr;
}

bool StfsDevice::exists(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return false;
    
    // Root always exists
    if (path.empty() || path == "/" || path == "\\") {
        return true;
    }
    
    return lookup_entry(path) != nullptr;
}

bool StfsDevice::is_directory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return false;
    
    // Root is directory
    if (path.empty() || path == "/" || path == "\\") {
        return true;
    }
    
    const StfsCachedEntry* entry = lookup_entry(path);
    return entry && entry->is_directory;
}

u32 StfsDevice::open(const std::string& path, FileAccess access, FileDisposition disposition) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return INVALID_FILE_HANDLE;
    
    // STFS is read-only
    u32 access_val = static_cast<u32>(access);
    if (access_val & 0x40000000) {
        LOGE("StfsDevice: Write access denied (read-only)");
        return INVALID_FILE_HANDLE;
    }
    
    if (disposition != FileDisposition::Open && disposition != FileDisposition::OpenIf) {
        LOGE("StfsDevice: Create/overwrite not supported (read-only)");
        return INVALID_FILE_HANDLE;
    }
    
    const StfsCachedEntry* entry = lookup_entry(path);
    if (!entry) {
        LOGE("StfsDevice: File not found: %s", path.c_str());
        return INVALID_FILE_HANDLE;
    }
    
    if (entry->is_directory) {
        LOGE("StfsDevice: Cannot open directory as file: %s", path.c_str());
        return INVALID_FILE_HANDLE;
    }
    
    u32 handle = next_handle_++;
    StfsOpenFile& file = open_files_[handle];
    file.handle = handle;
    file.path = path;
    file.starting_block = entry->starting_block;
    file.file_size = entry->file_size;
    file.position = 0;
    file.consecutive = entry->blocks_consecutive;
    
    // If not consecutive, build block chain
    if (!file.consecutive && file.file_size > 0) {
        build_block_chain(file.starting_block, file.file_size, file.block_chain);
    }
    
    LOGD("StfsDevice: Opened file: %s (handle=%u, block=%u, size=%u, consecutive=%d)",
         path.c_str(), handle, file.starting_block, file.file_size, file.consecutive);
    
    return handle;
}

void StfsDevice::close(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    open_files_.erase(handle);
}

s64 StfsDevice::read_file_data(StfsOpenFile& file, void* buffer, u64 size) {
    if (file.position >= file.file_size) {
        return 0;  // EOF
    }
    
    u64 remaining = file.file_size - file.position;
    u64 to_read = std::min(size, remaining);
    
    if (to_read == 0) {
        return 0;
    }
    
    u8* out = static_cast<u8*>(buffer);
    u64 total_read = 0;
    
    while (total_read < to_read) {
        // Calculate which block we need
        u32 block_index = static_cast<u32>(file.position / BLOCK_SIZE);
        u32 block_offset = static_cast<u32>(file.position % BLOCK_SIZE);
        
        // Get the actual block number
        u32 block_num;
        if (file.consecutive) {
            block_num = file.starting_block + block_index;
        } else {
            if (block_index >= file.block_chain.size()) {
                break;  // Ran out of blocks
            }
            block_num = file.block_chain[block_index];
        }
        
        // How much can we read from this block?
        u32 can_read = BLOCK_SIZE - block_offset;
        u64 want_read = to_read - total_read;
        u32 will_read = static_cast<u32>(std::min(static_cast<u64>(can_read), want_read));
        
        // Read the block
        u8 block_data[BLOCK_SIZE];
        Status status = read_block(block_num, block_data);
        if (status != Status::Ok) {
            LOGE("StfsDevice: Failed to read block %u", block_num);
            break;
        }
        
        // Copy data
        memcpy(out + total_read, block_data + block_offset, will_read);
        
        total_read += will_read;
        file.position += will_read;
    }
    
    return static_cast<s64>(total_read);
}

s64 StfsDevice::read(u32 handle, void* buffer, u64 size) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return -1;
    }
    
    return read_file_data(it->second, buffer, size);
}

s64 StfsDevice::write(u32 /*handle*/, const void* /*buffer*/, u64 /*size*/) {
    // STFS is read-only
    return -1;
}

s64 StfsDevice::seek(u32 handle, s64 offset, SeekOrigin origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return -1;
    }
    
    StfsOpenFile& file = it->second;
    
    s64 new_position;
    switch (origin) {
        case SeekOrigin::Begin:
            new_position = offset;
            break;
        case SeekOrigin::Current:
            new_position = static_cast<s64>(file.position) + offset;
            break;
        case SeekOrigin::End:
            new_position = static_cast<s64>(file.file_size) + offset;
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

u64 StfsDevice::tell(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return 0;
    }
    
    return it->second.position;
}

u64 StfsDevice::get_file_size(u32 handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return 0;
    }
    
    return it->second.file_size;
}

Status StfsDevice::get_file_info(const std::string& path, FileInfo& info) {
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
    
    const StfsCachedEntry* entry = lookup_entry(path);
    if (!entry) {
        return Status::NotFound;
    }
    
    info.size = entry->file_size;
    info.creation_time = entry->update_time;
    info.last_access_time = entry->access_time;
    info.last_write_time = entry->update_time;
    info.attributes = entry->is_directory ? FileAttributes::Directory :
                      static_cast<FileAttributes>(static_cast<u32>(FileAttributes::ReadOnly) |
                                                   static_cast<u32>(FileAttributes::Normal));
    
    return Status::Ok;
}

Status StfsDevice::list_directory(const std::string& path, std::vector<DirEntry>& entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mounted_) return Status::Error;
    
    entries.clear();
    
    std::string normalized = normalize_path(path);
    
    // Find parent index
    s32 parent_index = -1;  // -1 for root
    
    if (!normalized.empty()) {
        // Find the directory entry
        auto it = path_to_entry_.find(normalized);
        if (it == path_to_entry_.end()) {
            return Status::NotFound;
        }
        
        if (!file_table_[it->second].is_directory) {
            return Status::InvalidArgument;
        }
        
        parent_index = static_cast<s32>(it->second);
    }
    
    // Find all entries with this parent
    for (const auto& entry : file_table_) {
        if (entry.path_index == parent_index) {
            DirEntry de;
            de.name = entry.name;
            de.size = entry.file_size;
            de.creation_time = entry.update_time;
            de.last_write_time = entry.update_time;
            de.is_directory = entry.is_directory;
            de.attributes = entry.is_directory ? FileAttributes::Directory : FileAttributes::Normal;
            entries.push_back(std::move(de));
        }
    }
    
    return Status::Ok;
}

} // namespace x360mu
