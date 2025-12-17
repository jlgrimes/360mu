/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * VMX128 SIMD Tests
 */

#include <gtest/gtest.h>
#include "cpu/vmx128/vmx.h"
#include <cmath>

namespace x360mu {
namespace test {

class VMX128Test : public ::testing::Test {
protected:
    Vmx128Unit vmx;
    ThreadContext ctx;
    
    void SetUp() override {
        ctx.reset();
    }
    
    void TearDown() override {
    }
    
    // Helper to compare floats with tolerance
    static bool FloatNear(float a, float b, float epsilon = 1e-5f) {
        return std::fabs(a - b) < epsilon;
    }
};

//=============================================================================
// Basic Register Tests
//=============================================================================

TEST_F(VMX128Test, VectorRegisterUnion) {
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
    
    EXPECT_EQ(v.u8x16[0], 0);
    EXPECT_EQ(v.u8x16[15], 15);
}

TEST_F(VMX128Test, InstructionDecode) {
    u32 inst = (4 << 26) | (10 << 21) | (11 << 16) | (12 << 11);
    Vmx128Inst decoded = Vmx128Unit::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 4);
    EXPECT_EQ(decoded.vd, 10);
    EXPECT_EQ(decoded.va, 11);
    EXPECT_EQ(decoded.vb, 12);
}

//=============================================================================
// Float Arithmetic Tests
//=============================================================================

TEST_F(VMX128Test, VAddFp) {
    VectorReg va, vb, vd;
    va.f32x4[0] = 1.0f; va.f32x4[1] = 2.0f; va.f32x4[2] = 3.0f; va.f32x4[3] = 4.0f;
    vb.f32x4[0] = 5.0f; vb.f32x4[1] = 6.0f; vb.f32x4[2] = 7.0f; vb.f32x4[3] = 8.0f;
    
    vmx.vaddfp(vd, va, vb);
    
    EXPECT_FLOAT_EQ(vd.f32x4[0], 6.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[1], 8.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[2], 10.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[3], 12.0f);
}

TEST_F(VMX128Test, VSubFp) {
    VectorReg va, vb, vd;
    va.f32x4[0] = 10.0f; va.f32x4[1] = 20.0f; va.f32x4[2] = 30.0f; va.f32x4[3] = 40.0f;
    vb.f32x4[0] = 1.0f; vb.f32x4[1] = 2.0f; vb.f32x4[2] = 3.0f; vb.f32x4[3] = 4.0f;
    
    vmx.vsubfp(vd, va, vb);
    
    EXPECT_FLOAT_EQ(vd.f32x4[0], 9.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[1], 18.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[2], 27.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[3], 36.0f);
}

TEST_F(VMX128Test, VMulFp) {
    VectorReg va, vc, vd;
    va.f32x4[0] = 2.0f; va.f32x4[1] = 3.0f; va.f32x4[2] = 4.0f; va.f32x4[3] = 5.0f;
    vc.f32x4[0] = 3.0f; vc.f32x4[1] = 4.0f; vc.f32x4[2] = 5.0f; vc.f32x4[3] = 6.0f;
    
    vmx.vmulfp(vd, va, vc);
    
    EXPECT_FLOAT_EQ(vd.f32x4[0], 6.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[1], 12.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[2], 20.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[3], 30.0f);
}

TEST_F(VMX128Test, VMaddFp) {
    VectorReg va, vb, vc, vd;
    va.f32x4[0] = 2.0f; va.f32x4[1] = 3.0f; va.f32x4[2] = 4.0f; va.f32x4[3] = 5.0f;
    vb.f32x4[0] = 1.0f; vb.f32x4[1] = 1.0f; vb.f32x4[2] = 1.0f; vb.f32x4[3] = 1.0f;
    vc.f32x4[0] = 3.0f; vc.f32x4[1] = 4.0f; vc.f32x4[2] = 5.0f; vc.f32x4[3] = 6.0f;
    
    // vd = (va * vc) + vb
    vmx.vmaddfp(vd, va, vb, vc);
    
    EXPECT_FLOAT_EQ(vd.f32x4[0], 7.0f);   // 2*3 + 1
    EXPECT_FLOAT_EQ(vd.f32x4[1], 13.0f);  // 3*4 + 1
    EXPECT_FLOAT_EQ(vd.f32x4[2], 21.0f);  // 4*5 + 1
    EXPECT_FLOAT_EQ(vd.f32x4[3], 31.0f);  // 5*6 + 1
}

//=============================================================================
// Dot Product Tests (Critical for Games)
//=============================================================================

TEST_F(VMX128Test, VDot3Fp) {
    VectorReg va, vb, vd;
    // Vector A = (1, 2, 3, 0)
    va.f32x4[0] = 1.0f; va.f32x4[1] = 2.0f; va.f32x4[2] = 3.0f; va.f32x4[3] = 0.0f;
    // Vector B = (4, 5, 6, 0)
    vb.f32x4[0] = 4.0f; vb.f32x4[1] = 5.0f; vb.f32x4[2] = 6.0f; vb.f32x4[3] = 0.0f;
    
    // Dot3 = 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    vmx.vdot3fp(vd, va, vb);
    
    EXPECT_FLOAT_EQ(vd.f32x4[0], 32.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[1], 32.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[2], 32.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[3], 32.0f);
}

TEST_F(VMX128Test, VDot4Fp) {
    VectorReg va, vb, vd;
    // Vector A = (1, 2, 3, 4)
    va.f32x4[0] = 1.0f; va.f32x4[1] = 2.0f; va.f32x4[2] = 3.0f; va.f32x4[3] = 4.0f;
    // Vector B = (5, 6, 7, 8)
    vb.f32x4[0] = 5.0f; vb.f32x4[1] = 6.0f; vb.f32x4[2] = 7.0f; vb.f32x4[3] = 8.0f;
    
    // Dot4 = 1*5 + 2*6 + 3*7 + 4*8 = 5 + 12 + 21 + 32 = 70
    vmx.vdot4fp(vd, va, vb);
    
    EXPECT_FLOAT_EQ(vd.f32x4[0], 70.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[1], 70.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[2], 70.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[3], 70.0f);
}

//=============================================================================
// Cross Product Test (Physics/Lighting)
//=============================================================================

TEST_F(VMX128Test, VCross3Fp) {
    VectorReg va, vb, vd;
    // X axis = (1, 0, 0)
    va.f32x4[0] = 1.0f; va.f32x4[1] = 0.0f; va.f32x4[2] = 0.0f; va.f32x4[3] = 0.0f;
    // Y axis = (0, 1, 0)
    vb.f32x4[0] = 0.0f; vb.f32x4[1] = 1.0f; vb.f32x4[2] = 0.0f; vb.f32x4[3] = 0.0f;
    
    // X × Y = Z = (0, 0, 1)
    vmx.vcross3fp(vd, va, vb);
    
    EXPECT_FLOAT_EQ(vd.f32x4[0], 0.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[1], 0.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[2], 1.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[3], 0.0f);
}

TEST_F(VMX128Test, VCross3Fp_General) {
    VectorReg va, vb, vd;
    // A = (1, 2, 3)
    va.f32x4[0] = 1.0f; va.f32x4[1] = 2.0f; va.f32x4[2] = 3.0f; va.f32x4[3] = 0.0f;
    // B = (4, 5, 6)
    vb.f32x4[0] = 4.0f; vb.f32x4[1] = 5.0f; vb.f32x4[2] = 6.0f; vb.f32x4[3] = 0.0f;
    
    // A × B = (2*6 - 3*5, 3*4 - 1*6, 1*5 - 2*4) = (-3, 6, -3)
    vmx.vcross3fp(vd, va, vb);
    
    EXPECT_FLOAT_EQ(vd.f32x4[0], -3.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[1], 6.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[2], -3.0f);
    EXPECT_FLOAT_EQ(vd.f32x4[3], 0.0f);
}

//=============================================================================
// Shuffle Tests
//=============================================================================

TEST_F(VMX128Test, VShufD_Identity) {
    VectorReg vb, vd;
    vb.u32x4[0] = 0xAAAAAAAA;
    vb.u32x4[1] = 0xBBBBBBBB;
    vb.u32x4[2] = 0xCCCCCCCC;
    vb.u32x4[3] = 0xDDDDDDDD;
    
    // Identity shuffle: 0b11100100 = 0xE4 (3,2,1,0)
    vmx.vshufd(vd, vb, 0xE4);
    
    EXPECT_EQ(vd.u32x4[0], 0xAAAAAAAAu);
    EXPECT_EQ(vd.u32x4[1], 0xBBBBBBBBu);
    EXPECT_EQ(vd.u32x4[2], 0xCCCCCCCCu);
    EXPECT_EQ(vd.u32x4[3], 0xDDDDDDDDu);
}

TEST_F(VMX128Test, VShufD_Broadcast) {
    VectorReg vb, vd;
    vb.u32x4[0] = 0x11111111;
    vb.u32x4[1] = 0x22222222;
    vb.u32x4[2] = 0x33333333;
    vb.u32x4[3] = 0x44444444;
    
    // Broadcast element 0: 0b00000000 = 0x00
    vmx.vshufd(vd, vb, 0x00);
    
    EXPECT_EQ(vd.u32x4[0], 0x11111111u);
    EXPECT_EQ(vd.u32x4[1], 0x11111111u);
    EXPECT_EQ(vd.u32x4[2], 0x11111111u);
    EXPECT_EQ(vd.u32x4[3], 0x11111111u);
}

TEST_F(VMX128Test, VShufD_Reverse) {
    VectorReg vb, vd;
    vb.u32x4[0] = 1;
    vb.u32x4[1] = 2;
    vb.u32x4[2] = 3;
    vb.u32x4[3] = 4;
    
    // Reverse: 0b00011011 = 0x1B (0,1,2,3)
    vmx.vshufd(vd, vb, 0x1B);
    
    EXPECT_EQ(vd.u32x4[0], 4u);
    EXPECT_EQ(vd.u32x4[1], 3u);
    EXPECT_EQ(vd.u32x4[2], 2u);
    EXPECT_EQ(vd.u32x4[3], 1u);
}

//=============================================================================
// Logical Operations
//=============================================================================

TEST_F(VMX128Test, VAnd) {
    VectorReg va, vb, vd;
    va.u32x4[0] = 0xFF00FF00;
    va.u32x4[1] = 0xFF00FF00;
    va.u32x4[2] = 0xFF00FF00;
    va.u32x4[3] = 0xFF00FF00;
    vb.u32x4[0] = 0xFFFF0000;
    vb.u32x4[1] = 0xFFFF0000;
    vb.u32x4[2] = 0xFFFF0000;
    vb.u32x4[3] = 0xFFFF0000;
    
    vmx.vand(vd, va, vb);
    
    EXPECT_EQ(vd.u32x4[0], 0xFF000000u);
    EXPECT_EQ(vd.u32x4[1], 0xFF000000u);
}

TEST_F(VMX128Test, VOr) {
    VectorReg va, vb, vd;
    va.u32x4[0] = 0xFF00FF00;
    vb.u32x4[0] = 0x00FF00FF;
    
    vmx.vor(vd, va, vb);
    
    EXPECT_EQ(vd.u32x4[0], 0xFFFFFFFFu);
}

TEST_F(VMX128Test, VXor) {
    VectorReg va, vb, vd;
    va.u32x4[0] = 0xAAAAAAAA;
    vb.u32x4[0] = 0xFFFFFFFF;
    
    vmx.vxor(vd, va, vb);
    
    EXPECT_EQ(vd.u32x4[0], 0x55555555u);
}

//=============================================================================
// Splat Operations
//=============================================================================

TEST_F(VMX128Test, VSpltW) {
    VectorReg vb, vd;
    vb.u32x4[0] = 100;
    vb.u32x4[1] = 200;
    vb.u32x4[2] = 300;
    vb.u32x4[3] = 400;
    
    vmx.vspltw(vd, vb, 2);  // Splat element 2
    
    EXPECT_EQ(vd.u32x4[0], 300u);
    EXPECT_EQ(vd.u32x4[1], 300u);
    EXPECT_EQ(vd.u32x4[2], 300u);
    EXPECT_EQ(vd.u32x4[3], 300u);
}

TEST_F(VMX128Test, VSpltIsW) {
    VectorReg vd;
    
    vmx.vspltisw(vd, -1);  // Splat immediate signed
    
    EXPECT_EQ(vd.u32x4[0], 0xFFFFFFFFu);
    EXPECT_EQ(vd.u32x4[1], 0xFFFFFFFFu);
    EXPECT_EQ(vd.u32x4[2], 0xFFFFFFFFu);
    EXPECT_EQ(vd.u32x4[3], 0xFFFFFFFFu);
}

//=============================================================================
// Integer Arithmetic
//=============================================================================

TEST_F(VMX128Test, VAddUwm) {
    VectorReg va, vb, vd;
    va.u32x4[0] = 1; va.u32x4[1] = 2; va.u32x4[2] = 3; va.u32x4[3] = 4;
    vb.u32x4[0] = 10; vb.u32x4[1] = 20; vb.u32x4[2] = 30; vb.u32x4[3] = 40;
    
    vmx.vadd_uwm(vd, va, vb);
    
    EXPECT_EQ(vd.u32x4[0], 11u);
    EXPECT_EQ(vd.u32x4[1], 22u);
    EXPECT_EQ(vd.u32x4[2], 33u);
    EXPECT_EQ(vd.u32x4[3], 44u);
}

TEST_F(VMX128Test, VSubUwm) {
    VectorReg va, vb, vd;
    va.u32x4[0] = 100; va.u32x4[1] = 200; va.u32x4[2] = 300; va.u32x4[3] = 400;
    vb.u32x4[0] = 10; vb.u32x4[1] = 20; vb.u32x4[2] = 30; vb.u32x4[3] = 40;
    
    vmx.vsub_uwm(vd, va, vb);
    
    EXPECT_EQ(vd.u32x4[0], 90u);
    EXPECT_EQ(vd.u32x4[1], 180u);
    EXPECT_EQ(vd.u32x4[2], 270u);
    EXPECT_EQ(vd.u32x4[3], 360u);
}

} // namespace test
} // namespace x360mu
