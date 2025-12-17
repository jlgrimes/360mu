/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * ISO 9660 Device - Disc image mounting and reading
 */

#pragma once

#include "vfs.h"
#include <cstdio>
#include <map>
#include <vector>

namespace x360mu {

/**
 * ISO 9660 Primary Volume Descriptor
 * Must be packed - ISO 9660 format has no padding
 */
#pragma pack(push, 1)
struct IsoPrimaryVolumeDescriptor {
    u8 type;                    // Volume descriptor type (1 = PVD)
    char identifier[5];         // "CD001"
    u8 version;                 // Volume descriptor version
    u8 unused1;
    char system_id[32];         // System identifier
    char volume_id[32];         // Volume identifier
    u8 unused2[8];
    u32 volume_space_size_le;   // Little-endian volume size in blocks
    u32 volume_space_size_be;   // Big-endian volume size in blocks
    u8 unused3[32];
    u16 volume_set_size_le;
    u16 volume_set_size_be;
    u16 volume_seq_num_le;
    u16 volume_seq_num_be;
    u16 logical_block_size_le;  // Little-endian block size
    u16 logical_block_size_be;  // Big-endian block size
    u32 path_table_size_le;
    u32 path_table_size_be;
    u32 path_table_lba_le;
    u32 path_table_lba_opt_le;
    u32 path_table_lba_be;
    u32 path_table_lba_opt_be;
    u8 root_dir_record[34];     // Root directory record
    // ... more fields follow (publisher, copyright, etc.)
};
#pragma pack(pop)

/**
 * ISO 9660 Directory Record
 * Must be packed - ISO 9660 format has no padding
 */
#pragma pack(push, 1)
struct IsoDirectoryRecord {
    u8 length;                  // Length of directory record
    u8 ext_attr_length;         // Extended attribute length
    u32 extent_lba_le;          // Little-endian LBA
    u32 extent_lba_be;          // Big-endian LBA
    u32 data_length_le;         // Little-endian data length
    u32 data_length_be;         // Big-endian data length
    u8 recording_date[7];       // Recording date and time
    u8 flags;                   // File flags
    u8 interleave_unit;
    u8 interleave_gap;
    u16 volume_seq_le;
    u16 volume_seq_be;
    u8 name_length;             // Length of file identifier
    // Followed by file identifier (variable length)
};
#pragma pack(pop)

// Directory record flags
enum class IsoFileFlags : u8 {
    Hidden          = 0x01,
    Directory       = 0x02,
    AssociatedFile  = 0x04,
    Record          = 0x08,
    Protection      = 0x10,
    Reserved1       = 0x20,
    Reserved2       = 0x40,
    MultiExtent     = 0x80,
};

/**
 * Cached file/directory entry
 */
struct IsoCachedEntry {
    std::string name;
    u32 lba;                    // Logical block address
    u32 size;                   // File size in bytes
    bool is_directory;
    u64 creation_time;
    std::string full_path;      // Normalized path
};

/**
 * Open file state
 */
struct IsoOpenFile {
    u32 handle;
    std::string path;
    u32 lba;
    u32 size;
    u64 position;
};

/**
 * ISO 9660 Device implementation
 */
class IsoDevice : public VfsDevice {
public:
    IsoDevice();
    ~IsoDevice() override;
    
    Status mount(const std::string& iso_path) override;
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
    
    bool is_read_only() const override { return true; }
    const std::string& get_type() const override { return type_; }
    
    /**
     * Get volume identifier
     */
    const std::string& get_volume_id() const { return volume_id_; }
    
private:
    static constexpr u32 SECTOR_SIZE = 2048;
    static constexpr u32 PVD_SECTOR = 16;
    
    FILE* iso_file_ = nullptr;
    std::string iso_path_;
    std::string type_ = "iso";
    std::string volume_id_;
    bool mounted_ = false;
    
    // XGD (Xbox Game Disc) support
    bool is_xgd_ = false;
    u64 xgd_base_offset_ = 0;
    u32 xgd_sector_offset_ = 0;  // Adjustment for sector numbers
    
    // Primary volume descriptor
    u32 volume_space_size_ = 0;
    u16 logical_block_size_ = SECTOR_SIZE;
    u32 root_dir_lba_ = 0;
    u32 root_dir_size_ = 0;
    
    // File cache (path -> entry)
    std::map<std::string, IsoCachedEntry> file_cache_;
    
    // Directory cache (path -> list of entries)
    std::map<std::string, std::vector<IsoCachedEntry>> dir_cache_;
    
    // Open files
    std::unordered_map<u32, IsoOpenFile> open_files_;
    u32 next_handle_ = 1;
    
    std::mutex mutex_;
    
    /**
     * Read primary volume descriptor
     */
    Status read_pvd();
    
    /**
     * Read a sector from the ISO
     */
    Status read_sector(u32 lba, void* buffer);
    
    /**
     * Read multiple sectors
     */
    Status read_sectors(u32 lba, u32 count, void* buffer);
    
    /**
     * Read raw bytes at position
     */
    Status read_bytes(u64 offset, u64 size, void* buffer);
    
    /**
     * Parse a directory and cache its entries
     */
    Status parse_directory(u32 lba, u32 size, const std::string& parent_path);
    
    /**
     * Parse a directory record
     */
    bool parse_directory_record(const u8* data, IsoCachedEntry& entry);
    
    /**
     * Look up cached entry
     */
    const IsoCachedEntry* lookup_entry(const std::string& path);
    
    /**
     * Ensure directory is parsed
     */
    Status ensure_directory_parsed(const std::string& path);
    
    /**
     * Normalize a path for lookup
     */
    std::string normalize_path(const std::string& path);
    
    /**
     * Convert ISO date to timestamp
     */
    u64 iso_date_to_timestamp(const u8* date);
    
    /**
     * Try mounting as Xbox Game Disc (XGD) format
     */
    Status try_xgd_mount();
    
    /**
     * Parse XGD directory
     */
    Status parse_xgd_directory(u32 sector, u32 size, const std::string& parent_path);
    
    /**
     * Parse XGD directory entry (recursive tree traversal)
     */
    void parse_xgd_entry(const u8* data, u32 size, u32 offset,
                         const std::string& parent_path,
                         std::vector<IsoCachedEntry>& entries);
};

} // namespace x360mu

