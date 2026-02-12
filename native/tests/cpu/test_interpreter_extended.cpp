/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Extended PowerPC Interpreter Tests
 * Tests additional opcodes beyond the basic set in test_interpreter.cpp
 */

#include <gtest/gtest.h>
#include "cpu/xenon/cpu.h"
#include "memory/memory.h"

namespace x360mu {
namespace test {

class InterpreterExtTest : public ::testing::Test {
protected:
    std::unique_ptr<Memory> memory;
    std::unique_ptr<Interpreter> interp;
    ThreadContext ctx;

    void SetUp() override {
        memory = std::make_unique<Memory>();
        ASSERT_EQ(memory->initialize(), Status::Ok);
        interp = std::make_unique<Interpreter>(memory.get());
        ctx.reset();
        ctx.pc = 0x10000;
    }

    void TearDown() override {
        memory->shutdown();
    }

    void execute_instruction(u32 inst) {
        memory->write_u32(static_cast<GuestAddr>(ctx.pc), inst);
        interp->execute_one(ctx);
    }

    // --- Encoding helpers ---

    // ori rA, rS, UIMM (opcode 24)
    static u32 encode_ori(u8 ra, u8 rs, u16 uimm) {
        return (24 << 26) | (rs << 21) | (ra << 16) | uimm;
    }

    // oris rA, rS, UIMM (opcode 25)
    static u32 encode_oris(u8 ra, u8 rs, u16 uimm) {
        return (25 << 26) | (rs << 21) | (ra << 16) | uimm;
    }

    // xori rA, rS, UIMM (opcode 26)
    static u32 encode_xori(u8 ra, u8 rs, u16 uimm) {
        return (26 << 26) | (rs << 21) | (ra << 16) | uimm;
    }

    // xoris rA, rS, UIMM (opcode 27)
    static u32 encode_xoris(u8 ra, u8 rs, u16 uimm) {
        return (27 << 26) | (rs << 21) | (ra << 16) | uimm;
    }

    // andi. rA, rS, UIMM (opcode 28)
    static u32 encode_andi_dot(u8 ra, u8 rs, u16 uimm) {
        return (28 << 26) | (rs << 21) | (ra << 16) | uimm;
    }

    // andis. rA, rS, UIMM (opcode 29)
    static u32 encode_andis_dot(u8 ra, u8 rs, u16 uimm) {
        return (29 << 26) | (rs << 21) | (ra << 16) | uimm;
    }

    // addi rD, rA, SIMM (opcode 14)
    static u32 encode_addi(u8 rd, u8 ra, s16 simm) {
        return (14 << 26) | (rd << 21) | (ra << 16) | (static_cast<u16>(simm));
    }

    // addis rD, rA, SIMM (opcode 15)
    static u32 encode_addis(u8 rd, u8 ra, s16 simm) {
        return (15 << 26) | (rd << 21) | (ra << 16) | (static_cast<u16>(simm));
    }

    // subfic rD, rA, SIMM (opcode 8)
    static u32 encode_subfic(u8 rd, u8 ra, s16 simm) {
        return (8 << 26) | (rd << 21) | (ra << 16) | (static_cast<u16>(simm));
    }

    // subf rD, rA, rB (opcode 31, xo=40)
    static u32 encode_subf(u8 rd, u8 ra, u8 rb) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (40 << 1);
    }

    // neg rD, rA (opcode 31, xo=104)
    static u32 encode_neg(u8 rd, u8 ra) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (104 << 1);
    }

    // cmpi crfD, L, rA, SIMM (opcode 11)
    static u32 encode_cmpi(u8 crfd, u8 l, u8 ra, s16 simm) {
        return (11 << 26) | (crfd << 23) | (l << 21) | (ra << 16) | (static_cast<u16>(simm));
    }

    // cmpli crfD, L, rA, UIMM (opcode 10)
    static u32 encode_cmpli(u8 crfd, u8 l, u8 ra, u16 uimm) {
        return (10 << 26) | (crfd << 23) | (l << 21) | (ra << 16) | uimm;
    }

    // cmp crfD, L, rA, rB (opcode 31, xo=0)
    static u32 encode_cmp(u8 crfd, u8 l, u8 ra, u8 rb) {
        return (31 << 26) | (crfd << 23) | (l << 21) | (ra << 16) | (rb << 11) | (0 << 1);
    }

    // cmpl crfD, L, rA, rB (opcode 31, xo=32)
    static u32 encode_cmpl(u8 crfd, u8 l, u8 ra, u8 rb) {
        return (31 << 26) | (crfd << 23) | (l << 21) | (ra << 16) | (rb << 11) | (32 << 1);
    }

    // lwz rD, D(rA) (opcode 32)
    static u32 encode_lwz(u8 rd, u8 ra, s16 d) {
        return (32 << 26) | (rd << 21) | (ra << 16) | (static_cast<u16>(d));
    }

    // stw rS, D(rA) (opcode 36)
    static u32 encode_stw(u8 rs, u8 ra, s16 d) {
        return (36 << 26) | (rs << 21) | (ra << 16) | (static_cast<u16>(d));
    }

    // lbz rD, D(rA) (opcode 34)
    static u32 encode_lbz(u8 rd, u8 ra, s16 d) {
        return (34 << 26) | (rd << 21) | (ra << 16) | (static_cast<u16>(d));
    }

    // stb rS, D(rA) (opcode 38)
    static u32 encode_stb(u8 rs, u8 ra, s16 d) {
        return (38 << 26) | (rs << 21) | (ra << 16) | (static_cast<u16>(d));
    }

    // lhz rD, D(rA) (opcode 40)
    static u32 encode_lhz(u8 rd, u8 ra, s16 d) {
        return (40 << 26) | (rd << 21) | (ra << 16) | (static_cast<u16>(d));
    }

    // sth rS, D(rA) (opcode 44)
    static u32 encode_sth(u8 rs, u8 ra, s16 d) {
        return (44 << 26) | (rs << 21) | (ra << 16) | (static_cast<u16>(d));
    }

    // rlwinm rA, rS, SH, MB, ME (opcode 21)
    static u32 encode_rlwinm(u8 ra, u8 rs, u8 sh, u8 mb, u8 me) {
        return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1);
    }

    // and rA, rS, rB (opcode 31, xo=28)
    static u32 encode_and(u8 ra, u8 rs, u8 rb) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (28 << 1);
    }

    // or rA, rS, rB (opcode 31, xo=444)
    static u32 encode_or(u8 ra, u8 rs, u8 rb) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (444 << 1);
    }

    // xor rA, rS, rB (opcode 31, xo=316)
    static u32 encode_xor(u8 ra, u8 rs, u8 rb) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (316 << 1);
    }

    // nor rA, rS, rB (opcode 31, xo=124)
    static u32 encode_nor(u8 ra, u8 rs, u8 rb) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (124 << 1);
    }

    // nand rA, rS, rB (opcode 31, xo=476)
    static u32 encode_nand(u8 ra, u8 rs, u8 rb) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (476 << 1);
    }

    // mullw rD, rA, rB (opcode 31, xo=235)
    static u32 encode_mullw(u8 rd, u8 ra, u8 rb) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (235 << 1);
    }

    // divw rD, rA, rB (opcode 31, xo=491)
    static u32 encode_divw(u8 rd, u8 ra, u8 rb) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (491 << 1);
    }

    // divwu rD, rA, rB (opcode 31, xo=459)
    static u32 encode_divwu(u8 rd, u8 ra, u8 rb) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (459 << 1);
    }

    // mulli rD, rA, SIMM (opcode 7)
    static u32 encode_mulli(u8 rd, u8 ra, s16 simm) {
        return (7 << 26) | (rd << 21) | (ra << 16) | (static_cast<u16>(simm));
    }
};

//=============================================================================
// Logical Immediate Operations
//=============================================================================

TEST_F(InterpreterExtTest, Ori_Basic) {
    ctx.gpr[3] = 0xFF00;
    execute_instruction(encode_ori(5, 3, 0x00FF));
    EXPECT_EQ(ctx.gpr[5], 0xFFFFULL);
}

TEST_F(InterpreterExtTest, Ori_Nop) {
    // ori r0,r0,0 is the standard PPC NOP
    ctx.gpr[0] = 0;
    execute_instruction(encode_ori(0, 0, 0));
    EXPECT_EQ(ctx.gpr[0], 0ULL);
}

TEST_F(InterpreterExtTest, Oris_Basic) {
    ctx.gpr[3] = 0x00FF;
    execute_instruction(encode_oris(5, 3, 0xFF00));
    EXPECT_EQ(ctx.gpr[5], 0xFF0000FFULL);
}

TEST_F(InterpreterExtTest, Xori_Basic) {
    ctx.gpr[3] = 0xFFFF;
    execute_instruction(encode_xori(5, 3, 0xFF00));
    EXPECT_EQ(ctx.gpr[5], 0x00FFULL);
}

TEST_F(InterpreterExtTest, Xoris_Basic) {
    ctx.gpr[3] = 0xFFFF0000;
    execute_instruction(encode_xoris(5, 3, 0xFFFF));
    EXPECT_EQ(ctx.gpr[5], 0ULL);
}

TEST_F(InterpreterExtTest, Andi_Dot_Basic) {
    ctx.gpr[3] = 0xDEADBEEF;
    execute_instruction(encode_andi_dot(5, 3, 0xFF00));
    EXPECT_EQ(ctx.gpr[5], 0xBE00ULL);
}

TEST_F(InterpreterExtTest, Andi_Dot_UpdatesCR0) {
    ctx.gpr[3] = 0;
    execute_instruction(encode_andi_dot(5, 3, 0xFFFF));
    EXPECT_EQ(ctx.gpr[5], 0ULL);
    EXPECT_TRUE(ctx.cr[0].eq);
}

TEST_F(InterpreterExtTest, Andis_Dot_Basic) {
    ctx.gpr[3] = 0xCAFE0000;
    execute_instruction(encode_andis_dot(5, 3, 0xFF00));
    EXPECT_EQ(ctx.gpr[5], 0xCA000000ULL);
}

//=============================================================================
// Addis (Load Immediate Shifted)
//=============================================================================

TEST_F(InterpreterExtTest, Addis_Basic) {
    ctx.gpr[0] = 0;
    execute_instruction(encode_addis(5, 0, 0x1234));
    EXPECT_EQ(ctx.gpr[5], 0x12340000ULL);
}

TEST_F(InterpreterExtTest, Addis_WithBase) {
    ctx.gpr[3] = 0x5678;
    execute_instruction(encode_addis(5, 3, 0x1234));
    EXPECT_EQ(ctx.gpr[5], 0x12345678ULL);
}

//=============================================================================
// Subtract Operations
//=============================================================================

TEST_F(InterpreterExtTest, Subf_Basic) {
    // subf rD, rA, rB = rB - rA
    ctx.gpr[3] = 10;
    ctx.gpr[4] = 30;
    execute_instruction(encode_subf(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 20ULL);
}

TEST_F(InterpreterExtTest, Subfic_Basic) {
    // subfic rD, rA, SIMM = SIMM - rA
    ctx.gpr[3] = 10;
    execute_instruction(encode_subfic(5, 3, 100));
    EXPECT_EQ(ctx.gpr[5], 90ULL);
}

TEST_F(InterpreterExtTest, Neg_Basic) {
    ctx.gpr[3] = 42;
    execute_instruction(encode_neg(5, 3));
    EXPECT_EQ(static_cast<s64>(ctx.gpr[5]), -42);
}

TEST_F(InterpreterExtTest, Neg_Zero) {
    ctx.gpr[3] = 0;
    execute_instruction(encode_neg(5, 3));
    EXPECT_EQ(ctx.gpr[5], 0ULL);
}

//=============================================================================
// Compare Operations
//=============================================================================

TEST_F(InterpreterExtTest, Cmpi_LessThan) {
    ctx.gpr[3] = static_cast<u64>(static_cast<s64>(-5));
    execute_instruction(encode_cmpi(0, 0, 3, 10));
    EXPECT_TRUE(ctx.cr[0].lt);
    EXPECT_FALSE(ctx.cr[0].gt);
    EXPECT_FALSE(ctx.cr[0].eq);
}

TEST_F(InterpreterExtTest, Cmpi_Equal) {
    ctx.gpr[3] = 42;
    execute_instruction(encode_cmpi(0, 0, 3, 42));
    EXPECT_FALSE(ctx.cr[0].lt);
    EXPECT_FALSE(ctx.cr[0].gt);
    EXPECT_TRUE(ctx.cr[0].eq);
}

TEST_F(InterpreterExtTest, Cmpi_GreaterThan) {
    ctx.gpr[3] = 100;
    execute_instruction(encode_cmpi(0, 0, 3, 10));
    EXPECT_FALSE(ctx.cr[0].lt);
    EXPECT_TRUE(ctx.cr[0].gt);
    EXPECT_FALSE(ctx.cr[0].eq);
}

TEST_F(InterpreterExtTest, Cmpli_Unsigned) {
    ctx.gpr[3] = 0xFFFFFFFF;  // Large unsigned value
    execute_instruction(encode_cmpli(0, 0, 3, 0));
    EXPECT_FALSE(ctx.cr[0].lt);
    EXPECT_TRUE(ctx.cr[0].gt);
}

TEST_F(InterpreterExtTest, Cmp_Register) {
    ctx.gpr[3] = 5;
    ctx.gpr[4] = 10;
    execute_instruction(encode_cmp(0, 0, 3, 4));
    EXPECT_TRUE(ctx.cr[0].lt);
    EXPECT_FALSE(ctx.cr[0].gt);
}

TEST_F(InterpreterExtTest, Cmpl_Register_Unsigned) {
    ctx.gpr[3] = 0xFFFFFFFF;
    ctx.gpr[4] = 1;
    execute_instruction(encode_cmpl(0, 0, 3, 4));
    EXPECT_FALSE(ctx.cr[0].lt);
    EXPECT_TRUE(ctx.cr[0].gt);
}

TEST_F(InterpreterExtTest, Cmp_DifferentCRField) {
    ctx.gpr[3] = 5;
    ctx.gpr[4] = 10;
    execute_instruction(encode_cmp(2, 0, 3, 4));  // CR2
    EXPECT_TRUE(ctx.cr[2].lt);
    EXPECT_FALSE(ctx.cr[0].lt);  // CR0 unchanged
}

//=============================================================================
// Load/Store Operations
//=============================================================================

TEST_F(InterpreterExtTest, Lwz_Basic) {
    GuestAddr addr = 0x20000;
    memory->write_u32(addr, 0xDEADBEEF);
    ctx.gpr[4] = addr;
    execute_instruction(encode_lwz(3, 4, 0));
    EXPECT_EQ(ctx.gpr[3], 0xDEADBEEFULL);
}

TEST_F(InterpreterExtTest, Lwz_WithDisplacement) {
    GuestAddr addr = 0x20000;
    memory->write_u32(addr + 8, 0xCAFEBABE);
    ctx.gpr[4] = addr;
    execute_instruction(encode_lwz(3, 4, 8));
    EXPECT_EQ(ctx.gpr[3], 0xCAFEBABEULL);
}

TEST_F(InterpreterExtTest, Stw_Basic) {
    GuestAddr addr = 0x20000;
    ctx.gpr[3] = 0x12345678;
    ctx.gpr[4] = addr;
    execute_instruction(encode_stw(3, 4, 0));
    EXPECT_EQ(memory->read_u32(addr), 0x12345678u);
}

TEST_F(InterpreterExtTest, Lbz_Basic) {
    GuestAddr addr = 0x20000;
    memory->write_u8(addr, 0xAB);
    ctx.gpr[4] = addr;
    execute_instruction(encode_lbz(3, 4, 0));
    EXPECT_EQ(ctx.gpr[3], 0xABULL);
}

TEST_F(InterpreterExtTest, Stb_Basic) {
    GuestAddr addr = 0x20000;
    ctx.gpr[3] = 0xFF;
    ctx.gpr[4] = addr;
    execute_instruction(encode_stb(3, 4, 0));
    EXPECT_EQ(memory->read_u8(addr), 0xFFu);
}

TEST_F(InterpreterExtTest, Lhz_Basic) {
    GuestAddr addr = 0x20000;
    memory->write_u16(addr, 0xBEEF);
    ctx.gpr[4] = addr;
    execute_instruction(encode_lhz(3, 4, 0));
    EXPECT_EQ(ctx.gpr[3], 0xBEEFULL);
}

TEST_F(InterpreterExtTest, Sth_Basic) {
    GuestAddr addr = 0x20000;
    ctx.gpr[3] = 0x1234;
    ctx.gpr[4] = addr;
    execute_instruction(encode_sth(3, 4, 0));
    EXPECT_EQ(memory->read_u16(addr), 0x1234u);
}

TEST_F(InterpreterExtTest, LoadStore_Sequence) {
    // Simulate a common pattern: load, modify, store
    GuestAddr addr = 0x20000;
    memory->write_u32(addr, 100);
    ctx.gpr[4] = addr;

    // lwz r3, 0(r4)
    execute_instruction(encode_lwz(3, 4, 0));
    EXPECT_EQ(ctx.gpr[3], 100ULL);

    // addi r3, r3, 50
    ctx.pc = 0x10000;
    execute_instruction(encode_addi(3, 3, 50));
    EXPECT_EQ(ctx.gpr[3], 150ULL);

    // stw r3, 0(r4)
    ctx.pc = 0x10000;
    execute_instruction(encode_stw(3, 4, 0));
    EXPECT_EQ(memory->read_u32(addr), 150u);
}

//=============================================================================
// Rotate/Mask Operations
//=============================================================================

TEST_F(InterpreterExtTest, Rlwinm_ExtractByte) {
    // Extract MSB byte (bits 0-7): rlwinm rA, rS, 8, 24, 31
    ctx.gpr[3] = 0xAABBCCDD;
    execute_instruction(encode_rlwinm(5, 3, 8, 24, 31));
    EXPECT_EQ(ctx.gpr[5], 0xAAULL);
}

TEST_F(InterpreterExtTest, Rlwinm_ClearHighBits) {
    // clrlwi r5, r3, 16: rlwinm r5, r3, 0, 16, 31
    ctx.gpr[3] = 0xFFFF1234;
    execute_instruction(encode_rlwinm(5, 3, 0, 16, 31));
    EXPECT_EQ(ctx.gpr[5], 0x1234ULL);
}

TEST_F(InterpreterExtTest, Rlwinm_ShiftLeft) {
    // slwi r5, r3, 4: rlwinm r5, r3, 4, 0, 27
    ctx.gpr[3] = 0x0F;
    execute_instruction(encode_rlwinm(5, 3, 4, 0, 27));
    EXPECT_EQ(ctx.gpr[5], 0xF0ULL);
}

//=============================================================================
// Logical Register Operations
//=============================================================================

TEST_F(InterpreterExtTest, And_Basic) {
    ctx.gpr[3] = 0xFF00FF00;
    ctx.gpr[4] = 0x0FF00FF0;
    execute_instruction(encode_and(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0x0F000F00ULL);
}

TEST_F(InterpreterExtTest, Or_Basic) {
    ctx.gpr[3] = 0xFF00FF00;
    ctx.gpr[4] = 0x00FF00FF;
    execute_instruction(encode_or(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0xFFFFFFFFULL);
}

TEST_F(InterpreterExtTest, Or_MoveRegister) {
    // mr r5, r3 is encoded as or r5, r3, r3
    ctx.gpr[3] = 0xDEADBEEF;
    execute_instruction(encode_or(5, 3, 3));
    EXPECT_EQ(ctx.gpr[5], 0xDEADBEEFULL);
}

TEST_F(InterpreterExtTest, Xor_Basic) {
    ctx.gpr[3] = 0xFFFF0000;
    ctx.gpr[4] = 0xFF00FF00;
    execute_instruction(encode_xor(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0x00FFFF00ULL);
}

TEST_F(InterpreterExtTest, Nor_Basic) {
    ctx.gpr[3] = 0;
    ctx.gpr[4] = 0;
    execute_instruction(encode_nor(5, 3, 4));
    // nor of two zeros = NOT(0 | 0) = all ones
    EXPECT_EQ(ctx.gpr[5], 0xFFFFFFFFFFFFFFFFULL);
}

TEST_F(InterpreterExtTest, Nand_Basic) {
    ctx.gpr[3] = 0xFFFFFFFFFFFFFFFFULL;
    ctx.gpr[4] = 0xFFFFFFFFFFFFFFFFULL;
    execute_instruction(encode_nand(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0ULL);
}

//=============================================================================
// 32-bit Multiply/Divide
//=============================================================================

TEST_F(InterpreterExtTest, Mullw_Basic) {
    ctx.gpr[3] = 100;
    ctx.gpr[4] = 200;
    execute_instruction(encode_mullw(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 20000ULL);
}

TEST_F(InterpreterExtTest, Mullw_Negative) {
    ctx.gpr[3] = static_cast<u64>(static_cast<s32>(-10));
    ctx.gpr[4] = 5;
    execute_instruction(encode_mullw(5, 3, 4));
    // Result should be -50 sign extended to 64 bits
    EXPECT_EQ(static_cast<s64>(ctx.gpr[5]), -50);
}

TEST_F(InterpreterExtTest, Divw_Basic) {
    ctx.gpr[3] = 100;
    ctx.gpr[4] = 10;
    execute_instruction(encode_divw(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 10ULL);
}

TEST_F(InterpreterExtTest, Divwu_Basic) {
    ctx.gpr[3] = 0xFFFFFFFF;
    ctx.gpr[4] = 2;
    execute_instruction(encode_divwu(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 0x7FFFFFFFULL);
}

TEST_F(InterpreterExtTest, Mulli_Basic) {
    ctx.gpr[3] = 25;
    execute_instruction(encode_mulli(5, 3, 4));
    EXPECT_EQ(ctx.gpr[5], 100ULL);
}

TEST_F(InterpreterExtTest, Mulli_Negative) {
    ctx.gpr[3] = 10;
    execute_instruction(encode_mulli(5, 3, -3));
    EXPECT_EQ(static_cast<s64>(ctx.gpr[5]), -30);
}

//=============================================================================
// PC Advancement
//=============================================================================

TEST_F(InterpreterExtTest, PCAdvancesBy4) {
    u64 pc_before = ctx.pc;
    ctx.gpr[3] = 1;
    ctx.gpr[4] = 2;
    execute_instruction(encode_ori(5, 3, 0));
    EXPECT_EQ(ctx.pc, pc_before + 4);
}

//=============================================================================
// Edge Cases
//=============================================================================

TEST_F(InterpreterExtTest, LoadFromZeroBase) {
    // lwz r3, offset(r0) - when rA=0, base is 0 (not GPR[0])
    GuestAddr addr = 0x100;
    memory->write_u32(addr, 0xBAADF00D);
    ctx.gpr[0] = 0xDEAD;  // Should be ignored
    execute_instruction(encode_lwz(3, 0, 0x100));
    EXPECT_EQ(ctx.gpr[3], 0xBAADF00DULL);
}

TEST_F(InterpreterExtTest, Addi_WithZeroRA) {
    // addi r5, r0, 42 acts as li r5, 42 (rA=0 means literal 0)
    ctx.gpr[0] = 0xDEAD;
    execute_instruction(encode_addi(5, 0, 42));
    EXPECT_EQ(ctx.gpr[5], 42ULL);
}

} // namespace test
} // namespace x360mu
