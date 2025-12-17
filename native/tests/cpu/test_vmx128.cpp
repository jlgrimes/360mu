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
    VMX128Unit vmx;
    
    void SetUp() override {
        vmx.initialize();
    }
    
    void TearDown() override {
    }
};

TEST_F(VMX128Test, VectorAdd) {
    VectorReg a, b, result;
    
    // Set up test vectors
    a.f32[0] = 1.0f;
    a.f32[1] = 2.0f;
    a.f32[2] = 3.0f;
    a.f32[3] = 4.0f;
    
    b.f32[0] = 5.0f;
    b.f32[1] = 6.0f;
    b.f32[2] = 7.0f;
    b.f32[3] = 8.0f;
    
    result = vmx.vaddfp(a, b);
    
    EXPECT_FLOAT_EQ(result.f32[0], 6.0f);
    EXPECT_FLOAT_EQ(result.f32[1], 8.0f);
    EXPECT_FLOAT_EQ(result.f32[2], 10.0f);
    EXPECT_FLOAT_EQ(result.f32[3], 12.0f);
}

TEST_F(VMX128Test, VectorMultiply) {
    VectorReg a, b, result;
    
    a.f32[0] = 2.0f;
    a.f32[1] = 3.0f;
    a.f32[2] = 4.0f;
    a.f32[3] = 5.0f;
    
    b.f32[0] = 2.0f;
    b.f32[1] = 2.0f;
    b.f32[2] = 2.0f;
    b.f32[3] = 2.0f;
    
    result = vmx.vmulfp(a, b);
    
    EXPECT_FLOAT_EQ(result.f32[0], 4.0f);
    EXPECT_FLOAT_EQ(result.f32[1], 6.0f);
    EXPECT_FLOAT_EQ(result.f32[2], 8.0f);
    EXPECT_FLOAT_EQ(result.f32[3], 10.0f);
}

TEST_F(VMX128Test, DotProduct) {
    VectorReg a, b, result;
    
    a.f32[0] = 1.0f;
    a.f32[1] = 2.0f;
    a.f32[2] = 3.0f;
    a.f32[3] = 0.0f;
    
    b.f32[0] = 4.0f;
    b.f32[1] = 5.0f;
    b.f32[2] = 6.0f;
    b.f32[3] = 0.0f;
    
    result = vmx.vdot3(a, b);
    
    // 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    EXPECT_FLOAT_EQ(result.f32[0], 32.0f);
}

TEST_F(VMX128Test, CrossProduct) {
    VectorReg a, b, result;
    
    // X axis
    a.f32[0] = 1.0f;
    a.f32[1] = 0.0f;
    a.f32[2] = 0.0f;
    a.f32[3] = 0.0f;
    
    // Y axis
    b.f32[0] = 0.0f;
    b.f32[1] = 1.0f;
    b.f32[2] = 0.0f;
    b.f32[3] = 0.0f;
    
    result = vmx.vcross3(a, b);
    
    // X cross Y = Z
    EXPECT_FLOAT_EQ(result.f32[0], 0.0f);
    EXPECT_FLOAT_EQ(result.f32[1], 0.0f);
    EXPECT_FLOAT_EQ(result.f32[2], 1.0f);
}

} // namespace test
} // namespace x360mu

