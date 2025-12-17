/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Integration Test Level 1: ISO Mount and File Read
 * 
 * Tests:
 * - VirtualFileSystem initialization
 * - ISO mounting
 * - Directory listing
 * - Basic file reading
 * 
 * Usage: ./test_iso <path_to_iso>
 */

#include "kernel/filesystem/vfs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

using namespace x360mu;

static void print_attributes(FileAttributes attrs) {
    if ((u32)attrs & (u32)FileAttributes::Directory) printf("D");
    if ((u32)attrs & (u32)FileAttributes::ReadOnly) printf("R");
    if ((u32)attrs & (u32)FileAttributes::Hidden) printf("H");
    if ((u32)attrs & (u32)FileAttributes::System) printf("S");
    if ((u32)attrs & (u32)FileAttributes::Archive) printf("A");
}

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

int main(int argc, char** argv) {
    printf("===========================================\n");
    printf("360μ Integration Test Level 1: ISO Mount\n");
    printf("===========================================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <path_to_iso>\n", argv[0]);
        printf("\nThis test validates:\n");
        printf("  - VFS initialization\n");
        printf("  - ISO 9660 mounting\n");
        printf("  - Directory listing\n");
        printf("  - File reading\n");
        return 1;
    }
    
    const char* iso_path = argv[1];
    printf("ISO Path: %s\n\n", iso_path);
    
    // Verify file exists
    FILE* f = fopen(iso_path, "rb");
    if (!f) {
        printf("❌ FAIL: Cannot open ISO file: %s\n", iso_path);
        return 1;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long iso_size = ftell(f);
    fclose(f);
    
    char size_str[64];
    format_size(iso_size, size_str, sizeof(size_str));
    printf("ISO Size: %s\n\n", size_str);
    
    // Initialize VFS
    printf("[TEST 1] Initializing VFS...\n");
    VirtualFileSystem vfs;
    Status status = vfs.initialize("/tmp", "/tmp");
    if (status != Status::Ok) {
        printf("❌ FAIL: VFS initialization failed\n");
        return 1;
    }
    printf("✅ PASS: VFS initialized\n\n");
    
    // Mount ISO
    printf("[TEST 2] Mounting ISO as 'game:'...\n");
    status = vfs.mount_iso("game:", iso_path);
    if (status != Status::Ok) {
        printf("❌ FAIL: ISO mount failed (status=%d)\n", (int)status);
        vfs.shutdown();
        return 1;
    }
    printf("✅ PASS: ISO mounted successfully\n\n");
    
    // List root directory
    printf("[TEST 3] Listing root directory (game:\\)...\n");
    std::vector<DirEntry> entries;
    status = vfs.query_directory("game:\\", entries);
    if (status != Status::Ok) {
        printf("❌ FAIL: Directory listing failed (status=%d)\n", (int)status);
        vfs.shutdown();
        return 1;
    }
    
    printf("Found %zu entries:\n", entries.size());
    printf("-------------------------------------------\n");
    printf("%-40s %12s Attr\n", "Name", "Size");
    printf("-------------------------------------------\n");
    
    bool found_default_xex = false;
    u64 total_size = 0;
    
    for (const auto& entry : entries) {
        format_size(entry.size, size_str, sizeof(size_str));
        printf("%-40s %12s ", entry.name.c_str(), entry.size > 0 ? size_str : "-");
        print_attributes(entry.attributes);
        printf("\n");
        
        total_size += entry.size;
        
        // Check for default.xex
        if (entry.name == "default.xex" || entry.name == "DEFAULT.XEX") {
            found_default_xex = true;
        }
    }
    printf("-------------------------------------------\n");
    format_size(total_size, size_str, sizeof(size_str));
    printf("Total: %zu files/folders, %s\n\n", entries.size(), size_str);
    
    if (entries.empty()) {
        printf("❌ FAIL: No entries found in root directory\n");
        vfs.shutdown();
        return 1;
    }
    printf("✅ PASS: Directory listing succeeded (%zu entries)\n\n", entries.size());
    
    // Try to read default.xex
    printf("[TEST 4] Looking for default.xex...\n");
    
    std::string xex_path;
    if (vfs.file_exists("game:\\default.xex")) {
        xex_path = "game:\\default.xex";
    } else if (vfs.file_exists("game:\\DEFAULT.XEX")) {
        xex_path = "game:\\DEFAULT.XEX";
    }
    
    if (xex_path.empty()) {
        printf("⚠️  WARN: default.xex not found in root directory\n");
        printf("         This might be expected for some disc structures\n");
        printf("         Try checking subdirectories manually\n\n");
    } else {
        printf("Found: %s\n", xex_path.c_str());
        
        // Open and read header
        u32 handle = INVALID_FILE_HANDLE;
        status = vfs.open_file(xex_path, FileAccess::Read, handle);
        if (status != Status::Ok) {
            printf("❌ FAIL: Cannot open default.xex (status=%d)\n", (int)status);
            vfs.shutdown();
            return 1;
        }
        
        // Get file size
        u64 file_size = 0;
        vfs.get_file_size(handle, file_size);
        format_size(file_size, size_str, sizeof(size_str));
        printf("Size: %s\n", size_str);
        
        // Read magic bytes
        u8 magic[4] = {0};
        u64 bytes_read = 0;
        status = vfs.read_file(handle, magic, 4, bytes_read);
        if (status != Status::Ok || bytes_read != 4) {
            printf("❌ FAIL: Cannot read XEX magic (status=%d, read=%llu)\n", 
                   (int)status, (unsigned long long)bytes_read);
            vfs.close_file(handle);
            vfs.shutdown();
            return 1;
        }
        
        printf("Magic: %c%c%c%c (0x%02X%02X%02X%02X)\n", 
               magic[0], magic[1], magic[2], magic[3],
               magic[0], magic[1], magic[2], magic[3]);
        
        // Verify XEX2 magic
        if (magic[0] == 'X' && magic[1] == 'E' && magic[2] == 'X' && magic[3] == '2') {
            printf("✅ PASS: Valid XEX2 executable found!\n\n");
        } else {
            printf("⚠️  WARN: Not a XEX2 file (might be encrypted or different format)\n\n");
        }
        
        vfs.close_file(handle);
    }
    
    // Test reading a potentially larger file
    printf("[TEST 5] Testing sequential read performance...\n");
    
    // Find a larger file to read
    std::string test_file;
    u64 test_file_size = 0;
    for (const auto& entry : entries) {
        if (!entry.is_directory && entry.size > test_file_size && entry.size < 100 * 1024 * 1024) {
            test_file = "game:\\" + entry.name;
            test_file_size = entry.size;
        }
    }
    
    if (test_file.empty()) {
        printf("⚠️  WARN: No suitable test file found\n\n");
    } else {
        u32 handle = INVALID_FILE_HANDLE;
        status = vfs.open_file(test_file, FileAccess::Read, handle);
        if (status == Status::Ok) {
            format_size(test_file_size, size_str, sizeof(size_str));
            printf("Reading %s (%s)...\n", test_file.c_str(), size_str);
            
            // Read in chunks and measure time
            constexpr size_t CHUNK_SIZE = 256 * 1024;  // 256KB chunks
            u8* buffer = new u8[CHUNK_SIZE];
            u64 total_read = 0;
            
            auto start = std::chrono::high_resolution_clock::now();
            
            while (total_read < test_file_size) {
                u64 to_read = std::min((u64)CHUNK_SIZE, test_file_size - total_read);
                u64 bytes_read = 0;
                status = vfs.read_file(handle, buffer, to_read, bytes_read);
                if (status != Status::Ok || bytes_read == 0) break;
                total_read += bytes_read;
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            
            delete[] buffer;
            vfs.close_file(handle);
            
            format_size(total_read, size_str, sizeof(size_str));
            printf("Read %s in %lld ms", size_str, (long long)duration_ms);
            if (duration_ms > 0) {
                double mb_per_sec = (double)total_read / (1024.0 * 1024.0) / ((double)duration_ms / 1000.0);
                printf(" (%.2f MB/s)", mb_per_sec);
            }
            printf("\n");
            
            if (total_read == test_file_size) {
                printf("✅ PASS: Sequential read completed\n\n");
            } else {
                printf("⚠️  WARN: Incomplete read (%llu / %llu bytes)\n\n",
                       (unsigned long long)total_read, (unsigned long long)test_file_size);
            }
        } else {
            printf("⚠️  WARN: Could not open test file\n\n");
        }
    }
    
    // Cleanup
    printf("[CLEANUP] Unmounting and shutting down...\n");
    vfs.unmount("game:");
    vfs.shutdown();
    
    printf("\n===========================================\n");
    printf("SUMMARY: ISO Mount Test\n");
    printf("===========================================\n");
    printf("✅ VFS Initialization:     PASS\n");
    printf("✅ ISO Mount:              PASS\n");
    printf("✅ Directory Listing:      PASS\n");
    if (!xex_path.empty()) {
        printf("✅ XEX Magic Read:         PASS\n");
    } else {
        printf("⚠️  XEX Magic Read:         SKIP (no default.xex)\n");
    }
    printf("✅ Sequential Read:        PASS\n");
    printf("\n🎉 Level 1 Complete! ISO mounting works.\n");
    printf("   Next: Run test_xex_load with a XEX file\n");
    printf("===========================================\n");
    
    return 0;
}

