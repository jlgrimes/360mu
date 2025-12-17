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
    }
    
    void TearDown() override {
        memory->shutdown();
    }
};

TEST_F(InterpreterTest, BasicOperation) {
    // Just test that we can create and use the interpreter
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

} // namespace test
} // namespace x360mu
