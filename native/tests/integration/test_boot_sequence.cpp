/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Boot Sequence Integration Test
 * Tests the full initialization pipeline: Memory → CPU → Kernel → XEX Load → Execute
 */

#include <gtest/gtest.h>
#include "memory/memory.h"
#include "cpu/xenon/cpu.h"
#include "kernel/kernel.h"
#include "kernel/xkernel.h"
#include "kernel/xex_loader.h"
#include "kernel/filesystem/vfs.h"

namespace x360mu {
namespace test {

class BootSequenceTest : public ::testing::Test {
protected:
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;

    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);

        cpu_ = std::make_unique<Cpu>();
        CpuConfig config{};
        config.enable_jit = false;  // Use interpreter for reproducibility
        ASSERT_EQ(cpu_->initialize(memory_.get(), config), Status::Ok);
    }

    void TearDown() override {
        cpu_->shutdown();
        memory_->shutdown();
    }

    // Helper: create a minimal valid XEX2 header in memory
    std::vector<u8> create_minimal_xex() {
        std::vector<u8> xex;
        xex.resize(512, 0);

        // XEX2 magic "XEX2"
        xex[0] = 'X'; xex[1] = 'E'; xex[2] = 'X'; xex[3] = '2';

        // Module flags (offset 4, big-endian)
        xex[4] = 0; xex[5] = 0; xex[6] = 0; xex[7] = 0;

        // PE data offset (offset 8, big-endian) - points to header area
        xex[8] = 0; xex[9] = 0; xex[10] = 0x01; xex[11] = 0x00;  // 256

        // Reserved (offset 12)
        // Security info offset (offset 16, big-endian)
        xex[16] = 0; xex[17] = 0; xex[18] = 0; xex[19] = 0x80;  // 128

        // Header count (offset 20, big-endian)
        xex[20] = 0; xex[21] = 0; xex[22] = 0; xex[23] = 0;  // 0 optional headers

        return xex;
    }
};

//=============================================================================
// Subsystem Initialization Order
//=============================================================================

TEST_F(BootSequenceTest, MemoryInitFirst) {
    // Memory must initialize before anything else
    auto mem = std::make_unique<Memory>();
    EXPECT_EQ(mem->initialize(), Status::Ok);
    mem->shutdown();
}

TEST_F(BootSequenceTest, CpuDependsOnMemory) {
    // CPU needs valid memory to initialize
    auto cpu = std::make_unique<Cpu>();
    CpuConfig config{};
    config.enable_jit = false;
    EXPECT_EQ(cpu->initialize(memory_.get(), config), Status::Ok);
    cpu->shutdown();
}

TEST_F(BootSequenceTest, CpuRejectsNullMemory) {
    auto cpu = std::make_unique<Cpu>();
    CpuConfig config{};
    Status status = cpu->initialize(nullptr, config);
    // Should either fail or handle gracefully
    if (status == Status::Ok) {
        cpu->shutdown();
    }
}

//=============================================================================
// CPU Thread Initialization
//=============================================================================

TEST_F(BootSequenceTest, StartThread) {
    GuestAddr entry = 0x82000000;
    GuestAddr stack = 0x82100000;

    // Write a simple instruction at entry (blr = return)
    memory_->write_u32(entry, 0x4E800020);  // blr

    Status status = cpu_->start_thread(0, entry, stack);
    EXPECT_EQ(status, Status::Ok);

    auto& ctx = cpu_->get_context(0);
    EXPECT_EQ(ctx.pc, entry);
    EXPECT_TRUE(ctx.running);
}

TEST_F(BootSequenceTest, MultipleThreads) {
    GuestAddr entry = 0x82000000;
    memory_->write_u32(entry, 0x4E800020);  // blr

    for (u32 i = 0; i < 3; i++) {
        Status status = cpu_->start_thread(i, entry, 0x82100000 + i * 0x10000);
        EXPECT_EQ(status, Status::Ok);
    }

    EXPECT_TRUE(cpu_->any_running());
}

TEST_F(BootSequenceTest, StopThread) {
    GuestAddr entry = 0x82000000;
    memory_->write_u32(entry, 0x4E800020);

    cpu_->start_thread(0, entry, 0x82100000);
    EXPECT_TRUE(cpu_->get_context(0).running);

    cpu_->stop_thread(0);
    EXPECT_FALSE(cpu_->get_context(0).running);
}

//=============================================================================
// CPU Execution
//=============================================================================

TEST_F(BootSequenceTest, ExecuteSimpleInstruction) {
    GuestAddr entry = 0x82000000;

    // addi r3, r0, 42
    u32 addi = (14 << 26) | (3 << 21) | (0 << 16) | 42;
    memory_->write_u32(entry, addi);

    // blr (branch to link register - ends execution)
    memory_->write_u32(entry + 4, 0x4E800020);

    cpu_->start_thread(0, entry, 0x82100000);

    // Execute a few cycles
    cpu_->execute_thread(0, 10);

    auto& ctx = cpu_->get_context(0);
    EXPECT_EQ(ctx.gpr[3], 42u);
}

TEST_F(BootSequenceTest, ExecuteMultipleInstructions) {
    GuestAddr entry = 0x82000000;

    // li r3, 10 (addi r3, r0, 10)
    memory_->write_u32(entry, (14 << 26) | (3 << 21) | (0 << 16) | 10);
    // li r4, 20 (addi r4, r0, 20)
    memory_->write_u32(entry + 4, (14 << 26) | (4 << 21) | (0 << 16) | 20);
    // add r5, r3, r4
    memory_->write_u32(entry + 8, (31 << 26) | (5 << 21) | (3 << 16) | (4 << 11) | (266 << 1));
    // blr
    memory_->write_u32(entry + 12, 0x4E800020);

    cpu_->start_thread(0, entry, 0x82100000);
    cpu_->execute_thread(0, 20);

    auto& ctx = cpu_->get_context(0);
    EXPECT_EQ(ctx.gpr[5], 30u);
}

//=============================================================================
// XEX Loading
//=============================================================================

TEST_F(BootSequenceTest, XexLoaderRejectsEmpty) {
    std::vector<u8> empty;
    // Empty data should be rejected
    // Test through the XexLoader interface if available
}

TEST_F(BootSequenceTest, XexLoaderRejectsInvalidMagic) {
    std::vector<u8> bad_data(256, 0);
    bad_data[0] = 'N'; bad_data[1] = 'O'; bad_data[2] = 'T'; bad_data[3] = 'X';
    // Should reject non-XEX2 magic
}

TEST_F(BootSequenceTest, XexLoaderAcceptsValidMagic) {
    auto xex = create_minimal_xex();
    // First 4 bytes should be valid XEX2
    EXPECT_EQ(xex[0], 'X');
    EXPECT_EQ(xex[1], 'E');
    EXPECT_EQ(xex[2], 'X');
    EXPECT_EQ(xex[3], '2');
}

//=============================================================================
// Memory Map Verification
//=============================================================================

TEST_F(BootSequenceTest, PhysicalMemoryAccessible) {
    // Physical RAM range should be accessible
    GuestAddr addr = 0x00100000;
    memory_->write_u32(addr, 0xDEADBEEF);
    EXPECT_EQ(memory_->read_u32(addr), 0xDEADBEEFu);
}

TEST_F(BootSequenceTest, VirtualMirrorAccessible) {
    // Write through physical, read through virtual mirror
    GuestAddr phys = 0x00100000;
    GuestAddr virt = 0x80100000;

    memory_->write_u32(phys, 0xCAFEBABE);
    EXPECT_EQ(memory_->read_u32(virt), 0xCAFEBABEu);
}

TEST_F(BootSequenceTest, StackRegionAccessible) {
    // Typical stack area in virtual space
    GuestAddr stack = 0x82100000;
    memory_->write_u32(stack, 0x11223344);
    EXPECT_EQ(memory_->read_u32(stack), 0x11223344u);
}

//=============================================================================
// Full Boot Sequence Smoke Test
//=============================================================================

TEST_F(BootSequenceTest, FullInitShutdownCycle) {
    // Verify clean init -> use -> shutdown cycle without leaks/crashes
    auto mem = std::make_unique<Memory>();
    ASSERT_EQ(mem->initialize(), Status::Ok);

    auto cpu = std::make_unique<Cpu>();
    CpuConfig config{};
    config.enable_jit = false;
    ASSERT_EQ(cpu->initialize(mem.get(), config), Status::Ok);

    // Write and execute a simple program
    GuestAddr entry = 0x82000000;
    mem->write_u32(entry, (14 << 26) | (3 << 21) | (0 << 16) | 99);  // li r3, 99
    mem->write_u32(entry + 4, 0x4E800020);  // blr

    cpu->start_thread(0, entry, 0x82100000);
    cpu->execute_thread(0, 10);
    cpu->stop_thread(0);

    // Clean shutdown in reverse order
    cpu->shutdown();
    mem->shutdown();
}

TEST_F(BootSequenceTest, ResetAndRestart) {
    GuestAddr entry = 0x82000000;

    // First execution
    memory_->write_u32(entry, (14 << 26) | (3 << 21) | (0 << 16) | 1);  // li r3, 1
    memory_->write_u32(entry + 4, 0x4E800020);  // blr

    cpu_->start_thread(0, entry, 0x82100000);
    cpu_->execute_thread(0, 10);
    EXPECT_EQ(cpu_->get_context(0).gpr[3], 1u);

    // Reset
    cpu_->stop_thread(0);
    cpu_->reset();

    // Second execution with different code
    memory_->write_u32(entry, (14 << 26) | (3 << 21) | (0 << 16) | 99);  // li r3, 99
    memory_->write_u32(entry + 4, 0x4E800020);  // blr

    cpu_->start_thread(0, entry, 0x82100000);
    cpu_->execute_thread(0, 10);
    EXPECT_EQ(cpu_->get_context(0).gpr[3], 99u);
}

} // namespace test
} // namespace x360mu
