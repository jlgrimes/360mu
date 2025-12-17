/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * STFS Device - Xbox 360 package format (LIVE/PIRS/CON)
 */

#pragma once

#include "vfs.h"
#include <cstdio>
#include <map>
#include <vector>

namespace x360mu {

// STFS magic values
enum class StfsMagic : u32 {
    CON  = 0x434F4E20,  // 'CON ' - Console signed
    LIVE = 0x4C495645,  // 'LIVE' - Xbox Live signed
    PIRS = 0x50495253,  // 'PIRS' - Publisher signed
};

// STFS content types
enum class StfsContentType : u32 {
    ArcadeTitle         = 0x000D0000,
    AvatarItem          = 0x00009000,
    CacheFile           = 0x00040000,
    CommunityGame       = 0x02000000,
    GameDemo            = 0x00080000,
    GamerPicture        = 0x00020000,
    GameTitle           = 0x000A0000,
    GameTrailer         = 0x000C0000,
    GameVideo           = 0x00400000,
    InstalledGame       = 0x00004000,
    Installer           = 0x000B0000,
    IPTVPauseBuffer     = 0x00002000,
    LicenseStore        = 0x000F0000,
    MarketplaceContent  = 0x00000002,
    Movie               = 0x00100000,
    MusicVideo          = 0x00300000,
    PodcastVideo        = 0x00500000,
    Profile             = 0x00010000,
    Publisher           = 0x00000003,
    SavedGame           = 0x00000001,
    StorageDownload     = 0x00050000,
    Theme               = 0x00030000,
    Video               = 0x00200000,
    ViralVideo          = 0x00600000,
    XboxDownload        = 0x00070000,
    XboxOriginalGame    = 0x00005000,
    XboxSavedGame       = 0x00060000,
    Xbox360Title        = 0x00001000,
    XNA                 = 0x000E0000,
};

#pragma pack(push, 1)

/**
 * STFS package header
 */
struct StfsHeader {
    be_u32 magic;                    // 'CON ', 'LIVE', or 'PIRS'
    u8 signature[0x228];             // RSA signature
    u8 license_data[0x100];          // License entries
    u8 content_id[0x14];             // SHA1 hash of header
    be_u32 header_size;              // Header size
};

/**
 * STFS metadata
 */
struct StfsMetadata {
    be_u32 content_type;
    be_u32 metadata_version;
    be_u64 content_size;
    be_u32 execution_info_media_id;
    be_u32 execution_info_version;
    be_u32 execution_info_base_version;
    be_u32 execution_info_title_id;
    u8 execution_info_platform;
    u8 execution_info_executable_type;
    u8 execution_info_disc_number;
    u8 execution_info_disc_count;
    be_u32 save_game_id;
    u8 console_id[5];
    u8 profile_id[8];
    // Volume descriptor follows at 0x379
};

/**
 * STFS volume descriptor
 */
struct StfsVolumeDescriptor {
    u8 descriptor_size;              // Size of descriptor (usually 0x24)
    u8 reserved;
    u8 block_separation;             // 0 = single block, 1 = split blocks
    be_u16 file_table_block_count;   // Number of blocks in file table
    u8 file_table_block_num[3];      // 24-bit block number
    u8 top_hash_table_hash[0x14];    // SHA1 of top hash table
    be_u32 total_allocated_block_count;
    be_u32 total_unallocated_block_count;
};

/**
 * STFS file entry in file table
 */
struct StfsFileEntry {
    char name[0x28];                 // File name (null terminated)
    u8 flags;                        // 0x80 = directory, 0x40 = blocks consecutive
    u8 starting_block_num[3];        // 24-bit starting block
    u8 starting_block_num_high;      // High byte of block number (for large files)
    u8 reserved[2];
    be_u32 file_size;                // File size in bytes
    be_u32 update_timestamp;
    be_u32 access_timestamp;
};

#pragma pack(pop)

// File entry flags
enum class StfsEntryFlags : u8 {
    Directory = 0x80,
    Consecutive = 0x40,
};

/**
 * Cached STFS file entry
 */
struct StfsCachedEntry {
    std::string name;
    std::string full_path;
    u32 starting_block;
    u32 file_size;
    bool is_directory;
    bool blocks_consecutive;
    u32 update_time;
    u32 access_time;
    s32 path_index;         // Index of parent directory (-1 for root)
};

/**
 * Open STFS file state
 */
struct StfsOpenFile {
    u32 handle;
    std::string path;
    u32 starting_block;
    u32 file_size;
    u64 position;
    bool consecutive;
    std::vector<u32> block_chain;  // Computed block chain for non-consecutive
};

/**
 * STFS Device implementation
 */
class StfsDevice : public VfsDevice {
public:
    StfsDevice();
    ~StfsDevice() override;
    
    Status mount(const std::string& stfs_path) override;
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
     * Get package title ID
     */
    u32 get_title_id() const { return title_id_; }
    
    /**
     * Get content type
     */
    StfsContentType get_content_type() const { return content_type_; }
    
    /**
     * Get package magic
     */
    StfsMagic get_magic() const { return magic_; }
    
private:
    static constexpr u32 BLOCK_SIZE = 0x1000;          // 4KB blocks
    static constexpr u32 HASH_BLOCK_SIZE = 0x1000;     // Hash table block size
    static constexpr u32 DATA_BLOCKS_PER_HASH = 170;   // Data blocks per hash table
    
    FILE* stfs_file_ = nullptr;
    std::string stfs_path_;
    std::string type_ = "stfs";
    bool mounted_ = false;
    
    // Package info
    StfsMagic magic_ = StfsMagic::CON;
    StfsContentType content_type_ = StfsContentType::SavedGame;
    u32 title_id_ = 0;
    u64 content_size_ = 0;
    
    // Volume descriptor
    u32 file_table_block_count_ = 0;
    u32 file_table_start_block_ = 0;
    u32 total_allocated_blocks_ = 0;
    u8 block_separation_ = 0;
    
    // Header offset (depends on magic)
    u32 header_size_ = 0;
    u32 data_offset_ = 0;
    
    // File cache
    std::vector<StfsCachedEntry> file_table_;
    std::map<std::string, size_t> path_to_entry_;  // path -> file_table_ index
    
    // Open files
    std::unordered_map<u32, StfsOpenFile> open_files_;
    u32 next_handle_ = 1;
    
    std::mutex mutex_;
    
    /**
     * Read and parse STFS header
     */
    Status read_header();
    
    /**
     * Read and parse volume descriptor
     */
    Status read_volume_descriptor();
    
    /**
     * Parse file table
     */
    Status parse_file_table();
    
    /**
     * Build path index
     */
    void build_path_index();
    
    /**
     * Convert block number to file offset
     */
    u64 block_to_offset(u32 block_num);
    
    /**
     * Get hash table block number for a data block
     */
    u32 get_hash_block(u32 data_block);
    
    /**
     * Get next block in chain
     */
    u32 get_next_block(u32 block_num);
    
    /**
     * Build complete block chain for a file
     */
    Status build_block_chain(u32 start_block, u32 file_size, std::vector<u32>& chain);
    
    /**
     * Read a data block
     */
    Status read_block(u32 block_num, void* buffer);
    
    /**
     * Read bytes from file using block chain
     */
    s64 read_file_data(StfsOpenFile& file, void* buffer, u64 size);
    
    /**
     * Look up cached entry
     */
    const StfsCachedEntry* lookup_entry(const std::string& path);
    
    /**
     * Normalize path
     */
    std::string normalize_path(const std::string& path);
    
    /**
     * Read 24-bit integer from 3 bytes (little-endian)
     */
    static u32 read_u24(const u8* data) {
        return data[0] | (data[1] << 8) | (data[2] << 16);
    }
};

} // namespace x360mu
