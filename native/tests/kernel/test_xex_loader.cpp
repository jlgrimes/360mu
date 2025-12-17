/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * XEX Loader Tests
 */

#include <gtest/gtest.h>
#include "kernel/xex_loader.h"

namespace x360mu {
namespace test {

class XexLoaderTest : public ::testing::Test {
protected:
    XexTestHarness harness;
    
    void SetUp() override {
        ASSERT_EQ(harness.initialize(), Status::Ok);
    }
    
    void TearDown() override {
        harness.shutdown();
    }
};

TEST_F(XexLoaderTest, InvalidFile) {
    // Test loading non-existent file
    Status status = harness.load_xex("nonexistent.xex");
    EXPECT_NE(status, Status::Ok);
}

TEST_F(XexLoaderTest, InvalidMagic) {
    // Create a buffer with invalid magic
    u8 fake_xex[64] = { 0 };
    fake_xex[0] = 'X';
    fake_xex[1] = 'E';
    fake_xex[2] = 'X';
    fake_xex[3] = '1';  // Wrong magic - should be 'XEX2'
    
    XexLoader loader;
    Status status = loader.load_buffer(fake_xex, 64, "test.xex", harness.get_memory());
    
    EXPECT_NE(status, Status::Ok);
}

TEST_F(XexLoaderTest, ValidMagicMinimal) {
    // Create a minimal valid XEX2 header
    u8 minimal_xex[1024] = { 0 };
    
    // Magic: 'XEX2'
    minimal_xex[0] = 'X';
    minimal_xex[1] = 'E';
    minimal_xex[2] = 'X';
    minimal_xex[3] = '2';
    
    // Module flags (big-endian)
    minimal_xex[4] = 0x00;
    minimal_xex[5] = 0x00;
    minimal_xex[6] = 0x00;
    minimal_xex[7] = 0x01;  // Title flag
    
    // PE data offset (big-endian)
    minimal_xex[8] = 0x00;
    minimal_xex[9] = 0x00;
    minimal_xex[10] = 0x01;
    minimal_xex[11] = 0x00;  // 256
    
    // Reserved
    minimal_xex[12] = 0x00;
    minimal_xex[13] = 0x00;
    minimal_xex[14] = 0x00;
    minimal_xex[15] = 0x00;
    
    // Security offset (big-endian)
    minimal_xex[16] = 0x00;
    minimal_xex[17] = 0x00;
    minimal_xex[18] = 0x00;
    minimal_xex[19] = 0x80;  // 128
    
    // Header count
    minimal_xex[20] = 0x00;
    minimal_xex[21] = 0x00;
    minimal_xex[22] = 0x00;
    minimal_xex[23] = 0x02;  // 2 headers
    
    // Optional header 1: Base address
    minimal_xex[24] = 0x00;  // Key high
    minimal_xex[25] = 0x01;
    minimal_xex[26] = 0x02;
    minimal_xex[27] = 0x01;  // kImageBaseAddress
    minimal_xex[28] = 0x82;  // Base address
    minimal_xex[29] = 0x00;
    minimal_xex[30] = 0x00;
    minimal_xex[31] = 0x00;
    
    // Optional header 2: Entry point
    minimal_xex[32] = 0x00;  // Key high
    minimal_xex[33] = 0x01;
    minimal_xex[34] = 0x01;
    minimal_xex[35] = 0x00;  // kEntryPoint
    minimal_xex[36] = 0x82;  // Entry point
    minimal_xex[37] = 0x00;
    minimal_xex[38] = 0x10;
    minimal_xex[39] = 0x00;
    
    // Security info at offset 128
    minimal_xex[128] = 0x00;  // Header size
    minimal_xex[129] = 0x00;
    minimal_xex[130] = 0x01;
    minimal_xex[131] = 0x00;
    minimal_xex[132] = 0x00;  // Image size
    minimal_xex[133] = 0x00;
    minimal_xex[134] = 0x10;
    minimal_xex[135] = 0x00;
    
    XexLoader loader;
    Status status = loader.load_buffer(minimal_xex, sizeof(minimal_xex), "test.xex", nullptr);
    
    // Should parse the headers successfully
    EXPECT_EQ(status, Status::Ok);
    
    const XexModule* mod = loader.get_module();
    ASSERT_NE(mod, nullptr);
    
    EXPECT_EQ(mod->base_address, 0x82000000);
    EXPECT_EQ(mod->entry_point, 0x82001000);
    EXPECT_TRUE(mod->is_title);
}

} // namespace test
} // namespace x360mu

