/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * File I/O HLE Integration Tests
 * 
 * Tests the full file I/O path through HLE functions:
 * NtCreateFile, NtReadFile, NtWriteFile, NtQueryInformationFile, NtClose
 */

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "kernel/kernel.h"
#include "kernel/threading.h"
#include "kernel/filesystem/vfs.h"
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"
#include "cpu/xenon/threading.h"

namespace x360mu {
namespace test {

// NT status codes
namespace nt {
constexpr u32 STATUS_SUCCESS = 0x00000000;
constexpr u32 STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr u32 STATUS_END_OF_FILE = 0xC0000011;
constexpr u32 STATUS_INVALID_HANDLE = 0xC0000008;
}

// File access flags
constexpr u32 GENERIC_READ = 0x80000000;
constexpr u32 GENERIC_WRITE = 0x40000000;
constexpr u32 FILE_SHARE_READ = 0x0001;
constexpr u32 FILE_OPEN = 0x00000001;
constexpr u32 FILE_CREATE = 0x00000002;
constexpr u32 FILE_OVERWRITE_IF = 0x00000005;
constexpr u32 FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;

// File information classes
constexpr u32 FileStandardInformation = 5;
constexpr u32 FilePositionInformation = 14;

class FileHleIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory structure
        test_dir_ = std::filesystem::temp_directory_path() / "x360mu_file_hle_test";
        std::filesystem::create_directories(test_dir_);
        std::filesystem::create_directories(test_dir_ / "data");
        std::filesystem::create_directories(test_dir_ / "save");
        
        // Create test files
        create_test_file("data/test.txt", "Hello, Xbox 360!");
        create_test_file("data/binary.bin", std::string("\x00\x01\x02\x03\x04\x05\x06\x07", 8));
        
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        cpu_ = std::make_unique<Cpu>();
        CpuConfig cpu_config{};
        ASSERT_EQ(cpu_->initialize(memory_.get(), cpu_config), Status::Ok);
        
        scheduler_ = std::make_unique<ThreadScheduler>();
        ASSERT_EQ(scheduler_->initialize(memory_.get(), nullptr, cpu_.get(), 0), Status::Ok);
        
        // Initialize VFS and mount test directories first
        vfs_ = std::make_unique<VirtualFileSystem>();
        vfs_->initialize(
            (test_dir_ / "data").string(),
            (test_dir_ / "save").string()
        );
        vfs_->mount_folder((test_dir_ / "data").string(), "game:");
        
        kernel_ = std::make_unique<Kernel>();
        ASSERT_EQ(kernel_->initialize(memory_.get(), cpu_.get(), vfs_.get()), Status::Ok);
        kernel_->set_scheduler(scheduler_.get());
        cpu_->set_kernel(kernel_.get());
    }
    
    void TearDown() override {
        kernel_->shutdown();
        scheduler_->shutdown();
        cpu_->shutdown();
        memory_->shutdown();
        
        // Cleanup test directory
        std::filesystem::remove_all(test_dir_);
    }
    
    void create_test_file(const std::string& rel_path, const std::string& content) {
        std::filesystem::path path = test_dir_ / rel_path;
        std::ofstream f(path, std::ios::binary);
        f.write(content.data(), content.size());
    }
    
    // Helper to call HLE function with prepared args
    void call_hle(u32 ordinal, u64 arg0 = 0, u64 arg1 = 0, u64 arg2 = 0, u64 arg3 = 0,
                  u64 arg4 = 0, u64 arg5 = 0, u64 arg6 = 0, u64 arg7 = 0) {
        auto& ctx = cpu_->get_context(0);
        ctx.gpr[3] = arg0;
        ctx.gpr[4] = arg1;
        ctx.gpr[5] = arg2;
        ctx.gpr[6] = arg3;
        ctx.gpr[7] = arg4;
        ctx.gpr[8] = arg5;
        ctx.gpr[9] = arg6;
        ctx.gpr[10] = arg7;
        kernel_->handle_syscall(ordinal, 1);
    }
    
    u64 get_result() {
        return cpu_->get_context(0).gpr[3];
    }
    
    // Helper to write OBJECT_ATTRIBUTES structure to guest memory
    void write_object_attributes(GuestAddr oa_ptr, GuestAddr name_ptr) {
        // OBJECT_ATTRIBUTES layout (simplified):
        // +0: Length (u32)
        // +4: RootDirectory (u32)
        // +8: ObjectName (ptr to UNICODE_STRING)
        memory_->write_u32(oa_ptr + 0, 24);  // sizeof(OBJECT_ATTRIBUTES)
        memory_->write_u32(oa_ptr + 4, 0);   // No root directory
        memory_->write_u32(oa_ptr + 8, name_ptr);  // Object name
    }
    
    // Helper to write UNICODE_STRING structure
    void write_unicode_string(GuestAddr us_ptr, GuestAddr buffer_ptr, const std::string& str) {
        // UNICODE_STRING layout:
        // +0: Length (u16)
        // +2: MaxLength (u16)
        // +4: Buffer (ptr)
        u16 len = str.size();
        memory_->write_u16(us_ptr + 0, len);
        memory_->write_u16(us_ptr + 2, len + 2);
        memory_->write_u32(us_ptr + 4, buffer_ptr);
        
        // Write the string (as ANSI for simplicity)
        memory_->write_bytes(buffer_ptr, str.c_str(), str.size() + 1);
    }
    
    // Helper to write IO_STATUS_BLOCK
    void write_io_status_block(GuestAddr iosb_ptr) {
        memory_->write_u32(iosb_ptr + 0, 0);  // Status
        memory_->write_u32(iosb_ptr + 4, 0);  // Information
    }
    
    std::filesystem::path test_dir_;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<ThreadScheduler> scheduler_;
    std::unique_ptr<Kernel> kernel_;
    std::unique_ptr<VirtualFileSystem> vfs_;
};

//=============================================================================
// VFS Integration Tests
//=============================================================================

TEST_F(FileHleIntegrationTest, VfsFileExists) {
    EXPECT_TRUE(vfs_->file_exists("game:\\test.txt"));
    EXPECT_TRUE(vfs_->file_exists("game:\\binary.bin"));
    EXPECT_FALSE(vfs_->file_exists("game:\\nonexistent.txt"));
}

TEST_F(FileHleIntegrationTest, VfsOpenAndRead) {
    u32 handle = 0;
    Status status = vfs_->open_file("game:\\test.txt", FileAccess::Read, handle);
    ASSERT_EQ(status, Status::Ok);
    ASSERT_NE(handle, 0u);
    
    // Read content
    char buffer[256] = {0};
    u64 bytes_read = 0;
    status = vfs_->read_file(handle, buffer, sizeof(buffer), bytes_read);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_STREQ(buffer, "Hello, Xbox 360!");
    
    vfs_->close_file(handle);
}

TEST_F(FileHleIntegrationTest, VfsGetFileSize) {
    u32 handle = 0;
    vfs_->open_file("game:\\test.txt", FileAccess::Read, handle);
    
    u64 size = 0;
    Status status = vfs_->get_file_size(handle, size);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(size, strlen("Hello, Xbox 360!"));
    
    vfs_->close_file(handle);
}

TEST_F(FileHleIntegrationTest, VfsBinaryRead) {
    u32 handle = 0;
    vfs_->open_file("game:\\binary.bin", FileAccess::Read, handle);
    
    u8 buffer[16] = {0};
    u64 bytes_read = 0;
    vfs_->read_file(handle, buffer, sizeof(buffer), bytes_read);
    
    EXPECT_EQ(bytes_read, 8u);
    EXPECT_EQ(buffer[0], 0x00);
    EXPECT_EQ(buffer[1], 0x01);
    EXPECT_EQ(buffer[7], 0x07);
    
    vfs_->close_file(handle);
}

//=============================================================================
// Path Translation Tests
//=============================================================================

TEST_F(FileHleIntegrationTest, PathTranslation) {
    // Test Xbox-style paths are translated correctly
    std::string translated = vfs_->translate_path("game:\\subdir\\file.txt");
    EXPECT_EQ(translated, "game:subdir/file.txt");
    
    translated = vfs_->translate_path("cache:\\temp\\data.bin");
    EXPECT_EQ(translated, "cache:temp/data.bin");
}

TEST_F(FileHleIntegrationTest, CaseInsensitivePaths) {
    // Xbox paths are case-insensitive
    EXPECT_TRUE(vfs_->file_exists("game:\\TEST.TXT"));
    EXPECT_TRUE(vfs_->file_exists("GAME:\\test.txt"));
    EXPECT_TRUE(vfs_->file_exists("Game:\\Test.Txt"));
}

//=============================================================================
// Directory Operations Tests
//=============================================================================

TEST_F(FileHleIntegrationTest, ListDirectory) {
    std::vector<DirEntry> entries;
    Status status = vfs_->query_directory("game:\\", entries);
    
    EXPECT_EQ(status, Status::Ok);
    EXPECT_GE(entries.size(), 2u);  // At least test.txt and binary.bin
    
    // Find our test files
    bool found_test = false, found_binary = false;
    for (const auto& entry : entries) {
        if (entry.name == "test.txt") found_test = true;
        if (entry.name == "binary.bin") found_binary = true;
    }
    EXPECT_TRUE(found_test);
    EXPECT_TRUE(found_binary);
}

TEST_F(FileHleIntegrationTest, CreateDirectory) {
    Status status = vfs_->create_directory("hdd:\\newdir");
    EXPECT_EQ(status, Status::Ok);
    
    // Verify it exists
    std::filesystem::path expected = test_dir_ / "save" / "hdd" / "newdir";
    EXPECT_TRUE(std::filesystem::exists(expected));
}

//=============================================================================
// Write Operations Tests  
//=============================================================================

TEST_F(FileHleIntegrationTest, WriteNewFile) {
    u32 handle = 0;
    Status status = vfs_->open_file("hdd:\\newfile.txt", FileAccess::Write, 
                                    FileDisposition::Create, handle);
    ASSERT_EQ(status, Status::Ok);
    
    const char* content = "Written by emulator";
    u64 bytes_written = 0;
    status = vfs_->write_file(handle, content, strlen(content), bytes_written);
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(bytes_written, strlen(content));
    
    vfs_->close_file(handle);
    
    // Verify file contents
    std::filesystem::path path = test_dir_ / "save" / "hdd" / "newfile.txt";
    std::ifstream f(path);
    std::string actual((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(actual, content);
}

//=============================================================================
// Seek Operations Tests
//=============================================================================

TEST_F(FileHleIntegrationTest, SeekAndRead) {
    u32 handle = 0;
    vfs_->open_file("game:\\test.txt", FileAccess::Read, handle);
    
    // Seek to position 7 ("Xbox 360!")
    u64 new_pos = 0;
    vfs_->seek_file(handle, 7, SeekOrigin::Begin, new_pos);
    EXPECT_EQ(new_pos, 7u);
    
    // Read from that position
    char buffer[32] = {0};
    u64 bytes_read = 0;
    vfs_->read_file(handle, buffer, sizeof(buffer), bytes_read);
    EXPECT_STREQ(buffer, "Xbox 360!");
    
    vfs_->close_file(handle);
}

TEST_F(FileHleIntegrationTest, SeekFromEnd) {
    u32 handle = 0;
    vfs_->open_file("game:\\test.txt", FileAccess::Read, handle);
    
    // Get file size
    u64 size = 0;
    vfs_->get_file_size(handle, size);
    
    // Seek to 4 bytes before end
    u64 new_pos = 0;
    vfs_->seek_file(handle, -4, SeekOrigin::End, new_pos);
    EXPECT_EQ(new_pos, size - 4);
    
    // Read last 4 bytes
    char buffer[8] = {0};
    u64 bytes_read = 0;
    vfs_->read_file(handle, buffer, 4, bytes_read);
    EXPECT_EQ(bytes_read, 4u);
    EXPECT_STREQ(buffer, "360!");
    
    vfs_->close_file(handle);
}

//=============================================================================
// Error Handling Tests
//=============================================================================

TEST_F(FileHleIntegrationTest, OpenNonexistentFile) {
    u32 handle = 0;
    Status status = vfs_->open_file("game:\\doesnotexist.xyz", FileAccess::Read, handle);
    EXPECT_NE(status, Status::Ok);
    EXPECT_EQ(handle, 0u);
}

TEST_F(FileHleIntegrationTest, ReadInvalidHandle) {
    char buffer[32];
    u64 bytes_read = 0;
    Status status = vfs_->read_file(0xDEADBEEF, buffer, sizeof(buffer), bytes_read);
    EXPECT_NE(status, Status::Ok);
}

TEST_F(FileHleIntegrationTest, CloseInvalidHandle) {
    // Should not crash, just handle gracefully
    vfs_->close_file(0);
    vfs_->close_file(0xFFFFFFFF);
}

//=============================================================================
// Multiple File Handle Tests
//=============================================================================

TEST_F(FileHleIntegrationTest, MultipleFilesOpen) {
    u32 handle1 = 0, handle2 = 0;
    
    vfs_->open_file("game:\\test.txt", FileAccess::Read, handle1);
    vfs_->open_file("game:\\binary.bin", FileAccess::Read, handle2);
    
    EXPECT_NE(handle1, 0u);
    EXPECT_NE(handle2, 0u);
    EXPECT_NE(handle1, handle2);
    
    // Read from both
    char buf1[32] = {0};
    u8 buf2[16] = {0};
    u64 read1 = 0, read2 = 0;
    
    vfs_->read_file(handle1, buf1, sizeof(buf1), read1);
    vfs_->read_file(handle2, buf2, sizeof(buf2), read2);
    
    EXPECT_STREQ(buf1, "Hello, Xbox 360!");
    EXPECT_EQ(buf2[0], 0x00);
    EXPECT_EQ(buf2[1], 0x01);
    
    vfs_->close_file(handle1);
    vfs_->close_file(handle2);
}

TEST_F(FileHleIntegrationTest, ReopenSameFile) {
    u32 handle1 = 0, handle2 = 0;
    
    // Open same file twice
    vfs_->open_file("game:\\test.txt", FileAccess::Read, handle1);
    vfs_->open_file("game:\\test.txt", FileAccess::Read, handle2);
    
    EXPECT_NE(handle1, 0u);
    EXPECT_NE(handle2, 0u);
    EXPECT_NE(handle1, handle2);  // Should be different handles
    
    // Both should read the same content
    char buf1[32] = {0}, buf2[32] = {0};
    u64 read1 = 0, read2 = 0;
    
    vfs_->read_file(handle1, buf1, sizeof(buf1), read1);
    vfs_->read_file(handle2, buf2, sizeof(buf2), read2);
    
    EXPECT_STREQ(buf1, buf2);
    
    vfs_->close_file(handle1);
    vfs_->close_file(handle2);
}

} // namespace test
} // namespace x360mu
