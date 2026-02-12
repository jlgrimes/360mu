/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XEX2 Loader Implementation
 */

#include "xex_loader.h"
#include "xex_crypto.h"
#include "../memory/memory.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-xex"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[XEX] " __VA_ARGS__); printf("\n")
#define LOGW(...) printf("[XEX WARN] " __VA_ARGS__); printf("\n")
#define LOGE(...) printf("[XEX ERROR] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#endif

namespace x360mu {

// XEX magic values
constexpr u32 XEX2_MAGIC = 0x58455832;  // 'XEX2'
constexpr u32 XEX1_MAGIC = 0x58455831;  // 'XEX1' (pre-release format)

// PE signature
constexpr u32 PE_SIGNATURE = 0x00004550;  // 'PE\0\0'

//=============================================================================
// XexLoader Implementation
//=============================================================================

XexLoader::XexLoader() = default;
XexLoader::~XexLoader() = default;

u32 XexLoader::read_u32_be(const u8* data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

u16 XexLoader::read_u16_be(const u8* data) {
    return (data[0] << 8) | data[1];
}

std::string XexLoader::read_string(const u8* data, u32 max_len) {
    std::string result;
    for (u32 i = 0; i < max_len && data[i]; i++) {
        result += static_cast<char>(data[i]);
    }
    return result;
}

Status XexLoader::load_file(const std::string& path, Memory* memory) {
    // Open file
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error_ = "Failed to open file: " + path;
        LOGE("%s", error_.c_str());
        return Status::Error;
    }
    
    // Get file size
    u32 size = static_cast<u32>(file.tellg());
    file.seekg(0);
    
    // Read file
    std::vector<u8> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    file.close();
    
    // Extract filename for module name
    std::string name = path;
    auto pos = name.find_last_of("/\\");
    if (pos != std::string::npos) {
        name = name.substr(pos + 1);
    }
    
    return load_buffer(data.data(), size, name, memory);
}

Status XexLoader::load_buffer(const u8* data, u32 size, const std::string& name,
                               Memory* memory) {
    module_ = std::make_unique<XexModule>();
    module_->name = name;
    module_->path = name;
    
    // Parse XEX headers
    Status status = parse_headers(data, size);
    if (status != Status::Ok) {
        return status;
    }
    
    LOGI("Loaded XEX: %s (format: %s)", name.c_str(),
         module_->file_header.magic == XEX1_MAGIC ? "XEX1" : "XEX2");
    LOGI("  Base address: 0x%08X", module_->base_address);
    LOGI("  Entry point:  0x%08X", module_->entry_point);
    LOGI("  Image size:   0x%08X", module_->image_size);
    LOGI("  Title ID:     0x%08X", module_->execution_info.title_id);
    if (!module_->static_libraries.empty()) {
        LOGI("  Static libs:  %zu", module_->static_libraries.size());
    }
    if (!module_->resources.empty()) {
        LOGI("  Resources:    %zu", module_->resources.size());
    }
    
    // Load image into emulator memory
    if (memory && !module_->image_data.empty()) {
        // Map the image at base address
        for (u32 i = 0; i < module_->image_data.size(); i++) {
            memory->write_u8(module_->base_address + i, module_->image_data[i]);
        }
        
        LOGI("Loaded %zu bytes at 0x%08X", module_->image_data.size(), module_->base_address);
    }
    
    return Status::Ok;
}

Status XexLoader::parse_headers(const u8* data, u32 size) {
    if (size < sizeof(XexFileHeader)) {
        error_ = "File too small for XEX header";
        return Status::Error;
    }
    
    // Read file header
    const u8* ptr = data;
    module_->file_header.magic = read_u32_be(ptr); ptr += 4;
    module_->file_header.module_flags = read_u32_be(ptr); ptr += 4;
    module_->file_header.pe_data_offset = read_u32_be(ptr); ptr += 4;
    module_->file_header.reserved = read_u32_be(ptr); ptr += 4;
    module_->file_header.security_offset = read_u32_be(ptr); ptr += 4;
    module_->file_header.header_count = read_u32_be(ptr); ptr += 4;
    
    // Verify magic (accept both XEX2 and XEX1)
    if (module_->file_header.magic != XEX2_MAGIC &&
        module_->file_header.magic != XEX1_MAGIC) {
        error_ = "Invalid XEX magic";
        LOGE("Invalid magic: 0x%08X (expected 0x%08X or 0x%08X)",
             module_->file_header.magic, XEX2_MAGIC, XEX1_MAGIC);
        return Status::Error;
    }

    if (module_->file_header.magic == XEX1_MAGIC) {
        LOGI("Loading XEX1 (pre-release) format executable");
    }
    
    // Check if this is a title module
    module_->is_title = (module_->file_header.module_flags & 
                         static_cast<u32>(XexModuleFlags::kTitle)) != 0;
    
    LOGD("XEX header: flags=0x%08X, pe_offset=0x%08X, security_offset=0x%08X, headers=%u",
         module_->file_header.module_flags,
         module_->file_header.pe_data_offset,
         module_->file_header.security_offset,
         module_->file_header.header_count);
    
    // Parse optional headers
    Status status = parse_optional_headers(data, size, module_->file_header.header_count);
    if (status != Status::Ok) {
        return status;
    }
    
    // Parse security info
    if (module_->file_header.security_offset < size) {
        status = parse_security_info(data, module_->file_header.security_offset);
        if (status != Status::Ok) {
            return status;
        }
    }
    
    // Parse PE image
    if (module_->file_header.pe_data_offset < size) {
        status = parse_pe_image(data, module_->file_header.pe_data_offset,
                                size - module_->file_header.pe_data_offset);
        if (status != Status::Ok) {
            return status;
        }
    }
    
    return Status::Ok;
}

Status XexLoader::parse_optional_headers(const u8* data, u32 size, u32 count) {
    const u8* ptr = data + 24;  // After file header
    
    for (u32 i = 0; i < count; i++) {
        if (ptr + 8 > data + size) {
            break;
        }
        
        u32 key = read_u32_be(ptr); ptr += 4;
        u32 value = read_u32_be(ptr); ptr += 4;
        
        XexHeaderId header_id = static_cast<XexHeaderId>(key);
        
        switch (header_id) {
            case XexHeaderId::kEntryPoint:
                module_->entry_point = value;
                LOGD("Entry point: 0x%08X", value);
                break;
                
            case XexHeaderId::kImageBaseAddress:
                module_->base_address = value;
                LOGD("Base address: 0x%08X", value);
                break;
                
            case XexHeaderId::kOriginalBaseAddress:
                LOGD("Original base address: 0x%08X", value);
                break;
                
            case XexHeaderId::kDefaultStackSize:
                module_->default_stack_size = value;
                LOGD("Default stack size: 0x%08X", value);
                break;
                
            case XexHeaderId::kDefaultHeapSize:
                module_->default_heap_size = value;
                LOGD("Default heap size: 0x%08X", value);
                break;
                
            case XexHeaderId::kTLSInfo:
                if (value < size - 16) {
                    const u8* tls = data + value;
                    module_->tls_info.slot_count = read_u32_be(tls);
                    module_->tls_info.raw_data_address = read_u32_be(tls + 4);
                    module_->tls_info.data_size = read_u32_be(tls + 8);
                    module_->tls_info.raw_data_size = read_u32_be(tls + 12);
                    LOGD("TLS: slots=%u, addr=0x%08X, size=%u",
                         module_->tls_info.slot_count,
                         module_->tls_info.raw_data_address,
                         module_->tls_info.data_size);
                }
                break;
                
            case XexHeaderId::kExecutionInfo:
                if (value < size - 24) {
                    const u8* exec = data + value;
                    module_->execution_info.media_id = read_u32_be(exec);
                    module_->execution_info.version = read_u32_be(exec + 4);
                    module_->execution_info.base_version = read_u32_be(exec + 8);
                    module_->execution_info.title_id = read_u32_be(exec + 12);
                    module_->execution_info.platform = exec[16];
                    module_->execution_info.executable_type = exec[17];
                    module_->execution_info.disc_number = exec[18];
                    module_->execution_info.disc_count = exec[19];
                    module_->execution_info.savegame_id = read_u32_be(exec + 20);
                    LOGD("Execution info: title_id=0x%08X, version=%u.%u",
                         module_->execution_info.title_id,
                         module_->execution_info.version >> 16,
                         module_->execution_info.version & 0xFFFF);
                }
                break;
                
            case XexHeaderId::kImportLibraries:
                parse_import_libraries(data, value, size);
                break;

            case XexHeaderId::kResourceInfo:
                if (value < size - 4) {
                    const u8* res = data + value;
                    u32 res_size = read_u32_be(res);
                    // Each resource entry: 8 bytes name + 4 bytes address + 4 bytes size = 16 bytes
                    u32 entry_count = (res_size - 4) / 16;
                    const u8* entry = res + 4;
                    for (u32 r = 0; r < entry_count && entry + 16 <= data + size; r++) {
                        XexResource resource;
                        resource.name = read_string(entry, 8);
                        resource.address = read_u32_be(entry + 8);
                        resource.size = read_u32_be(entry + 12);
                        module_->resources.push_back(resource);
                        LOGD("Resource: %s addr=0x%08X size=0x%X",
                             resource.name.c_str(), resource.address, resource.size);
                        entry += 16;
                    }
                }
                break;

            case XexHeaderId::kStaticLibraries:
                if (value < size - 4) {
                    const u8* slib = data + value;
                    u32 slib_size = read_u32_be(slib);
                    // Each entry: 8 bytes name + 2 major + 2 minor + 2 build + 2 qfe + 2 approval = 18 bytes
                    u32 entry_count = (slib_size - 4) / 16;
                    const u8* entry = slib + 4;
                    for (u32 s = 0; s < entry_count && entry + 16 <= data + size; s++) {
                        XexStaticLibrary lib;
                        lib.name = read_string(entry, 8);
                        lib.version_major = read_u16_be(entry + 8);
                        lib.version_minor = read_u16_be(entry + 10);
                        lib.version_build = read_u16_be(entry + 12);
                        lib.version_qfe = (entry[14] >> 2) & 0x3F;
                        lib.approval_type = entry[14] & 0x03;
                        module_->static_libraries.push_back(lib);
                        LOGD("Static lib: %s %u.%u.%u.%u",
                             lib.name.c_str(), lib.version_major,
                             lib.version_minor, lib.version_build, lib.version_qfe);
                        entry += 16;
                    }
                }
                break;

            case XexHeaderId::kChecksumTimestamp:
                if (value < size - 8) {
                    u32 checksum = read_u32_be(data + value);
                    u32 timestamp = read_u32_be(data + value + 4);
                    LOGD("Checksum: 0x%08X, Timestamp: 0x%08X", checksum, timestamp);
                }
                break;

            case XexHeaderId::kSystemFlags:
                LOGD("System flags: 0x%08X", value);
                break;

            case XexHeaderId::kGameRatings:
                LOGD("Game ratings header at 0x%08X", value);
                break;

            case XexHeaderId::kLANKey:
                LOGD("LAN key at 0x%08X", value);
                break;
            
            case XexHeaderId::kBaseFileFormat:
                // File format descriptor
                if (value < size - 8) {
                    const u8* fmt = data + value;
                    u32 info_size = read_u32_be(fmt);
                    u32 enc_comp = read_u32_be(fmt + 4);
                    // High 16 bits = encryption type, Low 16 bits = compression type
                    module_->encryption_type = (enc_comp >> 16) & 0xFFFF;
                    module_->compression_type = enc_comp & 0xFFFF;
                    LOGI("File format: size=%u, encryption=%u, compression=%u",
                         info_size, module_->encryption_type, module_->compression_type);

                    // Parse compression-specific data
                    if (module_->compression_type == 1 && info_size > 8) {
                        // Basic compression: parse blocks (data_size, zero_size pairs)
                        u32 block_count = (info_size - 8) / 8;
                        const u8* blocks = fmt + 8;
                        LOGI("Parsing %u basic compression blocks:", block_count);
                        for (u32 b = 0; b < block_count && blocks + 8 <= data + size; b++) {
                            u32 data_size = read_u32_be(blocks);
                            u32 zero_size = read_u32_be(blocks + 4);
                            module_->compression_blocks.push_back({data_size, zero_size});
                            LOGI("  Block %u: data=%u (0x%X), zeros=%u (0x%X)",
                                 b, data_size, data_size, zero_size, zero_size);
                            blocks += 8;
                        }
                    } else if (module_->compression_type == 2 && info_size >= 12) {
                        // LZX compression: parse window size and first block offset
                        module_->lzx_window_size = read_u32_be(fmt + 8);
                        module_->lzx_first_block_offset = value + 12;
                    }
                }
                break;
                
            case XexHeaderId::kOriginalPeName:
                if (value < size) {
                    u32 name_len = (key >> 8) & 0xFF;
                    module_->name = read_string(data + value, name_len);
                    LOGD("Original PE name: %s", module_->name.c_str());
                }
                break;
                
            default:
                LOGD("Unknown header 0x%08X = 0x%08X", key, value);
                break;
        }
    }
    
    return Status::Ok;
}

Status XexLoader::parse_security_info(const u8* data, u32 offset) {
    const u8* ptr = data + offset;
    
    module_->security_info.header_size = read_u32_be(ptr);
    module_->security_info.image_size = read_u32_be(ptr + 4);
    
    module_->image_size = module_->security_info.image_size;
    
    // RSA signature at offset 8, 256 bytes
    memcpy(module_->security_info.rsa_signature, ptr + 8, 256);
    
    // More security info at offset 264
    ptr += 264;
    module_->security_info.unknown_count = read_u32_be(ptr);
    memcpy(module_->security_info.image_hash, ptr + 4, 20);
    module_->security_info.import_table_count = read_u32_be(ptr + 24);
    memcpy(module_->security_info.import_digest, ptr + 28, 20);
    memcpy(module_->security_info.media_id, ptr + 48, 16);
    memcpy(module_->security_info.file_key, ptr + 72, 16);  // Session key at offset 336 from security start
    module_->security_info.export_table_offset = read_u32_be(ptr + 88);
    memcpy(module_->security_info.header_hash, ptr + 84, 20);
    module_->security_info.game_region = read_u32_be(ptr + 104);
    module_->security_info.image_flags = read_u32_be(ptr + 108);
    
    LOGD("Security info: image_size=0x%08X, region=0x%08X, flags=0x%08X",
         module_->security_info.image_size,
         module_->security_info.game_region,
         module_->security_info.image_flags);
    
    return Status::Ok;
}

Status XexLoader::parse_import_libraries(const u8* data, u32 offset, u32 data_size) {
    if (offset + 12 > data_size) {
        LOGW("Import table offset 0x%08X exceeds data size", offset);
        return Status::Ok;  // Not fatal, just skip imports
    }
    
    const u8* ptr = data + offset;
    const u8* data_end = data + data_size;
    
    // XEX2 Import Header format:
    // - u32 total_size: Size of entire import header including strings
    // - u32 string_table_size: Size of string table
    // - u32 library_count: Number of import libraries
    u32 total_size = read_u32_be(ptr); ptr += 4;
    u32 string_table_size = read_u32_be(ptr); ptr += 4;
    u32 library_count = read_u32_be(ptr); ptr += 4;
    
    // Sanity check
    if (library_count > 100 || string_table_size > 0x10000 || total_size > 0x100000) {
        LOGW("Suspicious import table values: %u libs, %u string bytes, %u total",
             library_count, string_table_size, total_size);
        return Status::Ok;  // Skip imports
    }
    
    LOGI("Import libraries: %u libraries, %u bytes of strings", library_count, string_table_size);
    
    // String table follows header
    const char* strings = reinterpret_cast<const char*>(ptr);
    ptr += string_table_size;
    
    if (ptr >= data_end) {
        LOGW("Import string table exceeds data");
        return Status::Ok;
    }
    
    // Parse each library
    for (u32 i = 0; i < library_count && ptr + 40 <= data_end; i++) {
        XexImportLibrary lib;
        
        // Library record format:
        // - u32 record_size
        // - 20 bytes SHA-1 digest
        // - u32 import_id
        // - u32 version
        // - u32 version_min (optional)
        // - u16 name_index (into string table, multiply by string entry size)
        // - u16 import_count
        // - import addresses follow
        
        u32 record_size = read_u32_be(ptr); ptr += 4;
        
        if (record_size < 28 || ptr + record_size - 4 > data_end) {
            LOGW("Invalid import library record size: %u", record_size);
            break;
        }
        
        const u8* record_start = ptr;
        
        // SHA-1 digest (20 bytes)
        memcpy(lib.digest, ptr, 20); ptr += 20;
        
        // Import ID (used to identify which library)
        u32 import_id = read_u32_be(ptr); ptr += 4;
        
        // Version
        lib.version = read_u32_be(ptr); ptr += 4;
        
        // Version min
        lib.version_min = read_u32_be(ptr); ptr += 4;
        
        // Name index (byte offset into string table) and import count
        u16 name_index = read_u16_be(ptr); ptr += 2;
        lib.import_count = read_u16_be(ptr); ptr += 2;
        
        // Get library name from string table
        // name_index is an entry index into null-terminated strings, not byte offset
        const char* str_ptr = strings;
        const char* str_end = strings + string_table_size;
        u16 current_index = 0;
        while (str_ptr < str_end && current_index < name_index) {
            // Skip to next string
            while (str_ptr < str_end && *str_ptr != '\0') str_ptr++;
            if (str_ptr < str_end) str_ptr++;  // Skip null terminator
            current_index++;
        }
        if (str_ptr < str_end && current_index == name_index) {
            lib.name = std::string(str_ptr);
        } else {
            lib.name = "<unknown>";
        }
        
        // Read import records
        // Each import is 8 bytes: 4 bytes ordinal/type + 4 bytes thunk address
        // Some XEX files use a different format with only 4 bytes per import
        // We'll try to detect and handle both cases
        for (u32 j = 0; j < lib.import_count && ptr + 4 <= data_end; j++) {
            XexImportEntry entry;
            
            // Read the ordinal/type value
            u32 ordinal_value = read_u32_be(ptr); ptr += 4;

            // The ordinal value format (based on Xenia):
            // - High 16 bits (31-16): thunk address offset / table index
            // - Low 16 bits (15-0): actual ordinal number
            entry.ordinal = ordinal_value & 0x0000FFFF;  // Only use low 16 bits

            // Debug: log suspicious ordinals
            if (entry.ordinal > 1000) {
                LOGW("Import ordinal %u (0x%X) is unusually high (raw: 0x%08X)",
                     entry.ordinal, entry.ordinal, ordinal_value);
            }
            
            // Check if there's a thunk address following (8-byte format)
            // The thunk address should be in the executable's address space
            if (ptr + 4 <= data_end) {
                u32 next_value = read_u32_be(ptr);
                // If next value looks like a valid address (in typical XEX range 0x82000000+)
                // or if it's clearly not an ordinal, treat it as thunk address
                if ((next_value >= 0x82000000 && next_value < 0x90000000) ||
                    (next_value >= 0x80000000 && (next_value & 0x00FFFFFF) > 0x1000)) {
                    entry.thunk_address = next_value;
                    ptr += 4;
                } else {
                    // 4-byte format: ordinal only, no thunk address specified
                    // We'll generate thunk addresses later
                    entry.thunk_address = 0;
                }
            } else {
                entry.thunk_address = 0;
            }
            
            lib.imports.push_back(entry);
        }
        
        // Move ptr to next record based on record_size (not import count)
        ptr = record_start - 4 + record_size;
        
        LOGI("  Import lib: %s v%u.%u.%u.%u (%u imports)",
             lib.name.c_str(),
             (lib.version >> 24) & 0xFF,
             (lib.version >> 16) & 0xFF,
             (lib.version >> 8) & 0xFF,
             lib.version & 0xFF,
             lib.import_count);
        
        module_->imports.push_back(std::move(lib));
    }
    
    return Status::Ok;
}

Status XexLoader::parse_pe_image(const u8* data, u32 offset, u32 raw_size) {
    // The PE image in XEX can be:
    // 1. Uncompressed, unencrypted
    // 2. Encrypted (AES-128-CBC)
    // 3. Compressed (LZX or basic)
    // 4. Encrypted AND compressed
    
    LOGI("Parsing PE image: offset=0x%X, raw_size=%u", offset, raw_size);
    
    // Allocate space for the decrypted/decompressed image
    module_->image_data.resize(module_->image_size);
    
    // Make a copy of the encrypted data to work with
    std::vector<u8> working_data(data + offset, data + offset + raw_size);

    // For basic compression, decryption happens block-by-block below
    // For other modes, decrypt the whole image first
    if (module_->encryption_type == 1 && module_->compression_type != 1) {
        LOGI("Decrypting XEX image (%u bytes)...", raw_size);

        XexDecryptor decryptor;

        // Try auto-detecting the correct key type
        // XEX1 format uses XEX1 key, otherwise try retail first then devkit
        XexKeyType detected_key = XexKeyType::Retail;
        if (module_->file_header.magic == XEX1_MAGIC) {
            decryptor.set_key(module_->security_info.file_key, XexKeyType::XEX1);
            LOGI("Using XEX1 key for pre-release format");
        } else if (decryptor.try_keys(working_data.data(),
                                       std::min(raw_size, (u32)64),
                                       module_->security_info.file_key,
                                       detected_key)) {
            LOGI("Auto-detected key type: %s",
                 detected_key == XexKeyType::Retail ? "retail" :
                 detected_key == XexKeyType::DevKit ? "devkit" : "xex1");
            decryptor.set_key(module_->security_info.file_key, detected_key);
        } else {
            LOGW("Key auto-detection failed, trying retail key");
            decryptor.set_key(module_->security_info.file_key, XexKeyType::Retail);
        }

        // Use zero IV for XEX decryption
        u8 iv[16] = {0};

        Status status = decryptor.decrypt_image(working_data.data(), working_data.size(), iv);
        if (status != Status::Ok) {
            LOGE("Failed to decrypt XEX image");
            return status;
        }

        LOGI("Decryption complete (key type: %s)",
             decryptor.get_key_type() == XexKeyType::Retail ? "retail" :
             decryptor.get_key_type() == XexKeyType::DevKit ? "devkit" : "xex1");
    }
    
    // Decompress if needed
    if (module_->compression_type == 2) {
        // LZX compression
        LOGI("Decompressing LZX image...");

        // For LZX compression, the entire decrypted data is the LZX stream
        // No block structure to parse - just decompress directly
        XexDecryptor decryptor;
        Status status = decryptor.decompress_lzx(
            working_data.data(), working_data.size(),
            module_->image_data.data(), module_->image_size,
            module_->lzx_window_size);

        if (status != Status::Ok) {
            LOGW("LZX decompression failed, using raw data");
            // Fall back to copying raw data
            memcpy(module_->image_data.data(), working_data.data(),
                   std::min(working_data.size(), (size_t)module_->image_size));
        } else {
            LOGI("Decompression complete");
        }
    } else if (module_->compression_type == 1) {
        // Basic block compression
        LOGI("Decompressing basic blocks...");
        
        // For basic compression with encryption, we need to decrypt each block
        // and then append zeros as specified in the block table
        
        if (module_->compression_blocks.empty()) {
            LOGW("No compression blocks found, copying raw data");
            memcpy(module_->image_data.data(), working_data.data(),
                   std::min(working_data.size(), (size_t)module_->image_size));
        } else {
            XexDecryptor decryptor;
            // Use appropriate key for basic compression too
            if (module_->file_header.magic == XEX1_MAGIC) {
                decryptor.set_key(module_->security_info.file_key, XexKeyType::XEX1);
            } else {
                decryptor.set_key(module_->security_info.file_key, XexKeyType::Retail);
            }

            u32 src_offset = 0;
            u32 dst_offset = 0;
            u8 iv[16] = {0};  // IV chains across all blocks
            
            for (size_t i = 0; i < module_->compression_blocks.size() && dst_offset < module_->image_size; i++) {
                u32 data_size = module_->compression_blocks[i].first;
                u32 zero_size = module_->compression_blocks[i].second;
                
                if (src_offset + data_size > working_data.size()) {
                    LOGW("Block %zu exceeds input data (offset=%u, size=%u, avail=%zu)", 
                         i, src_offset, data_size, working_data.size());
                    break;
                }
                
                // Copy and decrypt block
                if (data_size > 0) {
                    // Round up to 16-byte boundary for AES
                    u32 aligned_size = (data_size + 15) & ~15;
                    
                    // Make a copy for decryption
                    std::vector<u8> block_data(working_data.begin() + src_offset,
                                               working_data.begin() + src_offset + std::min(aligned_size, (u32)working_data.size() - src_offset));
                    
                    // Pad to aligned size if needed
                    if (block_data.size() < aligned_size) {
                        block_data.resize(aligned_size, 0);
                    }
                    
                    if (module_->encryption_type == 1) {
                        // Save last 16 bytes of ciphertext for IV chain BEFORE decryption
                        // Use the actual data_size boundary for IV, aligned to 16 bytes
                        u8 next_iv[16];
                        u32 iv_offset = ((data_size + 15) & ~15) - 16;  // Last 16-byte block
                        if (iv_offset + 16 <= working_data.size() - src_offset) {
                            memcpy(next_iv, working_data.data() + src_offset + iv_offset, 16);
                        }
                        
                        // Decrypt with current IV
                        decryptor.decrypt_image(block_data.data(), aligned_size, iv);
                        
                        // Update IV for next block
                        memcpy(iv, next_iv, 16);
                    }
                    
                    // Copy decrypted data to output (only up to data_size, not padding)
                    u32 copy_size = std::min(data_size, module_->image_size - dst_offset);
                    memcpy(module_->image_data.data() + dst_offset, block_data.data(), copy_size);
                    dst_offset += copy_size;
                    src_offset += data_size;
                }
                
                // Append zeros
                if (zero_size > 0 && dst_offset < module_->image_size) {
                    u32 zeros_to_write = std::min(zero_size, module_->image_size - dst_offset);
                    memset(module_->image_data.data() + dst_offset, 0, zeros_to_write);
                    dst_offset += zeros_to_write;
                }
            }
            
            LOGI("Decompressed %u bytes to %u bytes", src_offset, dst_offset);
            
            // Debug: Check first 32 bytes of decompressed data
            if (module_->image_data.size() >= 32) {
                LOGI("First 32 decompressed bytes: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                     module_->image_data[0], module_->image_data[1], module_->image_data[2], module_->image_data[3],
                     module_->image_data[4], module_->image_data[5], module_->image_data[6], module_->image_data[7],
                     module_->image_data[8], module_->image_data[9], module_->image_data[10], module_->image_data[11],
                     module_->image_data[12], module_->image_data[13], module_->image_data[14], module_->image_data[15]);
            }
            
            // Debug: Check data at entry point offset
            u32 ep_offset = module_->entry_point - module_->base_address;
            if (ep_offset + 16 <= module_->image_data.size()) {
                LOGI("Data at entry point offset 0x%X: %02X%02X%02X%02X %02X%02X%02X%02X",
                     ep_offset,
                     module_->image_data[ep_offset], module_->image_data[ep_offset+1],
                     module_->image_data[ep_offset+2], module_->image_data[ep_offset+3],
                     module_->image_data[ep_offset+4], module_->image_data[ep_offset+5],
                     module_->image_data[ep_offset+6], module_->image_data[ep_offset+7]);
            }
        }
    } else if (module_->compression_type == 3) {
        // Delta compression
        LOGI("Decompressing delta patch...");
        XexDecryptor decryptor;
        // Delta compression requires a base image - for now warn and copy raw
        LOGW("Delta compression requires base image, not yet supported at load time");
        memcpy(module_->image_data.data(), working_data.data(),
               std::min(working_data.size(), (size_t)module_->image_size));
    } else {
        // No compression, just copy
        memcpy(module_->image_data.data(), working_data.data(),
               std::min(working_data.size(), (size_t)module_->image_size));
    }

    // Verify image hash if available
    {
        Sha1 sha;
        u8 computed_hash[20];
        Sha1::hash(module_->image_data.data(), module_->image_size, computed_hash);
        if (memcmp(computed_hash, module_->security_info.image_hash, 20) == 0) {
            LOGI("Image hash verification: PASSED");
        } else {
            LOGW("Image hash verification: FAILED (image may still be usable)");
        }
    }

    // Step 3: Parse PE headers from the decrypted/decompressed image
    const u8* img = module_->image_data.data();
    
    // DOS header check
    if (module_->image_data.size() >= 64 && img[0] == 'M' && img[1] == 'Z') {
        LOGI("Found valid DOS header (MZ)");
        u32 pe_offset = *reinterpret_cast<const u32*>(img + 60);
        
        if (pe_offset < module_->image_data.size() - 24) {
            // Parse PE header
            const u8* pe = img + pe_offset;
            u32 signature = *reinterpret_cast<const u32*>(pe);
            
            if (signature == PE_SIGNATURE) {
                LOGI("Found valid PE signature");
                // COFF header
                u16 num_sections = *reinterpret_cast<const u16*>(pe + 6);
                u16 opt_header_size = *reinterpret_cast<const u16*>(pe + 20);
                
                // Section headers follow optional header
                const u8* sections = pe + 24 + opt_header_size;
                
                for (u16 i = 0; i < num_sections; i++) {
                    if (sections + 40 > img + module_->image_data.size()) break;
                    
                    XexSection section;
                    section.name = read_string(sections, 8);
                    section.virtual_size = *reinterpret_cast<const u32*>(sections + 8);
                    section.virtual_address = *reinterpret_cast<const u32*>(sections + 12);
                    section.raw_size = *reinterpret_cast<const u32*>(sections + 16);
                    section.raw_offset = *reinterpret_cast<const u32*>(sections + 20);
                    section.flags = *reinterpret_cast<const u32*>(sections + 36);
                    
                    module_->sections.push_back(section);
                    
                    LOGI("  Section: %-8s VA=0x%08X Size=0x%08X Flags=0x%08X",
                         section.name.c_str(),
                         section.virtual_address,
                         section.virtual_size,
                         section.flags);
                    
                    sections += 40;
                }
            } else {
                LOGW("No PE signature found (got 0x%08X), image may still be encrypted", signature);
            }
        }
    } else {
        LOGW("No DOS header found, image may still be encrypted (first bytes: %02X %02X)",
             img[0], img[1]);
    }
    
    return Status::Ok;
}

GuestAddr XexLoader::get_entry_point() const {
    return module_ ? module_->entry_point : 0;
}

GuestAddr XexLoader::get_base_address() const {
    return module_ ? module_->base_address : 0;
}

u32 XexLoader::get_title_id() const {
    return module_ ? module_->execution_info.title_id : 0;
}

GuestAddr XexLoader::resolve_import(const std::string& library, u32 ordinal) {
    // This would be implemented by the kernel HLE to map imports
    // to emulated function handlers
    return 0;
}

GuestAddr XexLoader::get_export(u32 ordinal) const {
    if (!module_) return 0;
    
    for (const auto& exp : module_->exports) {
        if (exp.ordinal == ordinal) {
            return exp.address;
        }
    }
    return 0;
}

GuestAddr XexLoader::get_export_by_name(const std::string& name) const {
    if (!module_) return 0;
    
    for (const auto& exp : module_->exports) {
        if (exp.name == name) {
            return exp.address;
        }
    }
    return 0;
}

//=============================================================================
// XexTestHarness Implementation
//=============================================================================

XexTestHarness::XexTestHarness() = default;

XexTestHarness::~XexTestHarness() {
    shutdown();
}

Status XexTestHarness::initialize() {
    // Create memory subsystem
    memory_ = std::make_unique<Memory>();
    
    Status status = memory_->initialize();
    if (status != Status::Ok) {
        LOGE("Failed to initialize memory");
        return status;
    }
    
    initialized_ = true;
    LOGI("XEX Test Harness initialized");
    return Status::Ok;
}

void XexTestHarness::shutdown() {
    if (!initialized_) return;
    
    if (memory_) {
        memory_->shutdown();
        memory_.reset();
    }
    
    initialized_ = false;
}

Status XexTestHarness::load_xex(const std::string& path) {
    if (!initialized_) {
        if (initialize() != Status::Ok) {
            return Status::Error;
        }
    }
    
    return loader_.load_file(path, memory_.get());
}

void XexTestHarness::print_module_info() const {
    const auto* mod = loader_.get_module();
    if (!mod) {
        printf("No module loaded\n");
        return;
    }
    
    printf("\n=== XEX Module Info ===\n");
    printf("Name:           %s\n", mod->name.c_str());
    printf("Base Address:   0x%08X\n", mod->base_address);
    printf("Entry Point:    0x%08X\n", mod->entry_point);
    printf("Image Size:     0x%08X (%u KB)\n", mod->image_size, mod->image_size / 1024);
    printf("Title ID:       0x%08X\n", mod->execution_info.title_id);
    printf("Version:        %u.%u.%u.%u\n",
           (mod->execution_info.version >> 24) & 0xFF,
           (mod->execution_info.version >> 16) & 0xFF,
           (mod->execution_info.version >> 8) & 0xFF,
           mod->execution_info.version & 0xFF);
    printf("Is Title:       %s\n", mod->is_title ? "Yes" : "No");
    printf("Stack Size:     0x%08X\n", mod->default_stack_size);
    printf("Heap Size:      0x%08X\n", mod->default_heap_size);
    printf("Encryption:     %s\n", mod->encryption_type == 0 ? "None" :
           mod->encryption_type == 1 ? "Normal" : "Unknown");
    printf("Compression:    %s\n", mod->compression_type == 0 ? "None" :
           mod->compression_type == 1 ? "Basic" :
           mod->compression_type == 2 ? "LZX" :
           mod->compression_type == 3 ? "Delta" : "Unknown");

    if (!mod->static_libraries.empty()) {
        printf("\nStatic Libraries (%zu):\n", mod->static_libraries.size());
        for (const auto& lib : mod->static_libraries) {
            printf("  %-8s %u.%u.%u.%u\n", lib.name.c_str(),
                   lib.version_major, lib.version_minor,
                   lib.version_build, lib.version_qfe);
        }
    }

    if (!mod->resources.empty()) {
        printf("\nResources (%zu):\n", mod->resources.size());
        for (const auto& res : mod->resources) {
            printf("  %-8s addr=0x%08X size=0x%X\n",
                   res.name.c_str(), res.address, res.size);
        }
    }

    printf("\n");
}

void XexTestHarness::print_sections() const {
    const auto* mod = loader_.get_module();
    if (!mod) return;
    
    printf("\n=== Sections (%zu) ===\n", mod->sections.size());
    printf("%-8s  %-10s  %-10s  %-10s  Flags\n", "Name", "VA", "VSize", "RawSize");
    printf("--------  ----------  ----------  ----------  --------\n");
    
    for (const auto& sec : mod->sections) {
        printf("%-8s  0x%08X  0x%08X  0x%08X  0x%08X",
               sec.name.c_str(),
               sec.virtual_address,
               sec.virtual_size,
               sec.raw_size,
               sec.flags);
        
        if (sec.is_executable()) printf(" X");
        if (sec.is_readable()) printf(" R");
        if (sec.is_writable()) printf(" W");
        printf("\n");
    }
    printf("\n");
}

void XexTestHarness::print_imports() const {
    const auto* mod = loader_.get_module();
    if (!mod) return;
    
    printf("\n=== Imports (%zu libraries) ===\n", mod->imports.size());
    
    for (const auto& lib : mod->imports) {
        printf("\n%s v%u.%u.%u.%u (%u imports)\n",
               lib.name.c_str(),
               (lib.version >> 24) & 0xFF,
               (lib.version >> 16) & 0xFF,
               (lib.version >> 8) & 0xFF,
               lib.version & 0xFF,
               lib.import_count);
        
        for (u32 i = 0; i < std::min((u32)lib.imports.size(), 10u); i++) {
            printf("  ordinal=%4u thunk=0x%08X\n", 
                   lib.imports[i].ordinal, lib.imports[i].thunk_address);
        }
        if (lib.imports.size() > 10) {
            printf("  ... and %zu more\n", lib.imports.size() - 10);
        }
    }
    printf("\n");
}

void XexTestHarness::print_exports() const {
    const auto* mod = loader_.get_module();
    if (!mod) return;
    
    printf("\n=== Exports (%zu) ===\n", mod->exports.size());
    
    for (const auto& exp : mod->exports) {
        printf("  Ordinal %4u: 0x%08X", exp.ordinal, exp.address);
        if (!exp.name.empty()) {
            printf(" (%s)", exp.name.c_str());
        }
        printf("\n");
    }
    printf("\n");
}

void XexTestHarness::disassemble_entry(u32 instruction_count) const {
    const auto* mod = loader_.get_module();
    if (!mod || mod->image_data.empty()) return;
    
    printf("\n=== Disassembly at Entry Point 0x%08X ===\n", mod->entry_point);
    
    GuestAddr addr = mod->entry_point;
    GuestAddr base = mod->base_address;
    
    for (u32 i = 0; i < instruction_count; i++) {
        u32 offset = addr - base;
        if (offset + 4 > mod->image_data.size()) break;
        
        // Read instruction (big-endian)
        const u8* ptr = mod->image_data.data() + offset;
        u32 inst = (ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3];
        
        // Basic PowerPC disassembly
        printf("0x%08X:  %08X  ", addr, inst);
        
        // Decode opcode
        u32 opcode = (inst >> 26) & 0x3F;
        
        switch (opcode) {
            case 18:  // Branch
                printf("b       0x%08X", (inst & 0x03FFFFFC) | (addr & 0xFC000000));
                break;
            case 16:  // Branch conditional
                printf("bc      ...");
                break;
            case 19:  // System
                if ((inst & 0x7FF) == 0x20) printf("bclr");
                else if ((inst & 0x7FF) == 0x420) printf("bctr");
                else printf("sys     ...");
                break;
            case 31:  // Integer ops
                printf("int     ...");
                break;
            case 32:  // lwz
                printf("lwz     r%u, %d(r%u)", (inst >> 21) & 0x1F,
                       static_cast<s16>(inst & 0xFFFF), (inst >> 16) & 0x1F);
                break;
            case 36:  // stw
                printf("stw     r%u, %d(r%u)", (inst >> 21) & 0x1F,
                       static_cast<s16>(inst & 0xFFFF), (inst >> 16) & 0x1F);
                break;
            case 14:  // addi
                printf("addi    r%u, r%u, %d", (inst >> 21) & 0x1F,
                       (inst >> 16) & 0x1F, static_cast<s16>(inst & 0xFFFF));
                break;
            case 15:  // addis
                printf("addis   r%u, r%u, 0x%04X", (inst >> 21) & 0x1F,
                       (inst >> 16) & 0x1F, inst & 0xFFFF);
                break;
            default:
                printf("???     (opcode %u)", opcode);
                break;
        }
        printf("\n");
        
        addr += 4;
    }
    printf("\n");
}

void XexTestHarness::dump_memory(GuestAddr address, u32 size) const {
    if (!memory_) return;
    
    printf("\n=== Memory Dump 0x%08X - 0x%08X ===\n", address, address + size);
    
    for (u32 offset = 0; offset < size; offset += 16) {
        printf("%08X: ", address + offset);
        
        // Hex
        for (u32 i = 0; i < 16 && offset + i < size; i++) {
            printf("%02X ", memory_->read_u8(address + offset + i));
        }
        
        printf(" ");
        
        // ASCII
        for (u32 i = 0; i < 16 && offset + i < size; i++) {
            u8 c = memory_->read_u8(address + offset + i);
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        
        printf("\n");
    }
    printf("\n");
}

bool XexTestHarness::validate_image() const {
    const auto* mod = loader_.get_module();
    if (!mod) {
        printf("No module loaded\n");
        return false;
    }
    
    printf("\n=== Image Validation ===\n");
    
    bool valid = true;
    
    // Check base address
    if (mod->base_address == 0) {
        printf("ERROR: Base address is 0\n");
        valid = false;
    } else {
        printf("OK: Base address = 0x%08X\n", mod->base_address);
    }
    
    // Check entry point
    if (mod->entry_point == 0) {
        printf("ERROR: Entry point is 0\n");
        valid = false;
    } else if (mod->entry_point < mod->base_address ||
               mod->entry_point >= mod->base_address + mod->image_size) {
        printf("ERROR: Entry point 0x%08X outside image\n", mod->entry_point);
        valid = false;
    } else {
        printf("OK: Entry point = 0x%08X\n", mod->entry_point);
    }
    
    // Check image size
    if (mod->image_size == 0) {
        printf("ERROR: Image size is 0\n");
        valid = false;
    } else {
        printf("OK: Image size = 0x%08X\n", mod->image_size);
    }
    
    // Check sections
    if (mod->sections.empty()) {
        printf("WARNING: No sections found\n");
    } else {
        printf("OK: %zu sections\n", mod->sections.size());
    }
    
    printf("\nValidation: %s\n\n", valid ? "PASSED" : "FAILED");
    return valid;
}

bool XexTestHarness::run_tests() {
    printf("\n=== Running XEX Tests ===\n");
    
    const auto* mod = loader_.get_module();
    if (!mod) {
        printf("FAIL: No module loaded\n");
        return false;
    }
    
    int passed = 0;
    int failed = 0;
    
    // Test 1: Basic validation
    printf("Test 1: Basic validation... ");
    if (validate_image()) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
        failed++;
    }
    
    // Test 2: Memory mapping
    printf("Test 2: Memory mapping... ");
    if (memory_ && mod->base_address != 0) {
        u32 test_val = memory_->read_u32(mod->base_address);
        if (test_val != 0) {
            printf("PASS (first dword = 0x%08X)\n", test_val);
            passed++;
        } else {
            printf("FAIL (memory not mapped)\n");
            failed++;
        }
    } else {
        printf("SKIP\n");
    }
    
    // Test 3: Entry point readable
    printf("Test 3: Entry point readable... ");
    if (memory_ && mod->entry_point != 0) {
        u32 entry_inst = memory_->read_u32(mod->entry_point);
        printf("PASS (first instruction = 0x%08X)\n", entry_inst);
        passed++;
    } else {
        printf("SKIP\n");
    }
    
    printf("\nResults: %d passed, %d failed\n\n", passed, failed);
    return failed == 0;
}

} // namespace x360mu

