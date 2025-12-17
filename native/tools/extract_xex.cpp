/**
 * Quick XEX extractor from ISO
 */

#include "kernel/filesystem/vfs.h"
#include <cstdio>

using namespace x360mu;

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <iso_path> <output_path>\n", argv[0]);
        return 1;
    }
    
    const char* iso_path = argv[1];
    const char* output_path = argv[2];
    
    VirtualFileSystem vfs;
    if (vfs.initialize("/tmp", "/tmp") != Status::Ok) {
        printf("Failed to init VFS\n");
        return 1;
    }
    
    if (vfs.mount_iso("game:", iso_path) != Status::Ok) {
        printf("Failed to mount ISO\n");
        return 1;
    }
    
    // Find default.xex
    std::string xex_path;
    if (vfs.file_exists("game:\\default.xex")) {
        xex_path = "game:\\default.xex";
    } else if (vfs.file_exists("game:\\DEFAULT.XEX")) {
        xex_path = "game:\\DEFAULT.XEX";
    } else {
        printf("default.xex not found\n");
        return 1;
    }
    
    u32 handle = INVALID_FILE_HANDLE;
    if (vfs.open_file(xex_path, FileAccess::Read, handle) != Status::Ok) {
        printf("Failed to open XEX\n");
        return 1;
    }
    
    u64 size = 0;
    vfs.get_file_size(handle, size);
    printf("Extracting %s (%.2f MB)...\n", xex_path.c_str(), size / 1024.0 / 1024.0);
    
    // Read entire file
    std::vector<u8> data(size);
    u64 bytes_read = 0;
    if (vfs.read_file(handle, data.data(), size, bytes_read) != Status::Ok) {
        printf("Failed to read XEX\n");
        return 1;
    }
    
    vfs.close_file(handle);
    vfs.shutdown();
    
    // Write to output
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        printf("Failed to create output file\n");
        return 1;
    }
    
    fwrite(data.data(), 1, bytes_read, out);
    fclose(out);
    
    printf("Extracted %llu bytes to %s\n", (unsigned long long)bytes_read, output_path);
    return 0;
}
