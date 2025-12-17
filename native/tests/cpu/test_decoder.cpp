/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * PowerPC Decoder Tests
 */

#include <gtest/gtest.h>
#include "cpu/xenon/cpu.h"

namespace x360mu {
namespace test {

class DecoderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code
    }
    
    void TearDown() override {
        // Cleanup code
    }
};

TEST_F(DecoderTest, DecodeBranch) {
    // Test branch instruction decoding
    // b 0x1000
    u32 inst = 0x48001000;  // b +0x1000
    
    Decoder decoder;
    DecodedInst decoded = decoder.decode(inst);
    
    EXPECT_EQ(decoded.op, Opcode::B);
}

TEST_F(DecoderTest, DecodeLoadWord) {
    // Test lwz instruction
    // lwz r3, 0x10(r1)
    u32 inst = 0x80610010;
    
    Decoder decoder;
    DecodedInst decoded = decoder.decode(inst);
    
    EXPECT_EQ(decoded.op, Opcode::LWZ);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 1);
    EXPECT_EQ(decoded.imm, 0x10);
}

TEST_F(DecoderTest, DecodeStoreWord) {
    // Test stw instruction
    // stw r3, 0x20(r1)
    u32 inst = 0x90610020;
    
    Decoder decoder;
    DecodedInst decoded = decoder.decode(inst);
    
    EXPECT_EQ(decoded.op, Opcode::STW);
}

TEST_F(DecoderTest, DecodeAdd) {
    // Test add instruction
    u32 inst = 0x7C632214;  // add r3, r3, r4
    
    Decoder decoder;
    DecodedInst decoded = decoder.decode(inst);
    
    EXPECT_EQ(decoded.op, Opcode::ADD);
}

TEST_F(DecoderTest, DecodeAddi) {
    // Test addi instruction
    // addi r3, r4, 100
    u32 inst = 0x38640064;
    
    Decoder decoder;
    DecodedInst decoded = decoder.decode(inst);
    
    EXPECT_EQ(decoded.op, Opcode::ADDI);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 4);
    EXPECT_EQ(decoded.imm, 100);
}

} // namespace test
} // namespace x360mu

