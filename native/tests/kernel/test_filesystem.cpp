/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * File System Unit Tests
 */

#include <gtest/gtest.h>
#include "kernel/filesystem/vfs.h"
#include "kernel/filesystem/iso_device.h"
#include "kernel/filesystem/stfs_device.h"

#include <cstring>
#include <fstream>
#include <filesystem>

namespace x360mu {
namespace test {

// Test fixtures
class VfsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directories
        test_dir_ = std::filesystem::temp_directory_path() / "x360mu_test";
        std::filesystem::create_directories(test_dir_);
        std::filesystem::create_directories(test_dir_ / "data");
        std::filesystem::create_directories(test_dir_ / "save");
        
        // Create some test files
        create_test_file("data/test.txt", "Hello, World!");
        create_test_file("data/binary.bin", std::string(256, 'X'));
        std::filesystem::create_directories(test_dir_ / "data" / "subdir");
        create_test_file("data/subdir/nested.txt", "Nested file content");
    }
    
    void TearDown() override {
        // Clean up test files
        std::filesystem::remove_all(test_dir_);
    }
    
    void create_test_file(const std::string& rel_path, const std::string& content) {
        std::filesystem::path path = test_dir_ / rel_path;
        std::ofstream file(path, std::ios::binary);
        file.write(content.data(), content.size());
    }
    
    std::filesystem::path test_dir_;
};

// ============================================================================
// VirtualFileSystem Tests
// ============================================================================

TEST_F(VfsTest, Initialize) {
    VirtualFileSystem vfs;
    Status status = vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    
    EXPECT_EQ(status, Status::Ok);
    
    // Verify default mounts were created
    EXPECT_TRUE(vfs.file_exists("cache:\\"));
    EXPECT_TRUE(vfs.file_exists("hdd:\\"));
    EXPECT_TRUE(vfs.file_exists("title:\\"));
}

TEST_F(VfsTest, MountFolder) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    
    // Mount test data folder as game:
    Status status = vfs.mount_folder("game:", (test_dir_ / "data").string());
    EXPECT_EQ(status, Status::Ok);
    
    // Check file exists
    EXPECT_TRUE(vfs.file_exists("game:\\test.txt"));
    EXPECT_TRUE(vfs.file_exists("game:\\subdir\\nested.txt"));
    EXPECT_FALSE(vfs.file_exists("game:\\nonexistent.txt"));
}

TEST_F(VfsTest, OpenAndReadFile) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    vfs.mount_folder("game:", (test_dir_ / "data").string());
    
    // Open file
    u32 handle;
    Status status = vfs.open_file("game:\\test.txt", FileAccess::Read, handle);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_NE(handle, INVALID_FILE_HANDLE);
    
    // Read content
    char buffer[256] = {};
    u64 bytes_read;
    status = vfs.read_file(handle, buffer, sizeof(buffer), bytes_read);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(bytes_read, 13);
    EXPECT_STREQ(buffer, "Hello, World!");
    
    // Close file
    status = vfs.close_file(handle);
    EXPECT_EQ(status, Status::Ok);
}

TEST_F(VfsTest, GetFileSize) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    vfs.mount_folder("game:", (test_dir_ / "data").string());
    
    u32 handle;
    vfs.open_file("game:\\test.txt", FileAccess::Read, handle);
    
    u64 size;
    Status status = vfs.get_file_size(handle, size);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(size, 13);  // "Hello, World!" = 13 bytes
    
    vfs.close_file(handle);
}

TEST_F(VfsTest, SeekAndRead) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    vfs.mount_folder("game:", (test_dir_ / "data").string());
    
    u32 handle;
    vfs.open_file("game:\\test.txt", FileAccess::Read, handle);
    
    // Seek to position 7
    u64 new_pos;
    Status status = vfs.seek_file(handle, 7, SeekOrigin::Begin, new_pos);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(new_pos, 7);
    
    // Read from position 7
    char buffer[32] = {};
    u64 bytes_read;
    vfs.read_file(handle, buffer, sizeof(buffer), bytes_read);
    EXPECT_EQ(bytes_read, 6);  // "World!"
    EXPECT_STREQ(buffer, "World!");
    
    vfs.close_file(handle);
}

TEST_F(VfsTest, ListDirectory) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    vfs.mount_folder("game:", (test_dir_ / "data").string());
    
    std::vector<DirEntry> entries;
    Status status = vfs.query_directory("game:\\*", entries);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_GE(entries.size(), 3);  // test.txt, binary.bin, subdir
    
    // Check for expected entries
    bool found_test_txt = false;
    bool found_subdir = false;
    for (const auto& entry : entries) {
        if (entry.name == "test.txt") {
            found_test_txt = true;
            EXPECT_FALSE(entry.is_directory);
        }
        if (entry.name == "subdir") {
            found_subdir = true;
            EXPECT_TRUE(entry.is_directory);
        }
    }
    EXPECT_TRUE(found_test_txt);
    EXPECT_TRUE(found_subdir);
}

TEST_F(VfsTest, PathTranslation) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    
    // Test path translation
    std::string translated = vfs.translate_path("game:\\maps\\test.ff");
    EXPECT_EQ(translated, "game:maps/test.ff");
    
    translated = vfs.translate_path("cache:\\temp\\file.dat");
    EXPECT_EQ(translated, "cache:temp/file.dat");
}

TEST_F(VfsTest, WriteFile) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    
    // Create a new file in save storage
    u32 handle;
    Status status = vfs.open_file("hdd:\\newfile.txt", FileAccess::Write, 
                                  FileDisposition::Create, handle);
    EXPECT_EQ(status, Status::Ok);
    
    // Write content
    const char* content = "New file content";
    u64 bytes_written;
    status = vfs.write_file(handle, content, strlen(content), bytes_written);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(bytes_written, strlen(content));
    
    vfs.close_file(handle);
    
    // Read it back
    status = vfs.open_file("hdd:\\newfile.txt", FileAccess::Read, handle);
    EXPECT_EQ(status, Status::Ok);
    
    char buffer[64] = {};
    u64 bytes_read;
    vfs.read_file(handle, buffer, sizeof(buffer), bytes_read);
    EXPECT_STREQ(buffer, content);
    
    vfs.close_file(handle);
}

TEST_F(VfsTest, CreateDirectory) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    
    // Create a new directory
    Status status = vfs.create_directory("hdd:\\newdir");
    EXPECT_EQ(status, Status::Ok);
    
    // Verify it exists
    FileInfo info;
    status = vfs.get_file_info("hdd:\\newdir", info);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(info.attributes, FileAttributes::Directory);
}

TEST_F(VfsTest, CaseInsensitivePath) {
    VirtualFileSystem vfs;
    vfs.initialize(
        (test_dir_ / "data").string(),
        (test_dir_ / "save").string()
    );
    vfs.mount_folder("game:", (test_dir_ / "data").string());
    
    // Open with different cases - should all work
    u32 handle1, handle2, handle3;
    
    Status s1 = vfs.open_file("game:\\TEST.TXT", FileAccess::Read, handle1);
    Status s2 = vfs.open_file("GAME:\\test.txt", FileAccess::Read, handle2);
    Status s3 = vfs.open_file("Game:\\Test.Txt", FileAccess::Read, handle3);
    
    // At least the case-insensitive device lookup should work
    // File case sensitivity depends on host filesystem
    EXPECT_EQ(s1, s2);  // Should be consistent
    
    if (s1 == Status::Ok) vfs.close_file(handle1);
    if (s2 == Status::Ok) vfs.close_file(handle2);
    if (s3 == Status::Ok) vfs.close_file(handle3);
}

// ============================================================================
// HostDevice Tests
// ============================================================================

TEST_F(VfsTest, HostDevice_BasicOperations) {
    HostDevice device;
    
    Status status = device.mount((test_dir_ / "data").string());
    EXPECT_EQ(status, Status::Ok);
    
    EXPECT_TRUE(device.exists("test.txt"));
    EXPECT_TRUE(device.exists("subdir"));
    EXPECT_FALSE(device.exists("nonexistent.txt"));
    
    EXPECT_TRUE(device.is_directory("subdir"));
    EXPECT_FALSE(device.is_directory("test.txt"));
    
    device.unmount();
}

// ============================================================================
// ISO Device Tests (with mock/synthetic ISO)
// ============================================================================

class IsoTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "x360mu_iso_test";
        std::filesystem::create_directories(test_dir_);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }
    
    // Create a minimal valid ISO image for testing
    void create_minimal_iso(const std::string& filename) {
        std::filesystem::path path = test_dir_ / filename;
        std::ofstream file(path, std::ios::binary);
        
        // ISO consists of:
        // - 16 system sectors (32KB)
        // - Primary Volume Descriptor at sector 16
        // - Volume Descriptor Set Terminator at sector 17
        // - Root directory at sector 18
        
        constexpr size_t SECTOR_SIZE = 2048;
        std::vector<u8> sector(SECTOR_SIZE, 0);
        
        // Write 16 empty system sectors
        for (int i = 0; i < 16; i++) {
            file.write(reinterpret_cast<char*>(sector.data()), SECTOR_SIZE);
        }
        
        // Primary Volume Descriptor (sector 16)
        std::fill(sector.begin(), sector.end(), 0);
        sector[0] = 1;  // Type: Primary Volume Descriptor
        memcpy(&sector[1], "CD001", 5);  // Identifier
        sector[6] = 1;  // Version
        
        // System ID at offset 8 (32 bytes)
        memcpy(&sector[8], "TEST_SYSTEM", 11);
        
        // Volume ID at offset 40 (32 bytes)
        memcpy(&sector[40], "TEST_VOLUME", 11);
        
        // Volume space size (little-endian) at offset 80
        u32 volume_size = 20;  // 20 sectors
        memcpy(&sector[80], &volume_size, 4);
        
        // Logical block size (little-endian) at offset 128
        u16 block_size = SECTOR_SIZE;
        memcpy(&sector[128], &block_size, 2);
        
        // Root directory record at offset 156
        u8* root_rec = &sector[156];
        root_rec[0] = 34;  // Record length
        root_rec[1] = 0;   // Extended attribute length
        u32 root_lba = 18;
        memcpy(&root_rec[2], &root_lba, 4);  // Extent LBA (LE)
        u32 root_size = 2048;  // One sector
        memcpy(&root_rec[10], &root_size, 4);  // Data length (LE)
        root_rec[25] = 2;  // Flags: directory
        root_rec[32] = 1;  // Name length
        root_rec[33] = 0;  // Name: root (0x00)
        
        file.write(reinterpret_cast<char*>(sector.data()), SECTOR_SIZE);
        
        // Volume Descriptor Set Terminator (sector 17)
        std::fill(sector.begin(), sector.end(), 0);
        sector[0] = 255;  // Type: Terminator
        memcpy(&sector[1], "CD001", 5);
        sector[6] = 1;
        file.write(reinterpret_cast<char*>(sector.data()), SECTOR_SIZE);
        
        // Root directory (sector 18)
        std::fill(sector.begin(), sector.end(), 0);
        
        // "." entry
        u8* dot = &sector[0];
        dot[0] = 34;  // Length
        dot[2] = 18; dot[3] = 0; dot[4] = 0; dot[5] = 0;  // LBA
        dot[10] = 0; dot[11] = 8; dot[12] = 0; dot[13] = 0;  // Size (2048)
        dot[25] = 2;  // Directory
        dot[32] = 1;  // Name length
        dot[33] = 0;  // Name: "."
        
        // ".." entry  
        u8* dotdot = &sector[34];
        dotdot[0] = 34;
        dotdot[2] = 18; dotdot[3] = 0; dotdot[4] = 0; dotdot[5] = 0;
        dotdot[10] = 0; dotdot[11] = 8; dotdot[12] = 0; dotdot[13] = 0;
        dotdot[25] = 2;
        dotdot[32] = 1;
        dotdot[33] = 1;  // Name: ".."
        
        // "DEFAULT.XEX" file entry
        u8* xex = &sector[68];
        xex[0] = 46;  // Length (header + name)
        xex[2] = 19; xex[3] = 0; xex[4] = 0; xex[5] = 0;  // LBA = 19
        u32 xex_size = 4;  // 4 bytes
        memcpy(&xex[10], &xex_size, 4);  // Size
        xex[25] = 0;  // File, not directory
        xex[32] = 11;  // Name length
        memcpy(&xex[33], "DEFAULT.XEX", 11);
        
        file.write(reinterpret_cast<char*>(sector.data()), SECTOR_SIZE);
        
        // XEX data (sector 19) - just XEX2 magic
        std::fill(sector.begin(), sector.end(), 0);
        memcpy(&sector[0], "XEX2", 4);
        file.write(reinterpret_cast<char*>(sector.data()), SECTOR_SIZE);
        
        // Pad to 20 sectors
        std::fill(sector.begin(), sector.end(), 0);
        file.write(reinterpret_cast<char*>(sector.data()), SECTOR_SIZE);
    }
    
    std::filesystem::path test_dir_;
};

TEST_F(IsoTest, MountIso) {
    create_minimal_iso("test.iso");
    
    IsoDevice device;
    Status status = device.mount((test_dir_ / "test.iso").string());
    EXPECT_EQ(status, Status::Ok);
    
    EXPECT_EQ(device.get_volume_id(), "TEST_VOLUME");
    
    device.unmount();
}

TEST_F(IsoTest, ListRootDirectory) {
    create_minimal_iso("test.iso");
    
    IsoDevice device;
    device.mount((test_dir_ / "test.iso").string());
    
    std::vector<DirEntry> entries;
    Status status = device.list_directory("", entries);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(entries.size(), 1);  // DEFAULT.XEX
    
    if (!entries.empty()) {
        EXPECT_EQ(entries[0].name, "DEFAULT.XEX");
        EXPECT_FALSE(entries[0].is_directory);
    }
    
    device.unmount();
}

TEST_F(IsoTest, ReadXexMagic) {
    create_minimal_iso("test.iso");
    
    IsoDevice device;
    device.mount((test_dir_ / "test.iso").string());
    
    // Open default.xex
    u32 handle = device.open("DEFAULT.XEX", FileAccess::Read, FileDisposition::Open);
    EXPECT_NE(handle, INVALID_FILE_HANDLE);
    
    // Read magic
    u8 buffer[4] = {};
    s64 bytes_read = device.read(handle, buffer, 4);
    EXPECT_EQ(bytes_read, 4);
    EXPECT_EQ(buffer[0], 'X');
    EXPECT_EQ(buffer[1], 'E');
    EXPECT_EQ(buffer[2], 'X');
    EXPECT_EQ(buffer[3], '2');
    
    device.close(handle);
    device.unmount();
}

TEST_F(IsoTest, FileExists) {
    create_minimal_iso("test.iso");
    
    IsoDevice device;
    device.mount((test_dir_ / "test.iso").string());
    
    EXPECT_TRUE(device.exists("DEFAULT.XEX"));
    EXPECT_TRUE(device.exists("default.xex"));  // Case insensitive
    EXPECT_FALSE(device.exists("NONEXISTENT.TXT"));
    
    device.unmount();
}

TEST_F(IsoTest, VfsMountIso) {
    create_minimal_iso("test.iso");
    
    VirtualFileSystem vfs;
    vfs.initialize(test_dir_.string(), test_dir_.string());
    
    Status status = vfs.mount_iso("game:", (test_dir_ / "test.iso").string());
    EXPECT_EQ(status, Status::Ok);
    
    // Query directory through VFS
    std::vector<DirEntry> entries;
    status = vfs.query_directory("game:\\*", entries);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_GT(entries.size(), 0);
    
    // Open file through VFS
    u32 handle;
    status = vfs.open_file("game:\\default.xex", FileAccess::Read, handle);
    EXPECT_EQ(status, Status::Ok);
    
    u8 buffer[4];
    u64 bytes_read;
    vfs.read_file(handle, buffer, 4, bytes_read);
    EXPECT_EQ(buffer[0], 'X');
    
    vfs.close_file(handle);
}

// ============================================================================
// STFS Device Tests
// ============================================================================

class StfsTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "x360mu_stfs_test";
        std::filesystem::create_directories(test_dir_);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }
    
    std::filesystem::path test_dir_;
};

TEST(StfsDeviceTest, InvalidMagic) {
    // Create temp file with invalid magic
    auto temp_path = std::filesystem::temp_directory_path() / "invalid.stfs";
    {
        std::ofstream file(temp_path, std::ios::binary);
        const char data[1024] = "INVALID DATA";
        file.write(data, sizeof(data));
    }
    
    StfsDevice device;
    Status status = device.mount(temp_path.string());
    EXPECT_NE(status, Status::Ok);  // Should fail
    
    std::filesystem::remove(temp_path);
}

TEST(StfsDeviceTest, NonexistentFile) {
    StfsDevice device;
    Status status = device.mount("/nonexistent/path/file.stfs");
    EXPECT_EQ(status, Status::NotFound);
}

// Integration test - mount test ISO and verify file operations work
TEST_F(IsoTest, FullIntegration) {
    create_minimal_iso("game.iso");
    
    VirtualFileSystem vfs;
    vfs.initialize(test_dir_.string(), test_dir_.string());
    
    // Mount ISO as game:
    Status status = vfs.mount_iso("game:", (test_dir_ / "game.iso").string());
    ASSERT_EQ(status, Status::Ok);
    
    // Mount also as dvd: for testing
    status = vfs.mount_iso("dvd:", (test_dir_ / "game.iso").string());
    ASSERT_EQ(status, Status::Ok);
    
    // Verify both mounts work
    EXPECT_TRUE(vfs.file_exists("game:\\default.xex"));
    EXPECT_TRUE(vfs.file_exists("dvd:\\default.xex"));
    
    // Open from game: mount
    u32 handle;
    status = vfs.open_file("game:\\default.xex", FileAccess::Read, handle);
    EXPECT_EQ(status, Status::Ok);
    
    // Get file size
    u64 size;
    vfs.get_file_size(handle, size);
    EXPECT_EQ(size, 4);  // XEX2 magic only
    
    // Read full file
    std::vector<u8> buffer(size);
    u64 bytes_read;
    vfs.read_file(handle, buffer.data(), size, bytes_read);
    EXPECT_EQ(bytes_read, size);
    
    vfs.close_file(handle);
}

} // namespace test
} // namespace x360mu
