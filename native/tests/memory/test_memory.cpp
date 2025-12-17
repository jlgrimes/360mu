/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Memory System Tests
 */

#include <gtest/gtest.h>
#include "memory/memory.h"

namespace x360mu {
namespace test {

class MemoryTest : public ::testing::Test {
protected:
    std::unique_ptr<Memory> memory;
    
    void SetUp() override {
        memory = std::make_unique<Memory>();
        ASSERT_EQ(memory->initialize(), Status::Ok);
    }
    
    void TearDown() override {
        memory->shutdown();
    }
};

TEST_F(MemoryTest, BasicReadWrite) {
    // Use address within 512MB range
    GuestAddr addr = 0x00100000;  // 1MB offset
    
    memory->write_u32(addr, 0xDEADBEEF);
    EXPECT_EQ(memory->read_u32(addr), 0xDEADBEEF);
}

TEST_F(MemoryTest, ByteReadWrite) {
    GuestAddr addr = 0x00100100;
    
    memory->write_u8(addr, 0xAB);
    memory->write_u8(addr + 1, 0xCD);
    memory->write_u8(addr + 2, 0xEF);
    memory->write_u8(addr + 3, 0x12);
    
    EXPECT_EQ(memory->read_u8(addr), 0xAB);
    EXPECT_EQ(memory->read_u8(addr + 1), 0xCD);
    EXPECT_EQ(memory->read_u8(addr + 2), 0xEF);
    EXPECT_EQ(memory->read_u8(addr + 3), 0x12);
}

TEST_F(MemoryTest, HalfWordReadWrite) {
    GuestAddr addr = 0x00100200;
    
    memory->write_u16(addr, 0x1234);
    memory->write_u16(addr + 2, 0x5678);
    
    EXPECT_EQ(memory->read_u16(addr), 0x1234);
    EXPECT_EQ(memory->read_u16(addr + 2), 0x5678);
}

TEST_F(MemoryTest, DoubleWordReadWrite) {
    GuestAddr addr = 0x00100300;
    
    memory->write_u64(addr, 0x123456789ABCDEF0ULL);
    EXPECT_EQ(memory->read_u64(addr), 0x123456789ABCDEF0ULL);
}

TEST_F(MemoryTest, BlockWrite) {
    GuestAddr addr = 0x00100400;
    u8 data[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    
    for (int i = 0; i < 16; i++) {
        memory->write_u8(addr + i, data[i]);
    }
    
    for (int i = 0; i < 16; i++) {
        EXPECT_EQ(memory->read_u8(addr + i), data[i]);
    }
}

TEST_F(MemoryTest, Alignment) {
    // Test that unaligned access works correctly
    GuestAddr addr = 0x00100501;  // Unaligned address
    
    memory->write_u32(addr, 0xCAFEBABE);
    EXPECT_EQ(memory->read_u32(addr), 0xCAFEBABE);
}

} // namespace test
} // namespace x360mu
