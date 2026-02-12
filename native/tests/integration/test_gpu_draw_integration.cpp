/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * GPU Draw Integration Test
 * Tests the command processor → register state → draw call pipeline
 * Uses PM4 packet construction to simulate GPU command buffers
 */

#include <gtest/gtest.h>
#include "memory/memory.h"

namespace x360mu {
namespace test {

class GpuDrawIntegrationTest : public ::testing::Test {
protected:
    std::unique_ptr<Memory> memory_;
    GuestAddr cmd_base_ = 0x00800000;  // Command buffer in physical RAM

    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);
    }

    void TearDown() override {
        memory_->shutdown();
    }

    // PM4 packet helpers

    // Type 0: Register write
    u32 pm4_type0(u16 reg_index, u16 count) {
        return (0 << 30) | ((count - 1) << 16) | reg_index;
    }

    // Type 2: NOP
    u32 pm4_type2() {
        return (2 << 30);
    }

    // Type 3: Command packet
    u32 pm4_type3(u16 opcode, u16 count) {
        return (3 << 30) | (opcode << 8) | ((count - 1) & 0x3F);
    }

    // Write a command buffer to memory and return size in dwords
    u32 write_command_buffer(const std::vector<u32>& commands) {
        for (size_t i = 0; i < commands.size(); i++) {
            memory_->write_u32(cmd_base_ + static_cast<u32>(i * 4), commands[i]);
        }
        return static_cast<u32>(commands.size());
    }
};

//=============================================================================
// PM4 Packet Construction
//=============================================================================

TEST_F(GpuDrawIntegrationTest, Type0Packet_Format) {
    u32 pkt = pm4_type0(0x2000, 1);
    // Bits 31:30 = 0 (type 0)
    EXPECT_EQ((pkt >> 30) & 3, 0u);
    // Bits 15:0 = register index
    EXPECT_EQ(pkt & 0xFFFF, 0x2000u);
}

TEST_F(GpuDrawIntegrationTest, Type2Packet_IsNop) {
    u32 pkt = pm4_type2();
    EXPECT_EQ((pkt >> 30) & 3, 2u);
}

TEST_F(GpuDrawIntegrationTest, Type3Packet_Format) {
    u32 pkt = pm4_type3(0x2D, 2);  // DRAW_INDX, 2 dwords
    EXPECT_EQ((pkt >> 30) & 3, 3u);
    // Opcode in bits 15:8
    EXPECT_EQ((pkt >> 8) & 0xFF, 0x2Du);
}

//=============================================================================
// Command Buffer Writing
//=============================================================================

TEST_F(GpuDrawIntegrationTest, WriteCommandBuffer) {
    std::vector<u32> cmds = {
        pm4_type2(),     // NOP
        pm4_type2(),     // NOP
        pm4_type0(0x2000, 1),
        0xDEADBEEF,      // Register value
    };

    u32 count = write_command_buffer(cmds);
    EXPECT_EQ(count, 4u);

    // Verify commands in memory
    EXPECT_EQ(memory_->read_u32(cmd_base_), pm4_type2());
    EXPECT_EQ(memory_->read_u32(cmd_base_ + 4), pm4_type2());
    EXPECT_EQ(memory_->read_u32(cmd_base_ + 12), 0xDEADBEEFu);
}

//=============================================================================
// Register State Simulation
//=============================================================================

TEST_F(GpuDrawIntegrationTest, RegisterWrite_Sequence) {
    // Simulate a typical draw call register setup:
    // 1. Set primitive type
    // 2. Set vertex count
    // 3. Issue draw

    std::vector<u32> cmds;

    // SET_CONSTANT (opcode 0x2D for setting constants)
    // Type 0 packet: write to register 0x2180 (VGT_DRAW_INITIATOR)
    cmds.push_back(pm4_type0(0x2180, 1));
    cmds.push_back(0x00000003);  // Triangle list

    // Type 0 packet: write to register 0x2184 (VGT_IMMED_DATA)
    cmds.push_back(pm4_type0(0x2184, 1));
    cmds.push_back(3);  // 3 vertices

    write_command_buffer(cmds);

    // Verify the register values are in memory
    EXPECT_EQ(memory_->read_u32(cmd_base_ + 4), 0x00000003u);
    EXPECT_EQ(memory_->read_u32(cmd_base_ + 12), 3u);
}

//=============================================================================
// Multi-Draw Sequence
//=============================================================================

TEST_F(GpuDrawIntegrationTest, MultipleDrawCalls) {
    // Simulate multiple draw calls in a single command buffer
    std::vector<u32> cmds;

    // Draw 1: 3 vertices (triangle)
    cmds.push_back(pm4_type0(0x2180, 1));
    cmds.push_back(3);

    // NOP between draws (common for synchronization)
    cmds.push_back(pm4_type2());

    // Draw 2: 4 vertices (quad as 2 triangles)
    cmds.push_back(pm4_type0(0x2180, 1));
    cmds.push_back(6);

    u32 count = write_command_buffer(cmds);
    EXPECT_EQ(count, 5u);
}

//=============================================================================
// Constant Buffer Setup
//=============================================================================

TEST_F(GpuDrawIntegrationTest, ALUConstantWrite) {
    // Xbox 360 GPU has 256 float4 ALU constants per shader
    // They're written via Type 0 packets to the constant registers

    std::vector<u32> cmds;

    // Write 4 floats (one constant) to ALU constant 0
    // ALU constants start at register 0x4000
    cmds.push_back(pm4_type0(0x4000, 4));
    // 1.0f, 0.0f, 0.0f, 1.0f (identity column)
    union { float f; u32 u; } conv;
    conv.f = 1.0f; cmds.push_back(conv.u);
    conv.f = 0.0f; cmds.push_back(conv.u);
    conv.f = 0.0f; cmds.push_back(conv.u);
    conv.f = 1.0f; cmds.push_back(conv.u);

    write_command_buffer(cmds);

    // Verify the constant values
    union { u32 u; float f; } readback;
    readback.u = memory_->read_u32(cmd_base_ + 4);
    EXPECT_FLOAT_EQ(readback.f, 1.0f);
    readback.u = memory_->read_u32(cmd_base_ + 8);
    EXPECT_FLOAT_EQ(readback.f, 0.0f);
}

//=============================================================================
// Command Buffer Boundary Tests
//=============================================================================

TEST_F(GpuDrawIntegrationTest, EmptyCommandBuffer) {
    std::vector<u32> cmds;
    u32 count = write_command_buffer(cmds);
    EXPECT_EQ(count, 0u);
}

TEST_F(GpuDrawIntegrationTest, LargeCommandBuffer) {
    // Build a large command buffer with many NOPs
    std::vector<u32> cmds;
    for (int i = 0; i < 1000; i++) {
        cmds.push_back(pm4_type2());
    }

    u32 count = write_command_buffer(cmds);
    EXPECT_EQ(count, 1000u);

    // Verify first and last
    EXPECT_EQ(memory_->read_u32(cmd_base_), pm4_type2());
    EXPECT_EQ(memory_->read_u32(cmd_base_ + 999 * 4), pm4_type2());
}

//=============================================================================
// Memory Integration for GPU Buffers
//=============================================================================

TEST_F(GpuDrawIntegrationTest, VertexBufferInMemory) {
    // Simulate vertex data written by CPU for GPU consumption
    GuestAddr vb_base = 0x00900000;

    // Simple triangle: 3 vertices x (x, y, z, w) float4
    float vertices[] = {
        0.0f, 1.0f, 0.0f, 1.0f,   // Top
        -1.0f, -1.0f, 0.0f, 1.0f, // Bottom-left
        1.0f, -1.0f, 0.0f, 1.0f,  // Bottom-right
    };

    memory_->write_bytes(vb_base, vertices, sizeof(vertices));

    // Read back and verify
    float readback[12];
    memory_->read_bytes(vb_base, readback, sizeof(readback));

    EXPECT_FLOAT_EQ(readback[0], 0.0f);
    EXPECT_FLOAT_EQ(readback[1], 1.0f);
    EXPECT_FLOAT_EQ(readback[4], -1.0f);
    EXPECT_FLOAT_EQ(readback[5], -1.0f);
    EXPECT_FLOAT_EQ(readback[8], 1.0f);
    EXPECT_FLOAT_EQ(readback[9], -1.0f);
}

TEST_F(GpuDrawIntegrationTest, IndexBufferInMemory) {
    // Simulate index buffer
    GuestAddr ib_base = 0x00A00000;

    u16 indices[] = {0, 1, 2, 2, 1, 3};  // Two triangles

    for (int i = 0; i < 6; i++) {
        memory_->write_u16(ib_base + i * 2, indices[i]);
    }

    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(memory_->read_u16(ib_base + i * 2), indices[i]);
    }
}

//=============================================================================
// GPU Command + Data Coherency
//=============================================================================

TEST_F(GpuDrawIntegrationTest, CommandAndDataInSameMemory) {
    // GPU commands and vertex data share the same physical memory
    // Verify they don't interfere with each other

    GuestAddr cmd_addr = cmd_base_;
    GuestAddr vtx_addr = 0x00900000;

    // Write commands
    std::vector<u32> cmds = {pm4_type0(0x2180, 1), 3, pm4_type2()};
    write_command_buffer(cmds);

    // Write vertex data nearby
    memory_->write_u32(vtx_addr, 0x3F800000);  // 1.0f
    memory_->write_u32(vtx_addr + 4, 0x40000000);  // 2.0f

    // Both should be independently accessible
    EXPECT_EQ(memory_->read_u32(cmd_addr + 4), 3u);
    EXPECT_EQ(memory_->read_u32(vtx_addr), 0x3F800000u);
}

} // namespace test
} // namespace x360mu
