/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XEX2 Loader
 * 
 * Loads Xbox 360 executables (XEX2 format) into memory.
 * XEX2 is the executable format used by all Xbox 360 games and system software.
 */

#pragma once

#include "x360mu/types.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

namespace x360mu {

/**
 * XEX2 Header IDs
 */
enum class XexHeaderId : u32 {
    kResourceInfo = 0x000002FF,
    kBaseFileFormat = 0x000003FF,
    kBaseReference = 0x00000405,
    kDeltaPatchDescriptor = 0x000005FF,
    kBoundingPath = 0x000080FF,
    kDeviceId = 0x00008105,
    kOriginalBaseAddress = 0x00010001,
    kEntryPoint = 0x00010100,
    kImageBaseAddress = 0x00010201,
    kImportLibraries = 0x000103FF,
    kChecksumTimestamp = 0x00018002,
    kEnabledForCallcap = 0x00018102,
    kEnabledForFastcap = 0x00018200,
    kOriginalPeName = 0x000183FF,
    kStaticLibraries = 0x000200FF,
    kTLSInfo = 0x00020104,
    kDefaultStackSize = 0x00020200,
    kDefaultFilesystemCacheSize = 0x00020301,
    kDefaultHeapSize = 0x00020401,
    kPageHeapSizeAndFlags = 0x00028002,
    kSystemFlags = 0x00030000,
    kExecutionInfo = 0x00040006,
    kTitleWorkspaceSize = 0x00040201,
    kGameRatings = 0x00040310,
    kLANKey = 0x00040404,
    kXbox360Logo = 0x000405FF,
    kMultidiscMediaIds = 0x000406FF,
    kAlternateTitleIds = 0x000407FF,
    kAdditionalTitleMemory = 0x00040801,
    kExportsByName = 0x00E10402,
};

/**
 * XEX2 module flags
 */
enum class XexModuleFlags : u32 {
    kTitle = 0x00000001,
    kExportsToTitle = 0x00000002,
    kSystemDebugger = 0x00000004,
    kDllModule = 0x00000008,
    kModulePatch = 0x00000010,
    kFullPatch = 0x00000020,
    kDeltaPatch = 0x00000040,
    kUserMode = 0x00000080,
};

/**
 * XEX2 file header (first 24 bytes)
 */
struct XexFileHeader {
    u32 magic;              // 'XEX2'
    u32 module_flags;
    u32 pe_data_offset;     // Offset to PE data
    u32 reserved;
    u32 security_offset;    // Offset to security info
    u32 header_count;       // Number of optional headers
};

/**
 * XEX2 optional header
 */
struct XexOptionalHeader {
    u32 key;                // Header ID
    u32 value_or_offset;    // Value if < 0x100, offset if >= 0x100
};

/**
 * XEX2 security info
 */
struct XexSecurityInfo {
    u32 header_size;
    u32 image_size;
    u8 rsa_signature[256];
    u32 unknown_count;
    u8 image_hash[20];      // SHA-1
    u32 import_table_count;
    u8 import_digest[20];   // SHA-1
    u8 media_id[16];
    u8 file_key[16];        // AES key for file
    u32 export_table_offset;
    u8 header_hash[20];     // SHA-1
    u32 game_region;
    u32 image_flags;
};

/**
 * XEX2 execution info
 */
struct XexExecutionInfo {
    u32 media_id;
    u32 version;
    u32 base_version;
    u32 title_id;
    u8 platform;
    u8 executable_type;
    u8 disc_number;
    u8 disc_count;
    u32 savegame_id;
};

/**
 * XEX2 TLS info
 */
struct XexTlsInfo {
    u32 slot_count;
    u32 raw_data_address;
    u32 data_size;
    u32 raw_data_size;
};

/**
 * Single import entry info
 */
struct XexImportEntry {
    u32 ordinal;        // Function ordinal within the library
    u32 thunk_address;  // Address where thunk should be written
};

/**
 * Import library info
 */
struct XexImportLibrary {
    std::string name;
    u32 version_min;
    u32 version;
    u8 digest[20];
    u32 import_count;
    std::vector<XexImportEntry> imports;  // Import entries with ordinal and thunk address
};

/**
 * Export info
 */
struct XexExport {
    u32 ordinal;
    u32 address;
    std::string name;  // If exported by name
};

/**
 * PE Section info
 */
struct XexSection {
    std::string name;
    u32 virtual_address;
    u32 virtual_size;
    u32 raw_offset;
    u32 raw_size;
    u32 flags;
    
    bool is_executable() const { return (flags & 0x20000000) != 0; }
    bool is_readable() const { return (flags & 0x40000000) != 0; }
    bool is_writable() const { return (flags & 0x80000000) != 0; }
};

/**
 * Loaded module info
 */
struct XexModule {
    std::string name;
    std::string path;
    
    // Memory layout
    GuestAddr base_address;
    u32 image_size;
    GuestAddr entry_point;
    
    // Headers
    XexFileHeader file_header;
    XexSecurityInfo security_info;
    XexExecutionInfo execution_info;
    XexTlsInfo tls_info;
    
    // Sections
    std::vector<XexSection> sections;
    
    // Imports and exports
    std::vector<XexImportLibrary> imports;
    std::vector<XexExport> exports;
    
    // Stack/heap configuration
    u32 default_stack_size;
    u32 default_heap_size;
    
    // Loaded image data
    std::vector<u8> image_data;
    
    // Is this the main executable?
    bool is_title;
    
    // Encryption/compression
    u32 encryption_type = 0;  // 0=none, 1=encrypted
    u32 compression_type = 0; // 0=none, 1=basic, 2=LZX

    // Basic compression blocks (data_size, zero_size pairs)
    std::vector<std::pair<u32, u32>> compression_blocks;

    // LZX compression info
    u32 lzx_window_size = 0;  // Window size in bytes (for compression type 2)
    u32 lzx_first_block_offset = 0;  // Offset to first LZX block data
    
    // Module handle for HLE
    u32 handle;
};

/**
 * XEX2 Loader
 * 
 * Parses and loads XEX2 executables into emulator memory.
 */
class XexLoader {
public:
    XexLoader();
    ~XexLoader();
    
    /**
     * Load a XEX file from disk
     */
    Status load_file(const std::string& path, class Memory* memory);
    
    /**
     * Load a XEX from memory buffer
     */
    Status load_buffer(const u8* data, u32 size, const std::string& name,
                       class Memory* memory);
    
    /**
     * Get loaded module info
     */
    const XexModule* get_module() const { return module_.get(); }
    XexModule* get_module() { return module_.get(); }
    
    /**
     * Get entry point address
     */
    GuestAddr get_entry_point() const;
    
    /**
     * Get base address
     */
    GuestAddr get_base_address() const;
    
    /**
     * Get title ID
     */
    u32 get_title_id() const;
    
    /**
     * Resolve import by library name and ordinal
     */
    GuestAddr resolve_import(const std::string& library, u32 ordinal);
    
    /**
     * Get export by ordinal
     */
    GuestAddr get_export(u32 ordinal) const;
    
    /**
     * Get export by name
     */
    GuestAddr get_export_by_name(const std::string& name) const;
    
    /**
     * Apply base relocations
     */
    void apply_relocations(GuestAddr new_base);
    
    /**
     * Get last error message
     */
    const std::string& get_error() const { return error_; }
    
private:
    std::unique_ptr<XexModule> module_;
    std::string error_;
    
    // Parsing methods
    Status parse_headers(const u8* data, u32 size);
    Status parse_optional_headers(const u8* data, u32 size, u32 count);
    Status parse_security_info(const u8* data, u32 offset);
    Status parse_import_libraries(const u8* data, u32 offset, u32 data_size);
    Status parse_pe_image(const u8* data, u32 offset, u32 size);
    
    // Decompression
    Status decompress_image(const u8* compressed, u32 comp_size,
                            u8* decompressed, u32 decomp_size);
    
    // Decryption (for retail discs)
    Status decrypt_image(u8* data, u32 size, const u8* key);
    
    // Helper methods
    u32 read_u32_be(const u8* data);
    u16 read_u16_be(const u8* data);
    std::string read_string(const u8* data, u32 max_len);
};

/**
 * XEX Test Harness
 * 
 * Utility for testing XEX loading and basic execution.
 * Useful for debugging without running the full emulator.
 */
class XexTestHarness {
public:
    XexTestHarness();
    ~XexTestHarness();
    
    /**
     * Initialize the test harness
     */
    Status initialize();
    
    /**
     * Shutdown
     */
    void shutdown();
    
    /**
     * Load a XEX file
     */
    Status load_xex(const std::string& path);
    
    /**
     * Print module information
     */
    void print_module_info() const;
    
    /**
     * Print sections
     */
    void print_sections() const;
    
    /**
     * Print imports
     */
    void print_imports() const;
    
    /**
     * Print exports
     */
    void print_exports() const;
    
    /**
     * Disassemble entry point
     */
    void disassemble_entry(u32 instruction_count = 32) const;
    
    /**
     * Dump memory region
     */
    void dump_memory(GuestAddr address, u32 size) const;
    
    /**
     * Validate image integrity
     */
    bool validate_image() const;
    
    /**
     * Run basic tests
     */
    bool run_tests();
    
    /**
     * Get loader
     */
    XexLoader& get_loader() { return loader_; }
    
    /**
     * Get memory
     */
    class Memory* get_memory() { return memory_.get(); }
    
private:
    XexLoader loader_;
    std::unique_ptr<class Memory> memory_;
    bool initialized_ = false;
};

} // namespace x360mu

