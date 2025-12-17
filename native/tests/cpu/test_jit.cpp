/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * JIT Compiler Tests
 * 
 * Comprehensive tests for:
 * - ARM64 code emitter
 * - PowerPC instruction compilation
 * - Block cache management
 * - Register allocation
 */

#include <gtest/gtest.h>
#include <chrono>
#include "../../src/cpu/jit/jit.h"
#include "../../src/memory/memory.h"
#include "../../src/cpu/xenon/cpu.h"

namespace x360mu {
namespace test {

//=============================================================================
// Test Fixture Base
//=============================================================================

class JitTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        // Allocate code region
        ASSERT_EQ(memory_->allocate(CODE_BASE, CODE_SIZE, 
            MemoryRegion::Read | MemoryRegion::Write | MemoryRegion::Execute), 
            Status::Ok);
        
        // Allocate data region
        ASSERT_EQ(memory_->allocate(DATA_BASE, DATA_SIZE,
            MemoryRegion::Read | MemoryRegion::Write),
            Status::Ok);
    }
    
    void TearDown() override {
        memory_->shutdown();
    }
    
    // Write a PPC instruction to memory (big-endian)
    void write_ppc_inst(GuestAddr addr, u32 inst) {
        memory_->write_u32(addr, inst);
    }
    
    // PPC instruction encoders
    static u32 ppc_addi(int rd, int ra, s16 simm) {
        return (14 << 26) | (rd << 21) | (ra << 16) | (simm & 0xFFFF);
    }
    
    static u32 ppc_addis(int rd, int ra, s16 simm) {
        return (15 << 26) | (rd << 21) | (ra << 16) | (simm & 0xFFFF);
    }
    
    static u32 ppc_add(int rd, int ra, int rb, bool rc = false) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_subf(int rd, int ra, int rb, bool rc = false) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (40 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_mullw(int rd, int ra, int rb, bool rc = false) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (235 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_divw(int rd, int ra, int rb, bool rc = false) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (491 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_and(int ra, int rs, int rb, bool rc = false) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (28 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_or(int ra, int rs, int rb, bool rc = false) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (444 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_xor(int ra, int rs, int rb, bool rc = false) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (316 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_slw(int ra, int rs, int rb, bool rc = false) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (24 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_srw(int ra, int rs, int rb, bool rc = false) {
        return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (536 << 1) | (rc ? 1 : 0);
    }
    
    static u32 ppc_lwz(int rd, int ra, s16 offset) {
        return (32 << 26) | (rd << 21) | (ra << 16) | (offset & 0xFFFF);
    }
    
    static u32 ppc_lbz(int rd, int ra, s16 offset) {
        return (34 << 26) | (rd << 21) | (ra << 16) | (offset & 0xFFFF);
    }
    
    static u32 ppc_lhz(int rd, int ra, s16 offset) {
        return (40 << 26) | (rd << 21) | (ra << 16) | (offset & 0xFFFF);
    }
    
    static u32 ppc_stw(int rs, int ra, s16 offset) {
        return (36 << 26) | (rs << 21) | (ra << 16) | (offset & 0xFFFF);
    }
    
    static u32 ppc_stb(int rs, int ra, s16 offset) {
        return (38 << 26) | (rs << 21) | (ra << 16) | (offset & 0xFFFF);
    }
    
    static u32 ppc_sth(int rs, int ra, s16 offset) {
        return (44 << 26) | (rs << 21) | (ra << 16) | (offset & 0xFFFF);
    }
    
    static u32 ppc_b(s32 offset, bool link = false, bool absolute = false) {
        return (18 << 26) | (offset & 0x03FFFFFC) | (absolute ? 2 : 0) | (link ? 1 : 0);
    }
    
    static u32 ppc_bc(int bo, int bi, s16 offset, bool link = false, bool absolute = false) {
        return (16 << 26) | (bo << 21) | (bi << 16) | (offset & 0xFFFC) | (absolute ? 2 : 0) | (link ? 1 : 0);
    }
    
    static u32 ppc_blr() {
        return (19 << 26) | (0x14 << 21) | (16 << 1);
    }
    
    static u32 ppc_ori(int ra, int rs, u16 uimm) {
        return (24 << 26) | (rs << 21) | (ra << 16) | uimm;
    }
    
    static u32 ppc_nop() {
        return ppc_ori(0, 0, 0);
    }
    
    static u32 ppc_cmpwi(int crfd, int ra, s16 simm) {
        return (11 << 26) | (crfd << 23) | (ra << 16) | (simm & 0xFFFF);
    }
    
    static u32 ppc_cmplwi(int crfd, int ra, u16 uimm) {
        return (10 << 26) | (crfd << 23) | (ra << 16) | uimm;
    }
    
    static u32 ppc_mtspr(int spr, int rs) {
        int spr_lo = spr & 0x1F;
        int spr_hi = (spr >> 5) & 0x1F;
        return (31 << 26) | (rs << 21) | (spr_lo << 16) | (spr_hi << 11) | (467 << 1);
    }
    
    static u32 ppc_mfspr(int rd, int spr) {
        int spr_lo = spr & 0x1F;
        int spr_hi = (spr >> 5) & 0x1F;
        return (31 << 26) | (rd << 21) | (spr_lo << 16) | (spr_hi << 11) | (339 << 1);
    }
    
    static u32 ppc_rlwinm(int ra, int rs, int sh, int mb, int me, bool rc = false) {
        return (21 << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | (rc ? 1 : 0);
    }
    
    std::unique_ptr<Memory> memory_;
    
    static constexpr GuestAddr CODE_BASE = 0x82000000;
    static constexpr u64 CODE_SIZE = 64 * 1024;
    static constexpr GuestAddr DATA_BASE = 0x83000000;
    static constexpr u64 DATA_SIZE = 64 * 1024;
};

//=============================================================================
// ARM64 Emitter Tests
//=============================================================================

class ARM64EmitterTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer_.resize(4096);
        emit_ = std::make_unique<ARM64Emitter>(buffer_.data(), buffer_.size());
    }
    
    u32 get_inst(size_t index = 0) {
        return *reinterpret_cast<u32*>(buffer_.data() + index * 4);
    }
    
    std::vector<u8> buffer_;
    std::unique_ptr<ARM64Emitter> emit_;
};

TEST_F(ARM64EmitterTest, EmitAddImmediate) {
    emit_->ADD_imm(0, 1, 42);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    
    EXPECT_EQ(inst & 0xFF000000, 0x91000000);
    EXPECT_EQ((inst >> 5) & 0x1F, 1);
    EXPECT_EQ(inst & 0x1F, 0);
    EXPECT_EQ((inst >> 10) & 0xFFF, 42);
}

TEST_F(ARM64EmitterTest, EmitSubImmediate) {
    emit_->SUB_imm(2, 3, 100);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    
    EXPECT_EQ(inst & 0xFF000000, 0xD1000000);
    EXPECT_EQ(inst & 0x1F, 2);
    EXPECT_EQ((inst >> 5) & 0x1F, 3);
    EXPECT_EQ((inst >> 10) & 0xFFF, 100);
}

TEST_F(ARM64EmitterTest, EmitMovImm_Zero) {
    emit_->MOV_imm(5, 0);
    ASSERT_EQ(emit_->size(), 4);
}

TEST_F(ARM64EmitterTest, EmitMovImm_Small) {
    emit_->MOV_imm(0, 0x1234);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFF800000, 0xD2800000);
}

TEST_F(ARM64EmitterTest, EmitMovImm_Large) {
    emit_->MOV_imm(0, 0x123456789ABCDEF0ULL);
    
    ASSERT_GE(emit_->size(), 4);
    ASSERT_LE(emit_->size(), 16);
}

TEST_F(ARM64EmitterTest, EmitAddReg) {
    emit_->ADD(0, 1, 2);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    
    EXPECT_EQ(inst & 0xFF200000, 0x8B000000);
    EXPECT_EQ(inst & 0x1F, 0);
    EXPECT_EQ((inst >> 5) & 0x1F, 1);
    EXPECT_EQ((inst >> 16) & 0x1F, 2);
}

TEST_F(ARM64EmitterTest, EmitSubReg) {
    emit_->SUB(3, 4, 5);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    
    EXPECT_EQ(inst & 0xFF200000, 0xCB000000);
}

TEST_F(ARM64EmitterTest, EmitMul) {
    emit_->MUL(0, 1, 2);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFFE0FC00, 0x9B007C00);
}

TEST_F(ARM64EmitterTest, EmitDiv) {
    emit_->SDIV(0, 1, 2);
    emit_->UDIV(3, 4, 5);
    
    ASSERT_EQ(emit_->size(), 8);
}

TEST_F(ARM64EmitterTest, EmitLogicalReg) {
    emit_->AND(0, 1, 2);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFF200000, 0x8A000000);
    
    emit_->ORR(3, 4, 5);
    inst = get_inst(1);
    EXPECT_EQ(inst & 0xFF200000, 0xAA000000);
    
    emit_->EOR(6, 7, 8);
    inst = get_inst(2);
    EXPECT_EQ(inst & 0xFF200000, 0xCA000000);
}

TEST_F(ARM64EmitterTest, EmitShifts) {
    emit_->LSL(0, 1, 2);
    emit_->LSR(3, 4, 5);
    emit_->ASR(6, 7, 8);
    
    ASSERT_EQ(emit_->size(), 12);
}

TEST_F(ARM64EmitterTest, EmitLoadStore) {
    emit_->LDR(0, 1, 8);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFFC00000, 0xF9400000);
    
    emit_->STR(2, 3, 16);
    inst = get_inst(1);
    EXPECT_EQ(inst & 0xFFC00000, 0xF9000000);
}

TEST_F(ARM64EmitterTest, EmitBranch) {
    emit_->B(64);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFC000000, 0x14000000);
    EXPECT_EQ(inst & 0x03FFFFFF, 16);
}

TEST_F(ARM64EmitterTest, EmitConditionalBranch) {
    emit_->B_cond(arm64_cond::EQ, 32);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFF000010, 0x54000000);
    EXPECT_EQ(inst & 0x0F, arm64_cond::EQ);
}

TEST_F(ARM64EmitterTest, EmitBranchLink) {
    emit_->BL(128);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFC000000, 0x94000000);
}

TEST_F(ARM64EmitterTest, EmitBranchReg) {
    emit_->BR(arm64::X30);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFFFFFC1F, 0xD61F0000);
}

TEST_F(ARM64EmitterTest, EmitReturn) {
    emit_->RET();
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst, 0xD65F03C0);
}

TEST_F(ARM64EmitterTest, EmitCompare) {
    emit_->CMP(0, 1);
    emit_->CMP_imm(2, 42);
    
    ASSERT_EQ(emit_->size(), 8);
}

TEST_F(ARM64EmitterTest, EmitConditionalSet) {
    emit_->CSET(0, arm64_cond::EQ);
    emit_->CSET(1, arm64_cond::NE);
    emit_->CSET(2, arm64_cond::LT);
    emit_->CSET(3, arm64_cond::GT);
    
    ASSERT_EQ(emit_->size(), 16);
}

TEST_F(ARM64EmitterTest, EmitNeon) {
    emit_->FADD_vec(0, 1, 2, false);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = get_inst(0);
    EXPECT_EQ(inst & 0xFFA0FC00, 0x4E20D400);
}

TEST_F(ARM64EmitterTest, EmitByteReverse) {
    emit_->REV(0, 1);
    emit_->REV32(2, 3);
    emit_->REV16(4, 5);
    
    ASSERT_EQ(emit_->size(), 12);
}

TEST_F(ARM64EmitterTest, EmitExtend) {
    emit_->SXTB(0, 1);
    emit_->SXTH(2, 3);
    emit_->SXTW(4, 5);
    emit_->UXTB(6, 7);
    emit_->UXTH(8, 9);
    emit_->UXTW(10, 11);
    
    ASSERT_EQ(emit_->size(), 24);
}

TEST_F(ARM64EmitterTest, EmitCountLeadingZeros) {
    emit_->CLZ(0, 1);
    
    ASSERT_EQ(emit_->size(), 4);
}

//=============================================================================
// PPC Instruction Encoding Tests
//=============================================================================

class PPCEncodingTest : public ::testing::Test {};

TEST_F(PPCEncodingTest, EncodeAddi) {
    u32 inst = (14 << 26) | (3 << 21) | (0 << 16) | (42 & 0xFFFF);
    
    EXPECT_EQ((inst >> 26) & 0x3F, 14);
    EXPECT_EQ((inst >> 21) & 0x1F, 3);
    EXPECT_EQ((inst >> 16) & 0x1F, 0);
    EXPECT_EQ(inst & 0xFFFF, 42);
}

TEST_F(PPCEncodingTest, DecodeInstruction) {
    u32 inst = (14 << 26) | (3 << 21) | (1 << 16) | 100;
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 14);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 1);
    EXPECT_EQ(decoded.simm, 100);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Add);
}

TEST_F(PPCEncodingTest, DecodeAddInstruction) {
    u32 inst = (31 << 26) | (3 << 21) | (4 << 16) | (5 << 11) | (266 << 1);
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 31);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 4);
    EXPECT_EQ(decoded.rb, 5);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Add);
}

TEST_F(PPCEncodingTest, DecodeBranchInstruction) {
    u32 inst = (18 << 26) | (16 & 0x03FFFFFC);
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 18);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Branch);
    EXPECT_EQ(decoded.li, 16);
}

TEST_F(PPCEncodingTest, DecodeLoadInstruction) {
    u32 inst = (32 << 26) | (3 << 21) | (4 << 16) | 16;
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 32);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 4);
    EXPECT_EQ(decoded.simm, 16);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Load);
}

TEST_F(PPCEncodingTest, DecodeStoreInstruction) {
    u32 inst = (36 << 26) | (5 << 21) | (6 << 16) | 24;
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 36);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Store);
}

TEST_F(PPCEncodingTest, DecodeCompareInstruction) {
    u32 inst = (11 << 26) | (0 << 23) | (3 << 16) | ((-1) & 0xFFFF);
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 11);
    EXPECT_EQ(decoded.type, DecodedInst::Type::CompareLI);
}

//=============================================================================
// JIT Compiler Tests (ARM64 only)
//=============================================================================

#ifdef __aarch64__

class JitCompilerTest : public JitTest {
protected:
    void SetUp() override {
        JitTest::SetUp();
        
        jit_ = std::make_unique<JitCompiler>();
        ASSERT_EQ(jit_->initialize(memory_.get(), 4 * MB), Status::Ok);
        
        ctx_.reset();
        ctx_.running = true;
    }
    
    void TearDown() override {
        jit_->shutdown();
        JitTest::TearDown();
    }
    
    std::unique_ptr<JitCompiler> jit_;
    ThreadContext ctx_;
};

TEST_F(JitCompilerTest, Initialize) {
    auto stats = jit_->get_stats();
    EXPECT_EQ(stats.blocks_compiled, 0);
    EXPECT_EQ(stats.cache_hits, 0);
    EXPECT_EQ(stats.cache_misses, 0);
}

TEST_F(JitCompilerTest, CompileSimpleAddi) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 42));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    ctx_.gpr[0] = 0;
    
    jit_->execute(ctx_, 100);
    
    auto stats = jit_->get_stats();
    EXPECT_GE(stats.blocks_compiled, 1);
}

TEST_F(JitCompilerTest, CompileAddis) {
    write_ppc_inst(CODE_BASE, ppc_addis(3, 0, 0x1234));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[3], 0x12340000);
}

TEST_F(JitCompilerTest, CompileAdd) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 100));
    write_ppc_inst(CODE_BASE + 4, ppc_addi(4, 0, 50));
    write_ppc_inst(CODE_BASE + 8, ppc_add(5, 3, 4));
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[3], 100);
    EXPECT_EQ(ctx_.gpr[4], 50);
    EXPECT_EQ(ctx_.gpr[5], 150);
}

TEST_F(JitCompilerTest, CompileSubf) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 100));
    write_ppc_inst(CODE_BASE + 4, ppc_addi(4, 0, 30));
    write_ppc_inst(CODE_BASE + 8, ppc_subf(5, 4, 3));  // r5 = r3 - r4
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[5], 70);
}

TEST_F(JitCompilerTest, CompileMullw) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 7));
    write_ppc_inst(CODE_BASE + 4, ppc_addi(4, 0, 6));
    write_ppc_inst(CODE_BASE + 8, ppc_mullw(5, 3, 4));
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[5], 42);
}

TEST_F(JitCompilerTest, CompileDivw) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 100));
    write_ppc_inst(CODE_BASE + 4, ppc_addi(4, 0, 7));
    write_ppc_inst(CODE_BASE + 8, ppc_divw(5, 3, 4));
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[5], 14);
}

TEST_F(JitCompilerTest, CompileLogical) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 0x0F));
    write_ppc_inst(CODE_BASE + 4, ppc_addi(4, 0, 0xF0));
    write_ppc_inst(CODE_BASE + 8, ppc_and(5, 3, 4));
    write_ppc_inst(CODE_BASE + 12, ppc_or(6, 3, 4));
    write_ppc_inst(CODE_BASE + 16, ppc_xor(7, 3, 4));
    write_ppc_inst(CODE_BASE + 20, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[5], 0x00);   // 0x0F & 0xF0 = 0
    EXPECT_EQ(ctx_.gpr[6], 0xFF);   // 0x0F | 0xF0 = 0xFF
    EXPECT_EQ(ctx_.gpr[7], 0xFF);   // 0x0F ^ 0xF0 = 0xFF
}

TEST_F(JitCompilerTest, CompileShifts) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 1));
    write_ppc_inst(CODE_BASE + 4, ppc_addi(4, 0, 4));
    write_ppc_inst(CODE_BASE + 8, ppc_slw(5, 3, 4));  // 1 << 4 = 16
    write_ppc_inst(CODE_BASE + 12, ppc_addi(3, 0, 64));
    write_ppc_inst(CODE_BASE + 16, ppc_addi(4, 0, 3));
    write_ppc_inst(CODE_BASE + 20, ppc_srw(6, 3, 4)); // 64 >> 3 = 8
    write_ppc_inst(CODE_BASE + 24, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[5], 16);
    EXPECT_EQ(ctx_.gpr[6], 8);
}

TEST_F(JitCompilerTest, CompileRotate) {
    // rlwinm r4, r3, 4, 0, 27  - rotate left 4 and mask
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 0x0F));
    write_ppc_inst(CODE_BASE + 4, ppc_rlwinm(4, 3, 4, 0, 27));
    write_ppc_inst(CODE_BASE + 8, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[4], 0xF0);
}

TEST_F(JitCompilerTest, CompileCompare) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 10));
    write_ppc_inst(CODE_BASE + 4, ppc_cmpwi(0, 3, 5));
    write_ppc_inst(CODE_BASE + 8, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    // CR0: GT should be set (10 > 5)
    EXPECT_FALSE(ctx_.cr[0].lt);
    EXPECT_TRUE(ctx_.cr[0].gt);
    EXPECT_FALSE(ctx_.cr[0].eq);
}

TEST_F(JitCompilerTest, BlockCacheHit) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 1));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    jit_->execute(ctx_, 100);
    
    ctx_.pc = CODE_BASE;
    ctx_.running = true;
    ctx_.interrupted = false;
    
    jit_->execute(ctx_, 100);
    
    auto stats = jit_->get_stats();
    EXPECT_GE(stats.cache_hits, 1);
}

TEST_F(JitCompilerTest, InvalidateOnWrite) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 1));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    jit_->execute(ctx_, 100);
    
    auto stats1 = jit_->get_stats();
    u64 blocks_before = stats1.blocks_compiled;
    
    jit_->invalidate(CODE_BASE, 8);
    
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 2));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    ctx_.running = true;
    ctx_.interrupted = false;
    
    jit_->execute(ctx_, 100);
    
    auto stats2 = jit_->get_stats();
    EXPECT_GT(stats2.blocks_compiled, blocks_before);
}

TEST_F(JitCompilerTest, FlushCache) {
    for (int i = 0; i < 5; i++) {
        GuestAddr addr = CODE_BASE + i * 8;
        write_ppc_inst(addr, ppc_addi(3, 0, i));
        write_ppc_inst(addr + 4, ppc_blr());
        
        ctx_.pc = addr;
        ctx_.running = true;
        ctx_.interrupted = false;
        jit_->execute(ctx_, 100);
    }
    
    auto stats1 = jit_->get_stats();
    EXPECT_GE(stats1.blocks_compiled, 5);
    
    jit_->flush_cache();
    
    auto stats2 = jit_->get_stats();
    EXPECT_EQ(stats2.blocks_compiled, 0);
}

TEST_F(JitCompilerTest, CompileMtspr) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 0x1234));
    write_ppc_inst(CODE_BASE + 4, ppc_mtspr(8, 3));  // mtlr r3
    write_ppc_inst(CODE_BASE + 8, ppc_mfspr(4, 8));  // mflr r4
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.lr, 0x1234);
    EXPECT_EQ(ctx_.gpr[4], 0x1234);
}

TEST_F(JitCompilerTest, CompileMtctr) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 0x5678));
    write_ppc_inst(CODE_BASE + 4, ppc_mtspr(9, 3));  // mtctr r3
    write_ppc_inst(CODE_BASE + 8, ppc_mfspr(4, 9));  // mfctr r4
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.ctr, 0x5678);
    EXPECT_EQ(ctx_.gpr[4], 0x5678);
}

TEST_F(JitCompilerTest, MultipleBlocks) {
    // First block
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 10));
    write_ppc_inst(CODE_BASE + 4, ppc_b(8));  // Jump to second block
    
    // Second block
    write_ppc_inst(CODE_BASE + 12, ppc_addi(4, 0, 20));
    write_ppc_inst(CODE_BASE + 16, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 200);
    
    EXPECT_EQ(ctx_.gpr[3], 10);
    EXPECT_EQ(ctx_.gpr[4], 20);
    
    auto stats = jit_->get_stats();
    EXPECT_GE(stats.blocks_compiled, 2);
}

TEST_F(JitCompilerTest, CompileNegativeImmediate) {
    // Test addi with negative immediate
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, -100));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(static_cast<s64>(ctx_.gpr[3]), -100);
}

TEST_F(JitCompilerTest, CompileOri) {
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 0));
    write_ppc_inst(CODE_BASE + 4, ppc_ori(3, 3, 0xABCD));
    write_ppc_inst(CODE_BASE + 8, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[3], 0xABCD);
}

TEST_F(JitCompilerTest, CompileLwzStw) {
    // Write test value to memory
    memory_->write_u32(DATA_BASE, 0x12345678);
    
    // Load from memory
    write_ppc_inst(CODE_BASE, ppc_addis(4, 0, DATA_BASE >> 16));
    write_ppc_inst(CODE_BASE + 4, ppc_ori(4, 4, DATA_BASE & 0xFFFF));
    write_ppc_inst(CODE_BASE + 8, ppc_lwz(3, 4, 0));
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[3], 0x12345678);
}

TEST_F(JitCompilerTest, CompileLbz) {
    memory_->write_u8(DATA_BASE, 0xAB);
    
    write_ppc_inst(CODE_BASE, ppc_addis(4, 0, DATA_BASE >> 16));
    write_ppc_inst(CODE_BASE + 4, ppc_ori(4, 4, DATA_BASE & 0xFFFF));
    write_ppc_inst(CODE_BASE + 8, ppc_lbz(3, 4, 0));
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[3], 0xAB);
}

TEST_F(JitCompilerTest, CompileConditionalBranch) {
    // Setup: r3 = 10, compare with 5
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 10));
    write_ppc_inst(CODE_BASE + 4, ppc_cmpwi(0, 3, 5));
    // bc 12,1,8 - branch if GT (cr0.gt = 1)
    write_ppc_inst(CODE_BASE + 8, ppc_bc(12, 1, 8));
    write_ppc_inst(CODE_BASE + 12, ppc_addi(4, 0, 100)); // Not taken
    write_ppc_inst(CODE_BASE + 16, ppc_addi(4, 0, 200)); // Taken
    write_ppc_inst(CODE_BASE + 20, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    // Branch should be taken (10 > 5), so r4 = 200
    EXPECT_EQ(ctx_.gpr[4], 200);
}

TEST_F(JitCompilerTest, CompileLoop) {
    // Simple loop: count from 0 to 5
    // r3 = counter, r4 = limit
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 0));      // r3 = 0 (counter)
    write_ppc_inst(CODE_BASE + 4, ppc_addi(4, 0, 5));  // r4 = 5 (limit)
    
    // Loop:
    write_ppc_inst(CODE_BASE + 8, ppc_addi(3, 3, 1));  // r3++
    write_ppc_inst(CODE_BASE + 12, ppc_cmpwi(0, 3, 5)); // compare r3 with 5
    write_ppc_inst(CODE_BASE + 16, ppc_bc(4, 2, -8));  // bne loop (branch if not equal)
    write_ppc_inst(CODE_BASE + 20, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 1000);
    
    EXPECT_EQ(ctx_.gpr[3], 5);
}

TEST_F(JitCompilerTest, CompileNop) {
    write_ppc_inst(CODE_BASE, ppc_nop());
    write_ppc_inst(CODE_BASE + 4, ppc_nop());
    write_ppc_inst(CODE_BASE + 8, ppc_addi(3, 0, 42));
    write_ppc_inst(CODE_BASE + 12, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    jit_->execute(ctx_, 100);
    
    EXPECT_EQ(ctx_.gpr[3], 42);
}

TEST_F(JitCompilerTest, ExecutionPerformance) {
    // Measure JIT execution performance with a simple loop
    // This is a basic sanity check that JIT is faster than interpreter
    
    // Count to 10000
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 0));       // r3 = 0
    write_ppc_inst(CODE_BASE + 4, ppc_addi(3, 3, 1));   // r3++
    write_ppc_inst(CODE_BASE + 8, ppc_cmpwi(0, 3, 100)); // compare
    write_ppc_inst(CODE_BASE + 12, ppc_bc(4, 2, -8));   // loop
    write_ppc_inst(CODE_BASE + 16, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    auto start = std::chrono::high_resolution_clock::now();
    jit_->execute(ctx_, 100000);
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    EXPECT_EQ(ctx_.gpr[3], 100);
    
    // Just verify it completes - actual performance depends on hardware
    EXPECT_LT(duration.count(), 1000000); // Should complete in under 1 second
}

#endif // __aarch64__

//=============================================================================
// Register Allocator Tests
//=============================================================================

TEST(RegisterAllocatorTest, Initialize) {
    RegisterAllocator alloc;
    
    // All registers should not be cached initially
    for (int i = 0; i < 32; i++) {
        EXPECT_FALSE(alloc.is_cached(i));
    }
}

TEST(RegisterAllocatorTest, AllocTemp) {
    RegisterAllocator alloc;
    
    int r1 = alloc.alloc_temp();
    int r2 = alloc.alloc_temp();
    int r3 = alloc.alloc_temp();
    
    EXPECT_NE(r1, r2);
    EXPECT_NE(r2, r3);
    EXPECT_NE(r1, r3);
}

TEST(RegisterAllocatorTest, FreeTemp) {
    RegisterAllocator alloc;
    
    int r1 = alloc.alloc_temp();
    alloc.free_temp(r1);
    int r2 = alloc.alloc_temp();
    
    EXPECT_EQ(r1, r2);  // Should reuse freed register
}

TEST(RegisterAllocatorTest, Reset) {
    RegisterAllocator alloc;
    
    alloc.alloc_temp();
    alloc.alloc_temp();
    
    alloc.reset();
    
    for (int i = 0; i < 32; i++) {
        EXPECT_FALSE(alloc.is_cached(i));
    }
}

} // namespace test
} // namespace x360mu

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
