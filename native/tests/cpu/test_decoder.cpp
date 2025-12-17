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
};

TEST_F(DecoderTest, DecodeAdd) {
    // add r3, r4, r5 (opcode 31, xo 266)
    // PowerPC encoding: opcode(6) | rd(5) | ra(5) | rb(5) | xo(10) | rc(1)
    // 31<<26 | 3<<21 | 4<<16 | 5<<11 | 266<<1 | 0 = 0x7C640214
    u32 inst = (31 << 26) | (3 << 21) | (4 << 16) | (5 << 11) | (266 << 1) | 0;
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 31);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 4);
    EXPECT_EQ(decoded.rb, 5);
}

TEST_F(DecoderTest, DecodeAddi) {
    // addi r3, r4, 100
    // Encoding: opcode 14, rd=3, ra=4, simm=100
    u32 inst = (14 << 26) | (3 << 21) | (4 << 16) | 100;
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 14);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 4);
    EXPECT_EQ(decoded.simm, 100);
}

TEST_F(DecoderTest, DecodeBranch) {
    // b +0x100
    u32 inst = (18 << 26) | 0x100;
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 18);
}

TEST_F(DecoderTest, DecodeLoadWord) {
    // lwz r3, 0x10(r4)
    u32 inst = (32 << 26) | (3 << 21) | (4 << 16) | 0x10;
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 32);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 4);
    EXPECT_EQ(decoded.simm, 0x10);
}

} // namespace test
} // namespace x360mu
