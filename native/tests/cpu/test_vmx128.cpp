/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * VMX128 SIMD Tests
 */

#include <gtest/gtest.h>
#include "cpu/vmx128/vmx.h"

namespace x360mu {
namespace test {

class VMX128Test : public ::testing::Test {
protected:
    Vmx128Unit vmx;
    
    void SetUp() override {
    }
    
    void TearDown() override {
    }
};

TEST_F(VMX128Test, VectorRegisterUnion) {
    // Test that VectorReg union works correctly
    VectorReg v;
    v.u32x4[0] = 0x3F800000;  // 1.0f in IEEE 754
    v.u32x4[1] = 0x40000000;  // 2.0f
    v.u32x4[2] = 0x40400000;  // 3.0f
    v.u32x4[3] = 0x40800000;  // 4.0f
    
    EXPECT_FLOAT_EQ(v.f32x4[0], 1.0f);
    EXPECT_FLOAT_EQ(v.f32x4[1], 2.0f);
    EXPECT_FLOAT_EQ(v.f32x4[2], 3.0f);
    EXPECT_FLOAT_EQ(v.f32x4[3], 4.0f);
}

TEST_F(VMX128Test, VectorRegisterBytes) {
    VectorReg v;
    for (int i = 0; i < 16; i++) {
        v.u8x16[i] = static_cast<u8>(i);
    }
    
    // Check byte order
    EXPECT_EQ(v.u8x16[0], 0);
    EXPECT_EQ(v.u8x16[15], 15);
}

TEST_F(VMX128Test, InstructionDecode) {
    // Test VMX instruction decoding
    u32 inst = (4 << 26) | (10 << 21) | (11 << 16) | (12 << 11);
    Vmx128Inst decoded = Vmx128Unit::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 4);
    EXPECT_EQ(decoded.vd, 10);
    EXPECT_EQ(decoded.va, 11);
    EXPECT_EQ(decoded.vb, 12);
}

} // namespace test
} // namespace x360mu
