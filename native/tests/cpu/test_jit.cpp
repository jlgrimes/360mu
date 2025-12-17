/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * JIT Compiler Tests
 */

#include <gtest/gtest.h>
#include "../../src/cpu/jit/jit.h"
#include "../../src/memory/memory.h"
#include "../../src/cpu/xenon/cpu.h"

namespace x360mu {
namespace test {

class JitTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
        
        // Allocate code region
        ASSERT_EQ(memory_->allocate(CODE_BASE, CODE_SIZE, 
            MemoryRegion::Read | MemoryRegion::Write | MemoryRegion::Execute), 
            Status::Ok);
    }
    
    void TearDown() override {
        memory_->shutdown();
    }
    
    // Write a PPC instruction to memory (big-endian)
    void write_ppc_inst(GuestAddr addr, u32 inst) {
        memory_->write_u32(addr, inst);
    }
    
    // Helper to assemble simple PPC instructions
    static u32 ppc_addi(int rd, int ra, s16 simm) {
        return (14 << 26) | (rd << 21) | (ra << 16) | (simm & 0xFFFF);
    }
    
    static u32 ppc_addis(int rd, int ra, s16 simm) {
        return (15 << 26) | (rd << 21) | (ra << 16) | (simm & 0xFFFF);
    }
    
    static u32 ppc_add(int rd, int ra, int rb) {
        return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266 << 1);
    }
    
    static u32 ppc_lwz(int rd, int ra, s16 offset) {
        return (32 << 26) | (rd << 21) | (ra << 16) | (offset & 0xFFFF);
    }
    
    static u32 ppc_stw(int rs, int ra, s16 offset) {
        return (36 << 26) | (rs << 21) | (ra << 16) | (offset & 0xFFFF);
    }
    
    static u32 ppc_b(s32 offset, bool link = false, bool absolute = false) {
        return (18 << 26) | (offset & 0x03FFFFFC) | (absolute ? 2 : 0) | (link ? 1 : 0);
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
    
    std::unique_ptr<Memory> memory_;
    
    static constexpr GuestAddr CODE_BASE = 0x82000000;
    static constexpr u64 CODE_SIZE = 64 * 1024; // 64KB
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
    
    std::vector<u8> buffer_;
    std::unique_ptr<ARM64Emitter> emit_;
};

TEST_F(ARM64EmitterTest, EmitAddImmediate) {
    emit_->ADD_imm(0, 1, 42);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    
    // ADD X0, X1, #42
    // Expected: 0x91000A20
    EXPECT_EQ(inst & 0xFF000000, 0x91000000); // ADD imm opcode
    EXPECT_EQ((inst >> 5) & 0x1F, 1);  // Rn = X1
    EXPECT_EQ(inst & 0x1F, 0);          // Rd = X0
    EXPECT_EQ((inst >> 10) & 0xFFF, 42); // imm12 = 42
}

TEST_F(ARM64EmitterTest, EmitSubImmediate) {
    emit_->SUB_imm(2, 3, 100);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    
    EXPECT_EQ(inst & 0xFF000000, 0xD1000000); // SUB imm opcode
}

TEST_F(ARM64EmitterTest, EmitMovImm_Zero) {
    emit_->MOV_imm(5, 0);
    
    // MOV X5, XZR should emit ORR X5, XZR, XZR
    ASSERT_EQ(emit_->size(), 4);
}

TEST_F(ARM64EmitterTest, EmitMovImm_Small) {
    emit_->MOV_imm(0, 0x1234);
    
    // Should use MOVZ
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    EXPECT_EQ(inst & 0xFF800000, 0xD2800000); // MOVZ opcode
}

TEST_F(ARM64EmitterTest, EmitMovImm_Large) {
    emit_->MOV_imm(0, 0x123456789ABCDEF0ULL);
    
    // Should use MOVZ + up to 3 MOVKs
    ASSERT_GE(emit_->size(), 4);
    ASSERT_LE(emit_->size(), 16);
}

TEST_F(ARM64EmitterTest, EmitAddReg) {
    emit_->ADD(0, 1, 2);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    
    // ADD X0, X1, X2
    EXPECT_EQ(inst & 0xFF200000, 0x8B000000); // ADD reg opcode
    EXPECT_EQ(inst & 0x1F, 0);          // Rd = X0
    EXPECT_EQ((inst >> 5) & 0x1F, 1);   // Rn = X1
    EXPECT_EQ((inst >> 16) & 0x1F, 2);  // Rm = X2
}

TEST_F(ARM64EmitterTest, EmitLogicalReg) {
    emit_->AND(0, 1, 2);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    EXPECT_EQ(inst & 0xFF200000, 0x8A000000); // AND reg opcode
    
    emit_->ORR(3, 4, 5);
    inst = *reinterpret_cast<u32*>(buffer_.data() + 4);
    EXPECT_EQ(inst & 0xFF200000, 0xAA000000); // ORR reg opcode
}

TEST_F(ARM64EmitterTest, EmitLoadStore) {
    emit_->LDR(0, 1, 8);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    EXPECT_EQ(inst & 0xFFC00000, 0xF9400000); // LDR unsigned offset
}

TEST_F(ARM64EmitterTest, EmitBranch) {
    emit_->B(64);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    EXPECT_EQ(inst & 0xFC000000, 0x14000000); // B opcode
    
    // Offset is 64 bytes = 16 instructions
    EXPECT_EQ(inst & 0x03FFFFFF, 16);
}

TEST_F(ARM64EmitterTest, EmitConditionalBranch) {
    emit_->B_cond(arm64_cond::EQ, 32);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    EXPECT_EQ(inst & 0xFF000010, 0x54000000); // B.cond opcode
    EXPECT_EQ(inst & 0x0F, arm64_cond::EQ);   // condition
}

TEST_F(ARM64EmitterTest, EmitBranchLink) {
    emit_->BL(128);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    EXPECT_EQ(inst & 0xFC000000, 0x94000000); // BL opcode
}

TEST_F(ARM64EmitterTest, EmitBranchReg) {
    emit_->BR(arm64::X30);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    EXPECT_EQ(inst & 0xFFFFFC00, 0xD61F0000); // BR opcode
}

TEST_F(ARM64EmitterTest, EmitNeon) {
    emit_->FADD_vec(0, 1, 2, false);
    
    ASSERT_EQ(emit_->size(), 4);
    u32 inst = *reinterpret_cast<u32*>(buffer_.data());
    EXPECT_EQ(inst & 0xFFA0FC00, 0x4E20D400); // FADD 4S opcode
}

TEST_F(ARM64EmitterTest, EmitMultiple) {
    emit_->ADD_imm(0, 1, 1);    // X0 = X1 + 1
    emit_->ADD_imm(0, 0, 2);    // X0 = X0 + 2
    emit_->RET();
    
    ASSERT_EQ(emit_->size(), 12);
}

//=============================================================================
// JIT Compiler Tests
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
    // Initialization is done in SetUp
    auto stats = jit_->get_stats();
    EXPECT_EQ(stats.blocks_compiled, 0);
    EXPECT_EQ(stats.cache_hits, 0);
    EXPECT_EQ(stats.cache_misses, 0);
}

TEST_F(JitCompilerTest, CompileSimpleBlock) {
    // Write a simple block: addi r3, r0, 42; blr
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 42));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    // Set up context
    ctx_.pc = CODE_BASE;
    ctx_.gpr[0] = 0;
    
    // Execute (limited cycles)
    jit_->execute(ctx_, 100);
    
    // Check that block was compiled
    auto stats = jit_->get_stats();
    EXPECT_GE(stats.blocks_compiled, 1);
}

TEST_F(JitCompilerTest, BlockCacheHit) {
    // Write a simple block
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 1));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    
    // Execute twice - second time should hit cache
    jit_->execute(ctx_, 100);
    
    ctx_.pc = CODE_BASE;
    ctx_.running = true;
    ctx_.interrupted = false;
    
    jit_->execute(ctx_, 100);
    
    auto stats = jit_->get_stats();
    EXPECT_GE(stats.cache_hits, 1);
}

TEST_F(JitCompilerTest, InvalidateOnWrite) {
    // Write initial code
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 1));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    jit_->execute(ctx_, 100);
    
    auto stats1 = jit_->get_stats();
    u64 blocks_before = stats1.blocks_compiled;
    
    // Invalidate (simulate self-modifying code)
    jit_->invalidate(CODE_BASE, 8);
    
    // Write new code
    write_ppc_inst(CODE_BASE, ppc_addi(3, 0, 2));
    write_ppc_inst(CODE_BASE + 4, ppc_blr());
    
    ctx_.pc = CODE_BASE;
    ctx_.running = true;
    ctx_.interrupted = false;
    
    jit_->execute(ctx_, 100);
    
    auto stats2 = jit_->get_stats();
    // Should have compiled a new block
    EXPECT_GT(stats2.blocks_compiled, blocks_before);
}

TEST_F(JitCompilerTest, FlushCache) {
    // Compile several blocks
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
    
    // Flush cache
    jit_->flush_cache();
    
    auto stats2 = jit_->get_stats();
    EXPECT_EQ(stats2.blocks_compiled, 0);
    EXPECT_EQ(stats2.code_bytes_used, 0);
}

#endif // __aarch64__

//=============================================================================
// PPC Instruction Encoding Tests (used by JIT)
//=============================================================================

class PPCEncodingTest : public ::testing::Test {};

TEST_F(PPCEncodingTest, EncodeAddi) {
    // addi r3, r0, 42
    u32 inst = (14 << 26) | (3 << 21) | (0 << 16) | (42 & 0xFFFF);
    
    EXPECT_EQ((inst >> 26) & 0x3F, 14);  // opcode
    EXPECT_EQ((inst >> 21) & 0x1F, 3);   // rD
    EXPECT_EQ((inst >> 16) & 0x1F, 0);   // rA
    EXPECT_EQ(inst & 0xFFFF, 42);        // SIMM
}

TEST_F(PPCEncodingTest, DecodeInstruction) {
    // Test decoder with a known instruction
    u32 inst = (14 << 26) | (3 << 21) | (1 << 16) | 100;
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 14);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 1);
    EXPECT_EQ(decoded.simm, 100);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Add);
}

TEST_F(PPCEncodingTest, DecodeAddInstruction) {
    // add r3, r4, r5 = opcode 31, rd=3, ra=4, rb=5, xo=266
    u32 inst = (31 << 26) | (3 << 21) | (4 << 16) | (5 << 11) | (266 << 1);
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 31);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 4);
    EXPECT_EQ(decoded.rb, 5);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Add);
}

TEST_F(PPCEncodingTest, DecodeBranchInstruction) {
    // b +16 = opcode 18, offset=16
    u32 inst = (18 << 26) | (16 & 0x03FFFFFC);
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 18);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Branch);
    EXPECT_EQ(decoded.li, 16);
}

TEST_F(PPCEncodingTest, DecodeLoadInstruction) {
    // lwz r3, 16(r4) = opcode 32, rd=3, ra=4, offset=16
    u32 inst = (32 << 26) | (3 << 21) | (4 << 16) | 16;
    
    DecodedInst decoded = Decoder::decode(inst);
    
    EXPECT_EQ(decoded.opcode, 32);
    EXPECT_EQ(decoded.rd, 3);
    EXPECT_EQ(decoded.ra, 4);
    EXPECT_EQ(decoded.simm, 16);
    EXPECT_EQ(decoded.type, DecodedInst::Type::Load);
}

} // namespace test
} // namespace x360mu

// Main entry point for tests
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
