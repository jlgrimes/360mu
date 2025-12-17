/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * PowerPC Interpreter Tests
 */

#include <gtest/gtest.h>
#include "cpu/xenon/cpu.h"
#include "memory/memory.h"

namespace x360mu {
namespace test {

class InterpreterTest : public ::testing::Test {
protected:
    std::unique_ptr<Memory> memory;
    std::unique_ptr<Interpreter> interp;
    ThreadContext ctx;
    
    void SetUp() override {
        memory = std::make_unique<Memory>();
        Status status = memory->initialize();
        ASSERT_EQ(status, Status::Ok);
        
        interp = std::make_unique<Interpreter>(memory.get());
        ctx.reset();
        ctx.pc = 0x10000;
    }
    
    void TearDown() override {
        memory->shutdown();
    }
    
    // Helper to write instruction and execute
    void execute_instruction(u32 inst) {
        memory->write_u32(static_cast<GuestAddr>(ctx.pc), inst);
        interp->execute_one(ctx);
    }
    
    // Instruction encoding helpers
    static u32 encode_add(u8 rd, u8 ra, u8 rb) {
        // add rD, rA, rB: opcode=31, xo=266
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266 << 1);
    }
    
    static u32 encode_addi(u8 rd, u8 ra, s16 simm) {
        // addi rD, rA, SIMM: opcode=14
        return (14 << 26) | (rd << 21) | (ra << 16) | (static_cast<u16>(simm));
    }
    
    static u32 encode_mulld(u8 rd, u8 ra, u8 rb) {
        // mulld rD, rA, rB: opcode=31, xo=233
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (233 << 1);
    }
    
    static u32 encode_mulhd(u8 rd, u8 ra, u8 rb) {
        // mulhd rD, rA, rB: opcode=31, xo=73
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (73 << 1);
    }
    
    static u32 encode_mulhdu(u8 rd, u8 ra, u8 rb) {
        // mulhdu rD, rA, rB: opcode=31, xo=9
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (9 << 1);
    }
    
    static u32 encode_divd(u8 rd, u8 ra, u8 rb) {
        // divd rD, rA, rB: opcode=31, xo=489
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (489 << 1);
    }
    
    static u32 encode_divdu(u8 rd, u8 ra, u8 rb) {
        // divdu rD, rA, rB: opcode=31, xo=457
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (457 << 1);
    }
    
    static u32 encode_sld(u8 ra, u8 rs, u8 rb) {
        // sld rA, rS, rB: opcode=31, xo=27
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (27 << 1);
    }
    
    static u32 encode_srd(u8 ra, u8 rs, u8 rb) {
        // srd rA, rS, rB: opcode=31, xo=539
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (539 << 1);
    }
    
    static u32 encode_srad(u8 ra, u8 rs, u8 rb) {
        // srad rA, rS, rB: opcode=31, xo=794
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (794 << 1);
    }
    
    static u32 encode_lwarx(u8 rd, u8 ra, u8 rb) {
        // lwarx rD, rA, rB: opcode=31, xo=20
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (20 << 1);
    }
    
    static u32 encode_stwcx(u8 rs, u8 ra, u8 rb) {
        // stwcx. rS, rA, rB: opcode=31, xo=150
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (150 << 1) | 1;
    }
    
    static u32 encode_ld(u8 rd, u8 ra, s16 ds) {
        // ld rD, DS(rA): opcode=58, XO=0
        return (58 << 26) | (rd << 21) | (ra << 16) | ((ds & 0xFFFC) | 0);
    }
    
    static u32 encode_std(u8 rs, u8 ra, s16 ds) {
        // std rS, DS(rA): opcode=62, XO=0
        return (62 << 26) | (rs << 21) | (ra << 16) | ((ds & 0xFFFC) | 0);
    }
};

//=============================================================================
// Basic Tests
//=============================================================================

TEST_F(InterpreterTest, BasicOperation) {
    EXPECT_TRUE(interp != nullptr);
    EXPECT_TRUE(memory != nullptr);
}

TEST_F(InterpreterTest, RegisterReset) {
    ctx.gpr[0] = 0x12345678;
    ctx.reset();
    EXPECT_EQ(ctx.gpr[0], 0u);
}

TEST_F(InterpreterTest, ConditionRegisterReset) {
    ctx.cr[0].lt = true;
    ctx.cr[0].gt = true;
    ctx.reset();
    EXPECT_FALSE(ctx.cr[0].lt);
    EXPECT_FALSE(ctx.cr[0].gt);
}

//=============================================================================
// 64-bit Integer Operations
//=============================================================================

TEST_F(InterpreterTest, Mulld_Basic) {
    // mulld r5, r3, r4: low 64 bits of 64x64 multiply
    ctx.gpr[3] = 0x100000000ULL;
    ctx.gpr[4] = 0x100000000ULL;
    execute_instruction(encode_mulld(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0ULL);  // Low 64 bits overflow to 0
}

TEST_F(InterpreterTest, Mulld_SmallValues) {
    ctx.gpr[3] = 1000;
    ctx.gpr[4] = 2000;
    execute_instruction(encode_mulld(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 2000000ULL);
}

TEST_F(InterpreterTest, Mulhd_Basic) {
    // mulhd r5, r3, r4: high 64 bits of signed 64x64 multiply
    ctx.gpr[3] = 0x100000000ULL;
    ctx.gpr[4] = 0x100000000ULL;
    execute_instruction(encode_mulhd(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 1ULL);  // High 64 bits = 1
}

TEST_F(InterpreterTest, Mulhdu_Basic) {
    // mulhdu r5, r3, r4: high 64 bits of unsigned 64x64 multiply
    ctx.gpr[3] = 0x8000000000000000ULL;
    ctx.gpr[4] = 2;
    execute_instruction(encode_mulhdu(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 1ULL);
}

TEST_F(InterpreterTest, Divd_Basic) {
    // divd r5, r3, r4: signed 64-bit divide
    ctx.gpr[3] = 100;
    ctx.gpr[4] = 10;
    execute_instruction(encode_divd(5, 3, 4));
    EXPECT_EQ(static_cast<s64>(ctx.gpr[5]), 10);
}

TEST_F(InterpreterTest, Divd_Negative) {
    ctx.gpr[3] = static_cast<u64>(-100);
    ctx.gpr[4] = 10;
    execute_instruction(encode_divd(5, 3, 4));
    EXPECT_EQ(static_cast<s64>(ctx.gpr[5]), -10);
}

TEST_F(InterpreterTest, Divdu_Basic) {
    // divdu r5, r3, r4: unsigned 64-bit divide
    ctx.gpr[3] = 0xFFFFFFFFFFFFFFFFULL;
    ctx.gpr[4] = 2;
    execute_instruction(encode_divdu(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0x7FFFFFFFFFFFFFFFULL);
}

//=============================================================================
// 64-bit Shift Operations
//=============================================================================

TEST_F(InterpreterTest, Sld_Basic) {
    // sld rA, rS, rB: shift left doubleword
    ctx.gpr[3] = 1;
    ctx.gpr[4] = 32;
    execute_instruction(encode_sld(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0x100000000ULL);
}

TEST_F(InterpreterTest, Sld_LargeShift) {
    // Shift >= 64 should produce 0
    ctx.gpr[3] = 0xFFFFFFFFFFFFFFFFULL;
    ctx.gpr[4] = 64;
    execute_instruction(encode_sld(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0ULL);
}

TEST_F(InterpreterTest, Srd_Basic) {
    // srd rA, rS, rB: shift right doubleword
    ctx.gpr[3] = 0x100000000ULL;
    ctx.gpr[4] = 32;
    execute_instruction(encode_srd(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 1ULL);
}

TEST_F(InterpreterTest, Srad_Positive) {
    // srad rA, rS, rB: shift right algebraic doubleword
    ctx.gpr[3] = 0x100000000ULL;
    ctx.gpr[4] = 16;
    execute_instruction(encode_srad(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0x10000ULL);
    EXPECT_FALSE(ctx.xer.ca);  // No carry for positive
}

TEST_F(InterpreterTest, Srad_Negative) {
    ctx.gpr[3] = 0xFFFFFFFF80000000ULL;  // -0x80000000
    ctx.gpr[4] = 4;
    execute_instruction(encode_srad(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0xFFFFFFFFF8000000ULL);  // Sign extended
}

//=============================================================================
// Atomic Operations
//=============================================================================

TEST_F(InterpreterTest, Lwarx_Stwcx_Success) {
    // Setup memory
    GuestAddr addr = 0x20000;
    memory->write_u32(addr, 42);
    ctx.gpr[4] = addr;
    ctx.gpr[5] = 0;  // Base register = 0
    
    // lwarx r3, 0, r4
    execute_instruction(encode_lwarx(3, 5, 4));
    EXPECT_EQ(ctx.gpr[3], 42u);
    
    // stwcx. r6, 0, r4
    ctx.gpr[6] = 100;
    ctx.pc = 0x10000;  // Reset PC for next instruction
    execute_instruction(encode_stwcx(6, 5, 4));
    
    // Should succeed - EQ bit set
    EXPECT_TRUE(ctx.cr[0].eq);
    EXPECT_EQ(memory->read_u32(addr), 100u);
}

TEST_F(InterpreterTest, Stwcx_Failure_NoReservation) {
    // Setup memory without lwarx first
    GuestAddr addr = 0x20000;
    memory->write_u32(addr, 42);
    ctx.gpr[4] = addr;
    ctx.gpr[5] = 0;
    ctx.gpr[6] = 100;
    
    // Clear any existing reservation
    memory->clear_reservation();
    
    // stwcx. r6, 0, r4 - should fail
    execute_instruction(encode_stwcx(6, 5, 4));
    
    // Should fail - EQ bit cleared
    EXPECT_FALSE(ctx.cr[0].eq);
    EXPECT_EQ(memory->read_u32(addr), 42u);  // Unchanged
}

//=============================================================================
// 64-bit Load/Store (ld/std)
//=============================================================================

TEST_F(InterpreterTest, Ld_Basic) {
    // ld r3, 0(r4)
    GuestAddr addr = 0x20000;
    memory->write_u64(addr, 0x123456789ABCDEF0ULL);
    ctx.gpr[4] = addr;
    
    execute_instruction(encode_ld(3, 4, 0));
    EXPECT_EQ(ctx.gpr[3], 0x123456789ABCDEF0ULL);
}

TEST_F(InterpreterTest, Std_Basic) {
    // std r3, 0(r4)
    GuestAddr addr = 0x20000;
    ctx.gpr[3] = 0xDEADBEEFCAFEBABEULL;
    ctx.gpr[4] = addr;
    
    execute_instruction(encode_std(3, 4, 0));
    EXPECT_EQ(memory->read_u64(addr), 0xDEADBEEFCAFEBABEULL);
}

TEST_F(InterpreterTest, Ld_WithDisplacement) {
    // ld r3, 16(r4)
    GuestAddr base = 0x20000;
    memory->write_u64(base + 16, 0xABCDEF0123456789ULL);
    ctx.gpr[4] = base;
    
    execute_instruction(encode_ld(3, 4, 16));
    EXPECT_EQ(ctx.gpr[3], 0xABCDEF0123456789ULL);
}

//=============================================================================
// Integer Addition with Carry
//=============================================================================

TEST_F(InterpreterTest, Add_Basic) {
    ctx.gpr[3] = 100;
    ctx.gpr[4] = 200;
    execute_instruction(encode_add(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 300ULL);
}

TEST_F(InterpreterTest, Addi_Basic) {
    ctx.gpr[3] = 1000;
    execute_instruction(encode_addi(5, 3, 234));
    EXPECT_EQ(ctx.gpr[5], 1234ULL);
}

TEST_F(InterpreterTest, Addi_Negative) {
    ctx.gpr[3] = 1000;
    execute_instruction(encode_addi(5, 3, -100));
    EXPECT_EQ(ctx.gpr[5], 900ULL);
}

} // namespace test
} // namespace x360mu
