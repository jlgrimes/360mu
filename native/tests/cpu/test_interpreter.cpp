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
    std::unique_ptr<ThreadContext> ctx;
    
    void SetUp() override {
        memory = std::make_unique<Memory>();
        memory->initialize();
        
        ctx = std::make_unique<ThreadContext>();
        memset(ctx.get(), 0, sizeof(ThreadContext));
    }
    
    void TearDown() override {
        memory->shutdown();
    }
};

TEST_F(InterpreterTest, AddImmediate) {
    // addi r3, r0, 42
    u32 inst = 0x3860002A;
    
    Interpreter interp(memory.get(), ctx.get());
    interp.execute_one(inst);
    
    EXPECT_EQ(ctx->gpr[3], 42);
}

TEST_F(InterpreterTest, AddImmediateWithBase) {
    // addi r3, r4, 10
    ctx->gpr[4] = 100;
    u32 inst = 0x3864000A;
    
    Interpreter interp(memory.get(), ctx.get());
    interp.execute_one(inst);
    
    EXPECT_EQ(ctx->gpr[3], 110);
}

TEST_F(InterpreterTest, LoadWord) {
    // Write test value to memory
    memory->write_u32(0x82000100, 0xDEADBEEF);
    
    // lwz r3, 0(r4)
    ctx->gpr[4] = 0x82000100;
    u32 inst = 0x80640000;
    
    Interpreter interp(memory.get(), ctx.get());
    interp.execute_one(inst);
    
    EXPECT_EQ(ctx->gpr[3], 0xDEADBEEF);
}

TEST_F(InterpreterTest, StoreWord) {
    ctx->gpr[3] = 0xCAFEBABE;
    ctx->gpr[4] = 0x82000200;
    
    // stw r3, 0(r4)
    u32 inst = 0x90640000;
    
    Interpreter interp(memory.get(), ctx.get());
    interp.execute_one(inst);
    
    EXPECT_EQ(memory->read_u32(0x82000200), 0xCAFEBABE);
}

TEST_F(InterpreterTest, Branch) {
    ctx->pc = 0x82000000;
    
    // b +0x100
    u32 inst = 0x48000100;
    
    Interpreter interp(memory.get(), ctx.get());
    interp.execute_one(inst);
    
    EXPECT_EQ(ctx->pc, 0x82000100);
}

TEST_F(InterpreterTest, BranchLink) {
    ctx->pc = 0x82000000;
    
    // bl +0x200
    u32 inst = 0x48000201;
    
    Interpreter interp(memory.get(), ctx.get());
    interp.execute_one(inst);
    
    EXPECT_EQ(ctx->pc, 0x82000200);
    EXPECT_EQ(ctx->lr, 0x82000004);
}

} // namespace test
} // namespace x360mu

