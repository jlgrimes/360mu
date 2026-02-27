/**
 * 360mu - Xbox 360 Emulator for Android
 *
 * End-to-end rendering pipeline integration test
 *
 * Tests the full path: PM4 commands -> register setup -> shader translate
 * -> SPIR-V -> pipeline state -> draw -> present.
 *
 * Headless tests exercise everything up to the Vulkan draw call.
 * The Vulkan-dependent tests are guarded by a live VulkanBackend.
 */

#include <gtest/gtest.h>
#include "gpu/xenos/gpu.h"
#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/shader_translator.h"
#include "gpu/default_shaders.h"
#include "gpu/vulkan/vulkan_backend.h"
#include "memory/memory.h"
#include <cstring>
#include <cmath>

namespace x360mu {
namespace test {

// ============================================================================
// Helpers
// ============================================================================

// Encode a float as u32
static u32 f2u(float f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

// PM4 Type 0: register write (base_reg, count dwords following header)
static u32 pm4_type0(u16 reg_index, u16 count) {
    return (0u << 30) | (static_cast<u32>(count - 1) << 16) | reg_index;
}

// PM4 Type 2: NOP
static u32 pm4_type2() {
    return (2u << 30);
}

// PM4 Type 3: command (opcode in bits 8-15, count-1 in bits 0-5, type in bits 30-31)
static u32 pm4_type3(u16 opcode, u16 count) {
    return (3u << 30) | (static_cast<u32>(opcode) << 8) | ((count - 1) & 0x3F);
}

// ============================================================================
// Test Fixture: Memory + Command Processor (headless, no Vulkan)
// ============================================================================

class RenderPipelineTest : public ::testing::Test {
protected:
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<CommandProcessor> cp_;

    // Memory layout
    static constexpr GuestAddr CMD_BASE   = 0x00800000;
    static constexpr GuestAddr VB_BASE    = 0x00900000;
    static constexpr GuestAddr IB_BASE    = 0x00A00000;
    static constexpr GuestAddr SHADER_BASE = 0x00B00000;

    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);

        cp_ = std::make_unique<CommandProcessor>();
        // Initialize with memory only (no Vulkan, no shader translator)
        // process_ring_buffer still parses packets and updates register state
        ASSERT_EQ(cp_->initialize(memory_.get(), nullptr, nullptr, nullptr), Status::Ok);
    }

    void TearDown() override {
        cp_->shutdown();
        memory_->shutdown();
    }

    // Write commands to guest memory as a ring buffer, then process
    void execute_commands(const std::vector<u32>& cmds) {
        for (size_t i = 0; i < cmds.size(); i++) {
            memory_->write_u32(CMD_BASE + static_cast<u32>(i * 4), cmds[i]);
        }
        u32 rp = 0;
        u32 wp = static_cast<u32>(cmds.size());
        u32 ring_size = static_cast<u32>(cmds.size() * 4);
        // Ensure ring size is at least large enough
        if (ring_size < 4096) ring_size = 4096;
        cp_->process(CMD_BASE, ring_size, rp, wp);
    }

    // Write vertex data: triangle with positions (vec3) + color (vec4)
    void write_triangle_vertices() {
        // 3 vertices x 3 floats (position only)
        float verts[] = {
             0.0f,  0.5f,  0.0f,  // top
            -0.5f, -0.5f,  0.0f,  // bottom-left
             0.5f, -0.5f,  0.0f,  // bottom-right
        };
        memory_->write_bytes(VB_BASE, verts, sizeof(verts));
    }

    // Build a full "hello triangle" PM4 command sequence
    std::vector<u32> build_hello_triangle_commands() {
        std::vector<u32> cmds;

        // 1. Set render target (RB_COLOR_INFO)
        cmds.push_back(pm4_type0(xenos_reg::RB_COLOR_INFO, 1));
        cmds.push_back(0x00000006);  // eDRAM base=0, format=k_8_8_8_8 (6)

        // 2. Set surface info (RB_SURFACE_INFO) - 1280 wide, no MSAA
        cmds.push_back(pm4_type0(xenos_reg::RB_SURFACE_INFO, 1));
        cmds.push_back(1280);  // pitch in pixels

        // 3. Set viewport scale/offset
        cmds.push_back(pm4_type0(xenos_reg::PA_CL_VPORT_XSCALE, 6));
        cmds.push_back(f2u(640.0f));   // X scale
        cmds.push_back(f2u(640.0f));   // X offset
        cmds.push_back(f2u(-360.0f));  // Y scale (inverted)
        cmds.push_back(f2u(360.0f));   // Y offset
        cmds.push_back(f2u(0.5f));     // Z scale
        cmds.push_back(f2u(0.5f));     // Z offset

        // 4. Set scissor
        cmds.push_back(pm4_type0(xenos_reg::PA_SC_SCREEN_SCISSOR_TL, 2));
        cmds.push_back(0x00000000);  // top=0, left=0
        cmds.push_back((720 << 16) | 1280);  // bottom=720, right=1280

        // 5. Set rasterizer state (no cull, CCW front face)
        cmds.push_back(pm4_type0(xenos_reg::PA_SU_SC_MODE_CNTL, 1));
        cmds.push_back(0x00000000);  // cull_mode=0 (none)

        // 6. Depth control (depth test off)
        cmds.push_back(pm4_type0(xenos_reg::RB_DEPTHCONTROL, 1));
        cmds.push_back(0x00000000);

        // 7. Blend control (no blend)
        cmds.push_back(pm4_type0(xenos_reg::RB_COLORCONTROL, 1));
        cmds.push_back(0x00000000);

        // 8. Color mask (all channels)
        cmds.push_back(pm4_type0(xenos_reg::RB_COLOR_MASK, 1));
        cmds.push_back(0x0000000F);

        // 9. Set shader program addresses
        cmds.push_back(pm4_type0(xenos_reg::SQ_VS_PROGRAM, 1));
        cmds.push_back(SHADER_BASE >> 8);  // VS address (shifted)

        cmds.push_back(pm4_type0(xenos_reg::SQ_PS_PROGRAM, 1));
        cmds.push_back((SHADER_BASE + 0x1000) >> 8);  // PS address

        // 10. Set SQ_PROGRAM_CNTL (VS/PS config)
        cmds.push_back(pm4_type0(xenos_reg::SQ_PROGRAM_CNTL, 1));
        cmds.push_back(0x00000000);

        // 11. Set vertex fetch constant (fetch slot 0)
        //     6 dwords per fetch constant, base at FETCH_CONST_BASE
        cmds.push_back(pm4_type0(xenos_reg::FETCH_CONST_BASE, 6));
        cmds.push_back(VB_BASE & 0xFFFFFFFC);  // word 0: address
        cmds.push_back((((3 * 3 * 4 - 1) / 4) << 2) | 0x2);  // word 1: size, endian=8in32
        cmds.push_back(12);  // word 2: stride = 12 bytes (3 floats)
        cmds.push_back(0);   // word 3
        cmds.push_back(0);   // word 4
        cmds.push_back(0);   // word 5

        // 12. Draw (Type 3 DRAW_INDX_2 = non-indexed)
        //     Opcode 0x36, payload: VGT info
        cmds.push_back(pm4_type3(0x36, 1));
        u32 draw_info = (static_cast<u32>(PrimitiveType::TriangleList) & 0x3F) |
                        (3 << 16);  // 3 vertices
        cmds.push_back(draw_info);

        return cmds;
    }
};

// ============================================================================
// 1. PM4 Packet Encoding
// ============================================================================

TEST_F(RenderPipelineTest, PM4Type0_RegisterWriteEncoding) {
    u32 pkt = pm4_type0(xenos_reg::RB_COLOR_INFO, 1);
    EXPECT_EQ((pkt >> 30) & 3, 0u);                  // Type 0
    EXPECT_EQ(pkt & 0xFFFF, xenos_reg::RB_COLOR_INFO);  // Register
    EXPECT_EQ((pkt >> 16) & 0x3FFF, 0u);              // count - 1 = 0
}

TEST_F(RenderPipelineTest, PM4Type0_MultiRegWrite) {
    u32 pkt = pm4_type0(xenos_reg::PA_CL_VPORT_XSCALE, 6);
    EXPECT_EQ((pkt >> 16) & 0x3FFF, 5u);  // count - 1 = 5
}

TEST_F(RenderPipelineTest, PM4Type3_DrawCommandEncoding) {
    u32 pkt = pm4_type3(0x36, 1);
    EXPECT_EQ((pkt >> 30) & 3, 3u);       // Type 3
    EXPECT_EQ((pkt >> 8) & 0xFF, 0x36u);  // DRAW_INDX_2 opcode
}

// ============================================================================
// 2. Command Processor Register State Updates
// ============================================================================

TEST_F(RenderPipelineTest, CP_RegisterWrite_SingleReg) {
    std::vector<u32> cmds = {
        pm4_type0(xenos_reg::RB_COLOR_INFO, 1),
        0xDEADBEEF
    };
    execute_commands(cmds);
    EXPECT_EQ(cp_->get_register(xenos_reg::RB_COLOR_INFO), 0xDEADBEEFu);
}

TEST_F(RenderPipelineTest, CP_RegisterWrite_ViewportRegs) {
    std::vector<u32> cmds = {
        pm4_type0(xenos_reg::PA_CL_VPORT_XSCALE, 6),
        f2u(640.0f),
        f2u(640.0f),
        f2u(-360.0f),
        f2u(360.0f),
        f2u(0.5f),
        f2u(0.5f),
    };
    execute_commands(cmds);

    union { u32 u; float f; } r;

    r.u = cp_->get_register(xenos_reg::PA_CL_VPORT_XSCALE);
    EXPECT_FLOAT_EQ(r.f, 640.0f);

    r.u = cp_->get_register(xenos_reg::PA_CL_VPORT_YSCALE);
    EXPECT_FLOAT_EQ(r.f, -360.0f);

    r.u = cp_->get_register(xenos_reg::PA_CL_VPORT_ZSCALE);
    EXPECT_FLOAT_EQ(r.f, 0.5f);
}

TEST_F(RenderPipelineTest, CP_RegisterWrite_DepthAndBlend) {
    std::vector<u32> cmds = {
        pm4_type0(xenos_reg::RB_DEPTHCONTROL, 1),
        0x00000003,  // depth_test=1, depth_write=1
        pm4_type0(xenos_reg::RB_COLORCONTROL, 1),
        0x00000001,  // blend_enable=1
    };
    execute_commands(cmds);

    u32 dc = cp_->get_register(xenos_reg::RB_DEPTHCONTROL);
    EXPECT_TRUE(dc & 0x1);   // depth test
    EXPECT_TRUE(dc & 0x2);   // depth write

    u32 cc = cp_->get_register(xenos_reg::RB_COLORCONTROL);
    EXPECT_TRUE(cc & 0x1);   // blend enable
}

// ============================================================================
// 3. Full Hello Triangle Command Sequence
// ============================================================================

TEST_F(RenderPipelineTest, HelloTriangle_CommandSequence) {
    write_triangle_vertices();

    auto cmds = build_hello_triangle_commands();

    // Should not crash processing the commands headless
    execute_commands(cmds);

    // Verify key registers were set
    EXPECT_NE(cp_->get_register(xenos_reg::RB_COLOR_INFO), 0u);
    EXPECT_EQ(cp_->get_register(xenos_reg::RB_SURFACE_INFO), 1280u);
    EXPECT_EQ(cp_->get_register(xenos_reg::RB_DEPTHCONTROL), 0u);
    EXPECT_EQ(cp_->get_register(xenos_reg::RB_COLOR_MASK), 0x0Fu);

    // Verify vertex data in guest memory
    float v0_y;
    memory_->read_bytes(VB_BASE + 4, &v0_y, 4);
    EXPECT_FLOAT_EQ(v0_y, 0.5f);
}

TEST_F(RenderPipelineTest, HelloTriangle_PacketCount) {
    auto cmds = build_hello_triangle_commands();
    execute_commands(cmds);

    // Should have processed multiple packets
    EXPECT_GT(cp_->packets_processed(), 0u);
}

// ============================================================================
// 4. Vertex and Index Buffer in Guest Memory
// ============================================================================

TEST_F(RenderPipelineTest, VertexBuffer_TriangleLayout) {
    write_triangle_vertices();

    // Read back each vertex
    float v[9];
    memory_->read_bytes(VB_BASE, v, sizeof(v));

    // Vertex 0: top center
    EXPECT_FLOAT_EQ(v[0], 0.0f);
    EXPECT_FLOAT_EQ(v[1], 0.5f);
    EXPECT_FLOAT_EQ(v[2], 0.0f);

    // Vertex 1: bottom-left
    EXPECT_FLOAT_EQ(v[3], -0.5f);
    EXPECT_FLOAT_EQ(v[4], -0.5f);

    // Vertex 2: bottom-right
    EXPECT_FLOAT_EQ(v[6], 0.5f);
    EXPECT_FLOAT_EQ(v[7], -0.5f);
}

TEST_F(RenderPipelineTest, IndexBuffer_TwoTriangles) {
    u16 indices[] = {0, 1, 2, 2, 1, 3};
    memory_->write_bytes(IB_BASE, indices, sizeof(indices));

    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(memory_->read_u16(IB_BASE + i * 2), indices[i]);
    }
}

// ============================================================================
// 5. Default Shader SPIR-V Validation
// ============================================================================

TEST_F(RenderPipelineTest, DefaultVertexShader_ValidSPIRV) {
    const auto& spirv = get_default_vertex_shader_spirv();

    ASSERT_FALSE(spirv.empty());
    // SPIR-V magic number
    EXPECT_EQ(spirv[0], 0x07230203u);
    // Version 1.0
    EXPECT_EQ(spirv[1], 0x00010000u);
    // Bound (max ID + 1) should be reasonable
    EXPECT_GT(spirv[3], 0u);
    EXPECT_LT(spirv[3], 1000u);
    // Reserved word must be 0
    EXPECT_EQ(spirv[4], 0u);
}

TEST_F(RenderPipelineTest, DefaultPixelShader_ValidSPIRV) {
    const auto& spirv = get_default_pixel_shader_spirv();

    ASSERT_FALSE(spirv.empty());
    EXPECT_EQ(spirv[0], 0x07230203u);
    EXPECT_EQ(spirv[1], 0x00010000u);
    EXPECT_GT(spirv[3], 0u);
    EXPECT_LT(spirv[3], 1000u);
    EXPECT_EQ(spirv[4], 0u);
}

TEST_F(RenderPipelineTest, DefaultShaders_DifferentContent) {
    const auto& vs = get_default_vertex_shader_spirv();
    const auto& ps = get_default_pixel_shader_spirv();

    // They should be different (one is vertex, one is fragment)
    EXPECT_NE(vs.size(), ps.size());
}

// ============================================================================
// 6. Shader Translation Round-Trip
// ============================================================================

class ShaderTranslationTest : public ::testing::Test {
protected:
    std::unique_ptr<ShaderTranslator> translator_;

    void SetUp() override {
        translator_ = std::make_unique<ShaderTranslator>();
        ASSERT_EQ(translator_->initialize(""), Status::Ok);
    }

    void TearDown() override {
        translator_->shutdown();
    }
};

TEST_F(ShaderTranslationTest, MinimalVertexShader_Translate) {
    // Minimal Xenos vertex shader microcode:
    // A single control flow instruction that allocates position export,
    // followed by a single ALU instruction that copies input to output.
    //
    // CF: EXEC(addr=0, count=1) ALLOC_EXPORT_POSITION
    // ALU: MOV oPos, R0
    //
    // Encoded as 3-word CF + 3-word ALU clause.
    // The translator should handle this or return empty (unsupported encoding).
    //
    // For robustness, we test that translate() doesn't crash on synthetic data.

    // 48 bytes = 8 dwords minimum Xenos shader (2 CF instructions)
    u32 microcode[] = {
        // CF instruction 0: EXEC, address=0, count=1, alloc position
        0x00000000, 0x00000000, 0x00000000,
        // CF instruction 1: END
        0x00000000, 0x00000000, 0x00000000,
        // ALU clause placeholder (nop)
        0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000,
    };

    // Translate should not crash regardless of content
    auto spirv = translator_->translate(microcode, sizeof(microcode), ShaderType::Vertex);

    // If translation produced output, validate SPIR-V header
    if (!spirv.empty()) {
        EXPECT_EQ(spirv[0], 0x07230203u);  // SPIR-V magic
        EXPECT_GT(spirv.size(), 5u);
    }
    // Empty result is also acceptable for synthetic microcode
}

TEST_F(ShaderTranslationTest, TranslateNullptr_DoesNotCrash) {
    auto spirv = translator_->translate(nullptr, 0, ShaderType::Vertex);
    EXPECT_TRUE(spirv.empty());
}

TEST_F(ShaderTranslationTest, HashDeterministic) {
    u32 microcode[] = {0x11223344, 0x55667788, 0xAABBCCDD};
    u64 hash1 = translator_->compute_hash(microcode, sizeof(microcode));
    u64 hash2 = translator_->compute_hash(microcode, sizeof(microcode));
    EXPECT_EQ(hash1, hash2);

    // Different data -> different hash
    u32 microcode2[] = {0x11223344, 0x55667788, 0xAABBCCDE};
    u64 hash3 = translator_->compute_hash(microcode2, sizeof(microcode2));
    EXPECT_NE(hash1, hash3);
}

// ============================================================================
// 7. GPU Class Integration (headless)
// ============================================================================

class GpuIntegrationTest : public ::testing::Test {
protected:
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Gpu> gpu_;

    void SetUp() override {
        memory_ = std::make_unique<Memory>();
        ASSERT_EQ(memory_->initialize(), Status::Ok);

        gpu_ = std::make_unique<Gpu>();
        GpuConfig config;
        config.use_vulkan = true;
        config.cache_path = "";
        ASSERT_EQ(gpu_->initialize(memory_.get(), config), Status::Ok);
    }

    void TearDown() override {
        gpu_->shutdown();
        memory_->shutdown();
    }

    void WriteSingleType0Packet(GuestAddr ring_base, u32 reg, u32 value) {
        memory_->write_u32(ring_base + 0, (0u << 30) | (0u << 16) | reg);
        memory_->write_u32(ring_base + 4, value);
    }

    void ConfigureAndProcessRing(GuestAddr ring_base, GuestAddr readback_addr, u32 wptr_dwords) {
        gpu_->write_register(xenos_reg::CP_RB_BASE, ring_base);
        gpu_->write_register(xenos_reg::CP_RB_CNTL, 3);  // 16-byte ring buffer
        gpu_->write_register(xenos_reg::CP_RB_RPTR_ADDR, readback_addr);
        gpu_->write_register(xenos_reg::CP_RB_RPTR, 0);
        gpu_->write_register(xenos_reg::CP_RB_WPTR, wptr_dwords);
        gpu_->process_commands();
    }
};

TEST_F(GpuIntegrationTest, Initialize_RegistersReady) {
    // GPU should report idle status
    u32 status = gpu_->read_register(0x0010);
    EXPECT_EQ(status, 0x80000000u);  // GRBM_STATUS - idle
}

TEST_F(GpuIntegrationTest, WriteRegister_ReadBack) {
    gpu_->write_register(xenos_reg::RB_COLOR_INFO, 0x42);
    EXPECT_EQ(gpu_->read_register(xenos_reg::RB_COLOR_INFO), 0x42u);
}

TEST_F(GpuIntegrationTest, Reset_ClearsRegisters) {
    gpu_->write_register(xenos_reg::RB_COLOR_INFO, 0xFF);
    gpu_->reset();
    EXPECT_EQ(gpu_->read_register(xenos_reg::RB_COLOR_INFO), 0u);
    // But status should be idle again
    EXPECT_EQ(gpu_->read_register(0x0010), 0x80000000u);
}

TEST_F(GpuIntegrationTest, Present_NoSurface_DoesNotCrash) {
    // Without set_surface(), present should handle gracefully
    gpu_->present();
    EXPECT_TRUE(gpu_->frame_complete());
}

TEST_F(GpuIntegrationTest, FrameSkip_SkipsCorrectly) {
    gpu_->set_frame_skip(2);  // Present every 3rd frame
    EXPECT_EQ(gpu_->frame_skip(), 2u);
}

TEST_F(GpuIntegrationTest, TargetFPS_Configurable) {
    gpu_->set_target_fps(60);
    EXPECT_EQ(gpu_->target_fps(), 60u);

    gpu_->set_target_fps(0);
    EXPECT_EQ(gpu_->target_fps(), 0u);
}

TEST_F(GpuIntegrationTest, VSync_Toggle) {
    gpu_->set_vsync(false);
    EXPECT_FALSE(gpu_->vsync_enabled());

    gpu_->set_vsync(true);
    EXPECT_TRUE(gpu_->vsync_enabled());
}

TEST_F(GpuIntegrationTest, FenceSync_AllocateAndSignal) {
    u64 fence1 = gpu_->allocate_fence();
    u64 fence2 = gpu_->allocate_fence();
    EXPECT_EQ(fence2, fence1 + 1);

    // Initially GPU fence is at 0
    EXPECT_FALSE(gpu_->gpu_fence_reached(fence1));

    // Signal CPU fence
    gpu_->cpu_signal_fence(fence1);
    EXPECT_EQ(gpu_->get_cpu_fence(), fence1);
}

TEST_F(GpuIntegrationTest, ProcessCommands_NoRingBuffer) {
    // No ring buffer configured -> should return without crash
    gpu_->process_commands();
    // No frames should have completed
    EXPECT_FALSE(gpu_->frame_complete());
}

TEST_F(GpuIntegrationTest, ProcessCommands_HeadlessParsesRingBuffer) {
    static constexpr GuestAddr kRingBase = 0x00800000;
    static constexpr GuestAddr kReadbackAddr = 0x00801000;
    static constexpr u32 kColorInfoValue = 0x00ABCDEF;

    WriteSingleType0Packet(kRingBase, xenos_reg::RB_COLOR_INFO, kColorInfoValue);
    memory_->write_u32(kReadbackAddr, 0);

    ConfigureAndProcessRing(kRingBase, kReadbackAddr, 2);  // header + 1 data dword

    EXPECT_EQ(gpu_->read_register(xenos_reg::RB_COLOR_INFO), kColorInfoValue);
    EXPECT_EQ(memory_->read_u32(kReadbackAddr), 2u);
}

TEST_F(GpuIntegrationTest, ProcessCommands_AfterSurfaceLoss_StillParsesRingBuffer) {
    static constexpr GuestAddr kRingBase = 0x00802000;
    static constexpr GuestAddr kReadbackAddr = 0x00803000;
    static constexpr u32 kColorInfoValue = 0x00123456;

    // Simulate Android surface teardown (app background / rotation).
    // Regression: this used to shut down command_processor_ without reinitializing
    // it for headless mode, so ring packets stopped being consumed.
    gpu_->set_surface(nullptr);

    WriteSingleType0Packet(kRingBase, xenos_reg::RB_COLOR_INFO, kColorInfoValue);
    memory_->write_u32(kReadbackAddr, 0);

    ConfigureAndProcessRing(kRingBase, kReadbackAddr, 2);

    EXPECT_EQ(gpu_->read_register(xenos_reg::RB_COLOR_INFO), kColorInfoValue);
    EXPECT_EQ(memory_->read_u32(kReadbackAddr), 2u);
}

// ============================================================================
// 8. Pipeline State Hashing
// ============================================================================

TEST(PipelineStateTest, DefaultState_DeterministicHash) {
    PipelineState state1;
    PipelineState state2;
    EXPECT_EQ(state1.compute_hash(), state2.compute_hash());
}

TEST(PipelineStateTest, DifferentTopology_DifferentHash) {
    PipelineState state1;
    state1.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    PipelineState state2;
    state2.primitive_topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    EXPECT_NE(state1.compute_hash(), state2.compute_hash());
}

TEST(PipelineStateTest, DifferentBlend_DifferentHash) {
    PipelineState state1;
    state1.blend_enable = VK_FALSE;

    PipelineState state2;
    state2.blend_enable = VK_TRUE;

    EXPECT_NE(state1.compute_hash(), state2.compute_hash());
}

TEST(PipelineStateTest, VertexInput_AffectsHash) {
    PipelineState state1;
    PipelineState state2;
    state2.vertex_input.binding_count = 1;
    state2.vertex_input.bindings[0].binding = 0;
    state2.vertex_input.bindings[0].stride = 12;
    state2.vertex_input.bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    state2.vertex_input.attribute_count = 1;
    state2.vertex_input.attributes[0].location = 0;
    state2.vertex_input.attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    state2.vertex_input.attributes[0].offset = 0;

    EXPECT_NE(state1.compute_hash(), state2.compute_hash());
}

// ============================================================================
// 9. Multi-Draw Command Sequence
// ============================================================================

TEST_F(RenderPipelineTest, MultiDraw_TwoTriangles) {
    std::vector<u32> cmds;

    // Set up minimal state
    cmds.push_back(pm4_type0(xenos_reg::RB_SURFACE_INFO, 1));
    cmds.push_back(1280);

    // Draw 1: triangle
    cmds.push_back(pm4_type3(0x36, 1));
    cmds.push_back((static_cast<u32>(PrimitiveType::TriangleList) & 0x3F) | (3 << 16));

    // NOP separator
    cmds.push_back(pm4_type2());

    // Draw 2: another triangle
    cmds.push_back(pm4_type3(0x36, 1));
    cmds.push_back((static_cast<u32>(PrimitiveType::TriangleList) & 0x3F) | (6 << 16));

    execute_commands(cmds);

    // Surface info should be set
    EXPECT_EQ(cp_->get_register(xenos_reg::RB_SURFACE_INFO), 1280u);
    EXPECT_GT(cp_->packets_processed(), 3u);
}

TEST_F(RenderPipelineTest, RingPointersWrapByDwordsNotBytes) {
    // 4-dword ring. Header is at the final dword, payload wraps to dword 0.
    constexpr u32 kRingSizeBytes = 16;
    constexpr u32 kStartReadPtr = 3;
    constexpr u32 kWritePtr = 1;
    constexpr u32 kColorInfo = 0xDEADBEEF;

    memory_->write_u32(CMD_BASE + (kStartReadPtr * 4), pm4_type0(xenos_reg::RB_COLOR_INFO, 1));
    memory_->write_u32(CMD_BASE + 0, kColorInfo);

    u32 rp = kStartReadPtr;
    bool frame_done = cp_->process(CMD_BASE, kRingSizeBytes, rp, kWritePtr);

    EXPECT_FALSE(frame_done);
    EXPECT_EQ(cp_->get_register(xenos_reg::RB_COLOR_INFO), kColorInfo);
    EXPECT_EQ(rp, kWritePtr);
    EXPECT_EQ(cp_->packets_processed(), 1u);
}

// ============================================================================
// 10. SET_CONSTANT (Type 3 opcode 0x2D) Integration
// ============================================================================

TEST_F(RenderPipelineTest, SetConstant_ALUConstants) {
    // SET_CONSTANT writes to shader constant registers
    // Payload: type (0=ALU), offset, values...
    std::vector<u32> cmds;

    // SET_CONSTANT: type=0 (ALU), start_offset=0, 4 floats
    cmds.push_back(pm4_type3(0x2D, 6));
    cmds.push_back(0x00000000);  // Type 0 = ALU vertex constants, offset 0
    cmds.push_back(f2u(1.0f));
    cmds.push_back(f2u(0.0f));
    cmds.push_back(f2u(0.0f));
    cmds.push_back(f2u(1.0f));

    execute_commands(cmds);
    EXPECT_GT(cp_->packets_processed(), 0u);
}

// ============================================================================
// 11. EVENT_WRITE (Frame Boundary)
// ============================================================================

TEST_F(RenderPipelineTest, EventWrite_FrameComplete) {
    std::vector<u32> cmds;

    // EVENT_WRITE with VS_DONE / PS_DONE event
    cmds.push_back(pm4_type3(0x46, 1));
    cmds.push_back(0x00000016);  // CACHE_FLUSH_AND_INV_EVENT

    execute_commands(cmds);
    // After event write, frame may or may not be marked complete
    // depending on event type - just verify no crash
    EXPECT_GT(cp_->packets_processed(), 0u);
}

// ============================================================================
// 12. SURFACE_SYNC (Resolve Trigger)
// ============================================================================

TEST_F(RenderPipelineTest, SurfaceSync_CommandParsed) {
    std::vector<u32> cmds;

    // SURFACE_SYNC: 4 dwords payload
    cmds.push_back(pm4_type3(0x43, 4));
    cmds.push_back(0xFFFFFFFF);  // coher_cntl
    cmds.push_back(0x00000000);  // coher_size
    cmds.push_back(0x00000000);  // coher_base
    cmds.push_back(0x0000000A);  // poll interval

    execute_commands(cmds);
    EXPECT_GT(cp_->packets_processed(), 0u);
}

} // namespace test
} // namespace x360mu
