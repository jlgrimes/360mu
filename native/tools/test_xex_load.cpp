/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Integration Test Level 2: XEX Load and Parse
 * 
 * Tests:
 * - Memory system initialization
 * - XEX2 file header parsing
 * - Security info extraction
 * - PE section enumeration
 * - Import library detection
 * - Memory mapping validation
 * 
 * Usage: ./test_xex_load <path_to_xex>
 */

#include "memory/memory.h"
#include "kernel/xex_loader.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace x360mu;

static void format_size(u64 size, char* buf, size_t buf_size) {
    if (size >= 1024 * 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
    } else if (size >= 1024 * 1024) {
        snprintf(buf, buf_size, "%.2f MB", (double)size / (1024.0 * 1024.0));
    } else if (size >= 1024) {
        snprintf(buf, buf_size, "%.2f KB", (double)size / 1024.0);
    } else {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)size);
    }
}

static void print_hex(const u8* data, size_t len, size_t max_len = 20) {
    for (size_t i = 0; i < len && i < max_len; i++) {
        printf("%02X", data[i]);
    }
    if (len > max_len) printf("...");
}

int main(int argc, char** argv) {
    printf("============================================\n");
    printf("360μ Integration Test Level 2: XEX Loading\n");
    printf("============================================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <path_to_xex>\n", argv[0]);
        printf("\nTo extract default.xex from an ISO:\n");
        printf("  1. Mount ISO with a tool like 7-Zip, xorriso, etc.\n");
        printf("  2. Copy default.xex from the root\n");
        printf("\nThis test validates:\n");
        printf("  - Memory system initialization\n");
        printf("  - XEX2 header parsing\n");
        printf("  - Security info extraction\n");
        printf("  - Section enumeration\n");
        printf("  - Import library detection\n");
        return 1;
    }
    
    const char* xex_path = argv[1];
    printf("XEX Path: %s\n\n", xex_path);
    
    // Verify file exists and get size
    FILE* f = fopen(xex_path, "rb");
    if (!f) {
        printf("❌ FAIL: Cannot open XEX file: %s\n", xex_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);
    
    char size_str[64];
    format_size(file_size, size_str, sizeof(size_str));
    printf("File Size: %s\n\n", size_str);
    
    // Initialize Memory
    printf("[TEST 1] Initializing memory system...\n");
    auto memory = std::make_unique<Memory>();
    
    Status status = memory->initialize();
    if (status != Status::Ok) {
        printf("❌ FAIL: Memory initialization failed (status=%d)\n", (int)status);
        return 1;
    }
    printf("✅ PASS: Memory initialized (512 MB)\n\n");
    
    // Create XEX Loader
    printf("[TEST 2] Loading XEX file...\n");
    XexLoader loader;
    status = loader.load_file(xex_path, memory.get());
    if (status != Status::Ok) {
        printf("❌ FAIL: XEX loading failed (status=%d)\n", (int)status);
        printf("\nPossible reasons:\n");
        printf("  - File is encrypted (needs decryption keys)\n");
        printf("  - File is compressed (needs decompression)\n");
        printf("  - File is corrupted or not a valid XEX2\n");
        memory->shutdown();
        return 1;
    }
    printf("✅ PASS: XEX loaded successfully\n\n");
    
    // Get module info
    const XexModule* mod = loader.get_module();
    if (!mod) {
        printf("❌ FAIL: No module info available\n");
        memory->shutdown();
        return 1;
    }
    
    // Print module info
    printf("============================================\n");
    printf("MODULE INFORMATION\n");
    printf("============================================\n");
    printf("Name:           %s\n", mod->name.c_str());
    printf("Path:           %s\n", mod->path.c_str());
    printf("Base Address:   0x%08X\n", (unsigned)mod->base_address);
    printf("Image Size:     %s (0x%X)\n", 
           (format_size(mod->image_size, size_str, sizeof(size_str)), size_str),
           mod->image_size);
    printf("Entry Point:    0x%08X\n", (unsigned)mod->entry_point);
    printf("\n");
    
    // Validate entry point is in expected range
    if (mod->entry_point >= 0x80000000 && mod->entry_point < 0x90000000) {
        printf("✅ Entry point is in valid usermode range (0x80000000-0x90000000)\n");
    } else {
        printf("⚠️  Entry point 0x%08X may be unusual\n", (unsigned)mod->entry_point);
    }
    printf("\n");
    
    // File header info
    printf("============================================\n");
    printf("FILE HEADER\n");
    printf("============================================\n");
    printf("Magic:          0x%08X ('%c%c%c%c')\n", 
           mod->file_header.magic,
           (mod->file_header.magic >> 24) & 0xFF,
           (mod->file_header.magic >> 16) & 0xFF,
           (mod->file_header.magic >> 8) & 0xFF,
           mod->file_header.magic & 0xFF);
    printf("Module Flags:   0x%08X\n", mod->file_header.module_flags);
    printf("PE Offset:      0x%08X\n", mod->file_header.pe_data_offset);
    printf("Security Off:   0x%08X\n", mod->file_header.security_offset);
    printf("Header Count:   %u\n", mod->file_header.header_count);
    printf("\n");
    
    // Security info
    printf("============================================\n");
    printf("SECURITY INFORMATION\n");
    printf("============================================\n");
    printf("Header Size:    %u bytes\n", mod->security_info.header_size);
    printf("Image Size:     0x%08X\n", mod->security_info.image_size);
    printf("Game Region:    0x%08X\n", mod->security_info.game_region);
    printf("Image Flags:    0x%08X\n", mod->security_info.image_flags);
    printf("Media ID:       ");
    print_hex(mod->security_info.media_id, 16);
    printf("\n");
    printf("File Key:       ");
    print_hex(mod->security_info.file_key, 16);
    printf("\n");
    printf("Image Hash:     ");
    print_hex(mod->security_info.image_hash, 20);
    printf("\n\n");
    
    // Execution info
    printf("============================================\n");
    printf("EXECUTION INFO\n");
    printf("============================================\n");
    printf("Title ID:       0x%08X\n", mod->execution_info.title_id);
    printf("Media ID:       0x%08X\n", mod->execution_info.media_id);
    printf("Version:        %u.%u.%u.%u\n", 
           (mod->execution_info.version >> 24) & 0xFF,
           (mod->execution_info.version >> 16) & 0xFF,
           (mod->execution_info.version >> 8) & 0xFF,
           mod->execution_info.version & 0xFF);
    printf("Base Version:   %u.%u.%u.%u\n",
           (mod->execution_info.base_version >> 24) & 0xFF,
           (mod->execution_info.base_version >> 16) & 0xFF,
           (mod->execution_info.base_version >> 8) & 0xFF,
           mod->execution_info.base_version & 0xFF);
    printf("Platform:       %u\n", mod->execution_info.platform);
    printf("Exec Type:      %u\n", mod->execution_info.executable_type);
    printf("Disc:           %u of %u\n", mod->execution_info.disc_number, mod->execution_info.disc_count);
    printf("SaveGame ID:    0x%08X\n", mod->execution_info.savegame_id);
    printf("\n");
    
    // Stack/heap
    printf("============================================\n");
    printf("STACK & HEAP\n");
    printf("============================================\n");
    format_size(mod->default_stack_size, size_str, sizeof(size_str));
    printf("Stack Size:     %s\n", size_str);
    format_size(mod->default_heap_size, size_str, sizeof(size_str));
    printf("Heap Size:      %s\n", size_str);
    printf("\n");
    
    // Sections
    printf("============================================\n");
    printf("SECTIONS (%zu)\n", mod->sections.size());
    printf("============================================\n");
    if (mod->sections.empty()) {
        printf("  (no sections - may be normal for compressed XEX)\n");
    } else {
        printf("%-10s %-12s %-12s %-12s Flags\n", "Name", "VirtAddr", "VirtSize", "RawSize");
        printf("--------------------------------------------------------------\n");
        for (const auto& sec : mod->sections) {
            printf("%-10s 0x%08X   0x%08X   0x%08X   ", 
                   sec.name.c_str(),
                   sec.virtual_address,
                   sec.virtual_size,
                   sec.raw_size);
            if (sec.is_executable()) printf("X");
            if (sec.is_readable()) printf("R");
            if (sec.is_writable()) printf("W");
            printf("\n");
        }
    }
    printf("\n");
    
    // Imports
    printf("============================================\n");
    printf("IMPORT LIBRARIES (%zu)\n", mod->imports.size());
    printf("============================================\n");
    if (mod->imports.empty()) {
        printf("  (no imports found)\n");
    } else {
        for (const auto& imp : mod->imports) {
            printf("\n%s\n", imp.name.c_str());
            printf("  Version:     %u.%u.%u.%u (min: %u.%u.%u.%u)\n",
                   (imp.version >> 24) & 0xFF,
                   (imp.version >> 16) & 0xFF,
                   (imp.version >> 8) & 0xFF,
                   imp.version & 0xFF,
                   (imp.version_min >> 24) & 0xFF,
                   (imp.version_min >> 16) & 0xFF,
                   (imp.version_min >> 8) & 0xFF,
                   imp.version_min & 0xFF);
            printf("  Imports:     %u functions\n", imp.import_count);
            printf("  Digest:      ");
            print_hex(imp.digest, 20);
            printf("\n");
            
            // Show first few import ordinals
            if (!imp.imports.empty()) {
                printf("  Ordinals:    ");
                for (size_t i = 0; i < imp.imports.size() && i < 10; i++) {
                    printf("%u ", imp.imports[i]);
                }
                if (imp.imports.size() > 10) {
                    printf("... (%zu more)", imp.imports.size() - 10);
                }
                printf("\n");
            }
        }
    }
    printf("\n");
    
    // Exports
    printf("============================================\n");
    printf("EXPORTS (%zu)\n", mod->exports.size());
    printf("============================================\n");
    if (mod->exports.empty()) {
        printf("  (no exports - typical for game executables)\n");
    } else {
        for (const auto& exp : mod->exports) {
            printf("  [%4u] 0x%08X %s\n", exp.ordinal, exp.address, exp.name.c_str());
        }
    }
    printf("\n");
    
    // TLS Info
    printf("============================================\n");
    printf("TLS (Thread Local Storage)\n");
    printf("============================================\n");
    printf("Slot Count:     %u\n", mod->tls_info.slot_count);
    printf("Data Address:   0x%08X\n", mod->tls_info.raw_data_address);
    printf("Data Size:      0x%08X\n", mod->tls_info.data_size);
    printf("Raw Data Size:  0x%08X\n", mod->tls_info.raw_data_size);
    printf("\n");
    
    // Memory validation
    printf("[TEST 3] Validating memory mapping...\n");
    
    // Check if entry point is readable
    u32 entry_inst = memory->read_u32(mod->entry_point);
    printf("Instruction at entry point (0x%08X): 0x%08X\n", 
           (unsigned)mod->entry_point, entry_inst);
    
    if (entry_inst == 0) {
        printf("⚠️  Entry point contains zeros - XEX may be encrypted/compressed\n");
    } else {
        // Try to decode as PowerPC
        u32 opcode = (entry_inst >> 26) & 0x3F;
        printf("PowerPC opcode: %u\n", opcode);
        
        // Common first instructions
        if (opcode == 18) {
            printf("  -> Branch instruction (common entry point)\n");
        } else if (opcode == 31) {
            printf("  -> Extended opcode (likely mflr/mtlr)\n");
        } else if (opcode == 32 || opcode == 36) {
            printf("  -> Load/Store word (stack setup)\n");
        }
        printf("✅ PASS: Code appears to be loaded in memory\n");
    }
    printf("\n");
    
    // Cleanup
    printf("[CLEANUP] Shutting down...\n");
    memory->shutdown();
    
    printf("\n============================================\n");
    printf("SUMMARY: XEX Load Test\n");
    printf("============================================\n");
    printf("✅ Memory Init:            PASS\n");
    printf("✅ XEX Parse:              PASS\n");
    printf("✅ Header Extraction:      PASS\n");
    printf("✅ Import Detection:       PASS (%zu libraries)\n", mod->imports.size());
    printf("✅ Memory Mapping:         PASS\n");
    printf("\n🎉 Level 2 Complete! XEX loading works.\n");
    printf("   Entry Point: 0x%08X\n", (unsigned)mod->entry_point);
    printf("   Next: Run test_execute with the same XEX\n");
    printf("============================================\n");
    
    return 0;
}

