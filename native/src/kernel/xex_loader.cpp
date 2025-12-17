/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XEX2 Loader Implementation
 */

#include "xex_loader.h"
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

// XEX2 magic value
constexpr u32 XEX2_MAGIC = 0x58455832;  // 'XEX2'

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
    
    LOGI("Loaded XEX: %s", name.c_str());
    LOGI("  Base address: 0x%08X", module_->base_address);
    LOGI("  Entry point:  0x%08X", module_->entry_point);
    LOGI("  Image size:   0x%08X", module_->image_size);
    LOGI("  Title ID:     0x%08X", module_->execution_info.title_id);
    
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
    
    // Verify magic
    if (module_->file_header.magic != XEX2_MAGIC) {
        error_ = "Invalid XEX magic";
        LOGE("Invalid magic: 0x%08X (expected 0x%08X)",
             module_->file_header.magic, XEX2_MAGIC);
        return Status::Error;
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
                parse_import_libraries(data, value);
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
    memcpy(module_->security_info.file_key, ptr + 64, 16);
    module_->security_info.export_table_offset = read_u32_be(ptr + 80);
    memcpy(module_->security_info.header_hash, ptr + 84, 20);
    module_->security_info.game_region = read_u32_be(ptr + 104);
    module_->security_info.image_flags = read_u32_be(ptr + 108);
    
    LOGD("Security info: image_size=0x%08X, region=0x%08X, flags=0x%08X",
         module_->security_info.image_size,
         module_->security_info.game_region,
         module_->security_info.image_flags);
    
    return Status::Ok;
}

Status XexLoader::parse_import_libraries(const u8* data, u32 offset) {
    const u8* ptr = data + offset;
    
    u32 string_table_size = read_u32_be(ptr); ptr += 4;
    u32 library_count = read_u32_be(ptr); ptr += 4;
    
    // String table follows
    const char* strings = reinterpret_cast<const char*>(ptr);
    ptr += string_table_size;
    
    LOGD("Import libraries: %u libraries, %u bytes of strings",
         library_count, string_table_size);
    
    for (u32 i = 0; i < library_count; i++) {
        XexImportLibrary lib;
        
        u32 name_offset = read_u32_be(ptr); ptr += 4;
        if (name_offset < string_table_size) {
            lib.name = std::string(strings + name_offset);
        }
        
        // Skip unknown fields
        ptr += 4;  // Unknown
        
        lib.version_min = read_u32_be(ptr); ptr += 4;
        lib.version = read_u32_be(ptr); ptr += 4;
        
        memcpy(lib.digest, ptr, 20); ptr += 20;
        
        lib.import_count = read_u32_be(ptr); ptr += 4;
        
        // Read imports
        for (u32 j = 0; j < lib.import_count; j++) {
            u32 import_addr = read_u32_be(ptr); ptr += 4;
            lib.imports.push_back(import_addr);
        }
        
        LOGD("  Import lib: %s v%u.%u.%u.%u (%u imports)",
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

Status XexLoader::parse_pe_image(const u8* data, u32 offset, u32 size) {
    // The PE image in XEX can be:
    // 1. Uncompressed
    // 2. LZX compressed
    // 3. Basic compressed (simple blocks)
    
    // For now, assume uncompressed and just copy
    module_->image_data.resize(module_->image_size);
    
    if (offset + size > module_->image_size) {
        size = module_->image_size - offset;
    }
    
    // Copy what we have
    memcpy(module_->image_data.data(), data + offset, 
           std::min(size, static_cast<u32>(module_->image_data.size())));
    
    // Try to parse PE headers from the image
    const u8* img = module_->image_data.data();
    
    // DOS header check
    if (module_->image_data.size() >= 64 && img[0] == 'M' && img[1] == 'Z') {
        u32 pe_offset = *reinterpret_cast<const u32*>(img + 60);
        
        if (pe_offset < module_->image_data.size() - 24) {
            // Parse PE header
            const u8* pe = img + pe_offset;
            u32 signature = *reinterpret_cast<const u32*>(pe);
            
            if (signature == PE_SIGNATURE) {
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
                    
                    LOGD("  Section: %-8s VA=0x%08X Size=0x%08X Flags=0x%08X",
                         section.name.c_str(),
                         section.virtual_address,
                         section.virtual_size,
                         section.flags);
                    
                    sections += 40;
                }
            }
        }
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
        
        for (u32 i = 0; i < std::min(lib.import_count, 10u); i++) {
            printf("  0x%08X\n", lib.imports[i]);
        }
        if (lib.import_count > 10) {
            printf("  ... and %u more\n", lib.import_count - 10);
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

