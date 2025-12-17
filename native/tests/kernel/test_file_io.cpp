/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Unit tests for Kernel File I/O HLE
 */

#include <gtest/gtest.h>
#include "kernel/kernel.h"
#include "kernel/filesystem/vfs.h"
#include "memory/memory.h"
#include <fstream>
#include <filesystem>

namespace x360mu {
namespace test {

class FileIOTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory structure
        test_dir_ = std::filesystem::temp_directory_path() / "x360mu_test_io";
        std::filesystem::create_directories(test_dir_);
        
        // Create test files
        std::ofstream(test_dir_ / "test.txt") << "Hello Xbox 360!";
        std::ofstream(test_dir_ / "data.bin").write("\x00\x01\x02\x03\x04\x05\x06\x07", 8);
        std::filesystem::create_directories(test_dir_ / "subdir");
        std::ofstream(test_dir_ / "subdir" / "nested.txt") << "Nested file content";
        
        // Initialize components
        memory_ = std::make_unique<Memory>();
        memory_->initialize();
        
        vfs_ = std::make_unique<VirtualFileSystem>();
        vfs_->initialize(test_dir_.string(), test_dir_.string());
        vfs_->mount_folder("game:", test_dir_.string());
    }
    
    void TearDown() override {
        vfs_->shutdown();
        memory_->shutdown();
        
        // Clean up test directory
        std::filesystem::remove_all(test_dir_);
    }
    
    std::filesystem::path test_dir_;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<VirtualFileSystem> vfs_;
};

// Test VFS path translation
TEST_F(FileIOTest, PathTranslation) {
    // Test basic path translation
    std::string result = vfs_->translate_path("game:test.txt");
    EXPECT_FALSE(result.empty());
    EXPECT_NE(result.find("test.txt"), std::string::npos);
}

// Test file existence check
TEST_F(FileIOTest, FileExists) {
    EXPECT_TRUE(vfs_->file_exists("game:test.txt"));
    EXPECT_TRUE(vfs_->file_exists("game:data.bin"));
    EXPECT_TRUE(vfs_->file_exists("game:subdir/nested.txt"));
    EXPECT_FALSE(vfs_->file_exists("game:nonexistent.txt"));
}

// Test opening and closing files
TEST_F(FileIOTest, OpenCloseFile) {
    u32 handle;
    
    // Open existing file
    Status status = vfs_->open_file("game:test.txt", FileAccess::Read, handle);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_NE(handle, INVALID_FILE_HANDLE);
    
    // Close file
    status = vfs_->close_file(handle);
    EXPECT_EQ(status, Status::Ok);
    
    // Try to open non-existent file
    status = vfs_->open_file("game:nonexistent.txt", FileAccess::Read, handle);
    EXPECT_NE(status, Status::Ok);
}

// Test reading files
TEST_F(FileIOTest, ReadFile) {
    u32 handle;
    Status status = vfs_->open_file("game:test.txt", FileAccess::Read, handle);
    ASSERT_EQ(status, Status::Ok);
    
    char buffer[256] = {0};
    u64 bytes_read = 0;
    
    status = vfs_->read_file(handle, buffer, sizeof(buffer), bytes_read);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_GT(bytes_read, 0u);
    EXPECT_STREQ(buffer, "Hello Xbox 360!");
    
    vfs_->close_file(handle);
}

// Test reading binary files
TEST_F(FileIOTest, ReadBinaryFile) {
    u32 handle;
    Status status = vfs_->open_file("game:data.bin", FileAccess::Read, handle);
    ASSERT_EQ(status, Status::Ok);
    
    u8 buffer[8] = {0};
    u64 bytes_read = 0;
    
    status = vfs_->read_file(handle, buffer, sizeof(buffer), bytes_read);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(bytes_read, 8u);
    
    for (int i = 0; i < 8; i++) {
        EXPECT_EQ(buffer[i], static_cast<u8>(i));
    }
    
    vfs_->close_file(handle);
}

// Test file seeking
TEST_F(FileIOTest, SeekFile) {
    u32 handle;
    Status status = vfs_->open_file("game:test.txt", FileAccess::Read, handle);
    ASSERT_EQ(status, Status::Ok);
    
    // Seek to middle
    u64 new_pos = 0;
    status = vfs_->seek_file(handle, 6, SeekOrigin::Begin, new_pos);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(new_pos, 6u);
    
    char buffer[256] = {0};
    u64 bytes_read = 0;
    status = vfs_->read_file(handle, buffer, sizeof(buffer), bytes_read);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_STREQ(buffer, "Xbox 360!");
    
    vfs_->close_file(handle);
}

// Test file size query
TEST_F(FileIOTest, GetFileSize) {
    u32 handle;
    Status status = vfs_->open_file("game:test.txt", FileAccess::Read, handle);
    ASSERT_EQ(status, Status::Ok);
    
    u64 size = 0;
    status = vfs_->get_file_size(handle, size);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(size, 15u);  // "Hello Xbox 360!" is 15 chars
    
    vfs_->close_file(handle);
}

// Test file info query
TEST_F(FileIOTest, GetFileInfo) {
    FileInfo info;
    Status status = vfs_->get_file_info("game:test.txt", info);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(info.size, 15u);
    EXPECT_EQ(static_cast<u32>(info.attributes) & static_cast<u32>(FileAttributes::Directory), 0u);
    
    // Test directory info
    status = vfs_->get_file_info("game:subdir", info);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_NE(static_cast<u32>(info.attributes) & static_cast<u32>(FileAttributes::Directory), 0u);
}

// Test directory enumeration
TEST_F(FileIOTest, QueryDirectory) {
    std::vector<DirEntry> entries;
    Status status = vfs_->query_directory("game:", entries);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_GE(entries.size(), 3u);  // test.txt, data.bin, subdir
    
    // Verify expected entries exist
    bool found_test = false;
    bool found_data = false;
    bool found_subdir = false;
    
    for (const auto& entry : entries) {
        if (entry.name == "test.txt") found_test = true;
        if (entry.name == "data.bin") found_data = true;
        if (entry.name == "subdir") {
            found_subdir = true;
            EXPECT_TRUE(entry.is_directory);
        }
    }
    
    EXPECT_TRUE(found_test);
    EXPECT_TRUE(found_data);
    EXPECT_TRUE(found_subdir);
}

// Test writing files
TEST_F(FileIOTest, WriteFile) {
    // Create new file
    u32 handle;
    Status status = vfs_->open_file("game:newfile.txt", FileAccess::ReadWrite, 
                                    FileDisposition::Create, handle);
    ASSERT_EQ(status, Status::Ok);
    
    const char* data = "Written from test!";
    u64 bytes_written = 0;
    status = vfs_->write_file(handle, data, strlen(data), bytes_written);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(bytes_written, strlen(data));
    
    vfs_->close_file(handle);
    
    // Verify file was written
    std::ifstream verify(test_dir_ / "newfile.txt");
    std::string content;
    std::getline(verify, content);
    EXPECT_EQ(content, "Written from test!");
}

// Test file position tracking
TEST_F(FileIOTest, FilePosition) {
    u32 handle;
    Status status = vfs_->open_file("game:test.txt", FileAccess::Read, handle);
    ASSERT_EQ(status, Status::Ok);
    
    // Initial position should be 0
    u64 pos = 0;
    status = vfs_->get_file_position(handle, pos);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(pos, 0u);
    
    // Read some bytes
    char buffer[5];
    u64 bytes_read = 0;
    vfs_->read_file(handle, buffer, 5, bytes_read);
    
    // Position should advance
    status = vfs_->get_file_position(handle, pos);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(pos, 5u);
    
    vfs_->close_file(handle);
}

// Test Xbox path formats
TEST_F(FileIOTest, XboxPathFormats) {
    // Various Xbox 360 path formats that should work
    EXPECT_TRUE(vfs_->file_exists("game:test.txt"));
    EXPECT_TRUE(vfs_->file_exists("game:/test.txt"));
    EXPECT_TRUE(vfs_->file_exists("game:\\test.txt"));
    EXPECT_TRUE(vfs_->file_exists("game://test.txt"));
}

} // namespace test
} // namespace x360mu
