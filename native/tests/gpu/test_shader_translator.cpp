/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Shader Translator Unit Tests
 * Tests for Xenos shader to SPIR-V translation
 */

#include <gtest/gtest.h>
#include "../../src/gpu/xenos/shader_translator.h"
#include <cstring>

using namespace x360mu;

// Simple test vertex shader microcode (minimal shader that sets position)
// This is a simplified representation - real Xenos shaders are more complex
static const u32 simple_vertex_shader[] = {
    // Control flow: EXEC_END with 1 ALU instruction
    0x00000102,  // CF: EXEC_END, addr=0, count=1
    0x00000000,
    0x00000000,
    
    // ALU instruction: MOV r0, c0 (move constant 0 to temp 0)
    0x00000000,  // src regs
    0x00000000,  // dest=r0, vector_op=ADDv, scalar_op=0
    0x00000000,
};

// Simple pixel shader that outputs a solid color
static const u32 simple_pixel_shader[] = {
    // Control flow: EXEC_END with 1 ALU instruction
    0x00000102,
    0x00000000,
    0x00000000,
    
    // ALU instruction: MOV r0, c0
    0x00000000,
    0x00000000,
    0x00000000,
};

class ShaderTranslatorTest : public ::testing::Test {
protected:
    ShaderTranslator translator;
    
    void SetUp() override {
        ASSERT_EQ(translator.initialize(""), Status::Ok);
    }
    
    void TearDown() override {
        translator.shutdown();
    }
};

// Test: Translator initialization
TEST_F(ShaderTranslatorTest, Initialization) {
    // Setup already done in SetUp()
    // If we got here, initialization succeeded
    SUCCEED();
}

// Test: Simple vertex shader translation
TEST_F(ShaderTranslatorTest, SimpleVertexShaderTranslation) {
    std::vector<u32> spirv = translator.translate(
        simple_vertex_shader, 
        sizeof(simple_vertex_shader), 
        ShaderType::Vertex
    );
    
    // Should produce valid SPIR-V (non-empty)
    ASSERT_GT(spirv.size(), 5u);
    
    // Check SPIR-V magic number
    EXPECT_EQ(spirv[0], 0x07230203u);
}

// Test: Simple pixel shader translation
TEST_F(ShaderTranslatorTest, SimplePixelShaderTranslation) {
    std::vector<u32> spirv = translator.translate(
        simple_pixel_shader, 
        sizeof(simple_pixel_shader), 
        ShaderType::Pixel
    );
    
    // Should produce valid SPIR-V (non-empty)
    ASSERT_GT(spirv.size(), 5u);
    
    // Check SPIR-V magic number
    EXPECT_EQ(spirv[0], 0x07230203u);
}

// Test: Shader caching
TEST_F(ShaderTranslatorTest, ShaderCaching) {
    // First translation
    std::vector<u32> spirv1 = translator.translate(
        simple_vertex_shader, 
        sizeof(simple_vertex_shader), 
        ShaderType::Vertex
    );
    
    // Second translation of same shader should hit cache
    std::vector<u32> spirv2 = translator.translate(
        simple_vertex_shader, 
        sizeof(simple_vertex_shader), 
        ShaderType::Vertex
    );
    
    // Results should be identical
    ASSERT_EQ(spirv1.size(), spirv2.size());
    EXPECT_EQ(memcmp(spirv1.data(), spirv2.data(), spirv1.size() * sizeof(u32)), 0);
    
    // Check stats
    auto stats = translator.get_stats();
    EXPECT_GE(stats.cache_hits, 1u);
}

// Test: Shader analysis
TEST_F(ShaderTranslatorTest, ShaderAnalysis) {
    ShaderInfo info = translator.analyze(
        simple_vertex_shader,
        sizeof(simple_vertex_shader),
        ShaderType::Vertex
    );
    
    EXPECT_EQ(info.type, ShaderType::Vertex);
    // Simple shader should have minimal resource usage
    EXPECT_LE(info.temp_register_count, 128u);
    EXPECT_LE(info.max_const_register, 256u);
}

// Test: Hash computation consistency
TEST_F(ShaderTranslatorTest, HashComputationConsistency) {
    // Same data should produce same hash
    u64 hash1 = ShaderTranslator::compute_hash(simple_vertex_shader, sizeof(simple_vertex_shader));
    u64 hash2 = ShaderTranslator::compute_hash(simple_vertex_shader, sizeof(simple_vertex_shader));
    EXPECT_EQ(hash1, hash2);
    
    // Different data should produce different hash
    u64 hash3 = ShaderTranslator::compute_hash(simple_pixel_shader, sizeof(simple_pixel_shader));
    EXPECT_NE(hash1, hash3);
}

// Test: SPIR-V Builder basic operations
TEST(SpirvBuilderTest, BasicTypes) {
    SpirvBuilder builder;
    builder.begin(ShaderType::Vertex);
    
    u32 void_type = builder.type_void();
    u32 bool_type = builder.type_bool();
    u32 int_type = builder.type_int(32, true);
    u32 uint_type = builder.type_int(32, false);
    u32 float_type = builder.type_float(32);
    u32 vec4_type = builder.type_vector(float_type, 4);
    
    // Types should have unique IDs
    EXPECT_NE(void_type, bool_type);
    EXPECT_NE(bool_type, int_type);
    EXPECT_NE(int_type, uint_type);
    EXPECT_NE(uint_type, float_type);
    EXPECT_NE(float_type, vec4_type);
    
    // Same type requested twice should return same ID (caching)
    u32 float_type2 = builder.type_float(32);
    EXPECT_EQ(float_type, float_type2);
    
    std::vector<u32> spirv = builder.end();
    ASSERT_GT(spirv.size(), 5u);
    EXPECT_EQ(spirv[0], 0x07230203u);
}

// Test: SPIR-V Builder constants
TEST(SpirvBuilderTest, Constants) {
    SpirvBuilder builder;
    builder.begin(ShaderType::Vertex);
    
    u32 const_true = builder.const_bool(true);
    u32 const_false = builder.const_bool(false);
    u32 const_zero = builder.const_float(0.0f);
    u32 const_one = builder.const_float(1.0f);
    u32 const_int = builder.const_int(42);
    u32 const_uint = builder.const_uint(100);
    
    // All should have unique IDs
    EXPECT_NE(const_true, const_false);
    EXPECT_NE(const_zero, const_one);
    EXPECT_NE(const_int, const_uint);
    
    std::vector<u32> spirv = builder.end();
    EXPECT_EQ(spirv[0], 0x07230203u);
}

// Test: Empty shader handling
TEST_F(ShaderTranslatorTest, EmptyShaderHandling) {
    // Too small shader should fail gracefully
    u32 tiny_shader[] = { 0x00000000 };
    std::vector<u32> spirv = translator.translate(
        tiny_shader, 
        4, 
        ShaderType::Vertex
    );
    
    // Should return empty vector on failure
    EXPECT_TRUE(spirv.empty());
}

// Test: Null pointer handling
TEST_F(ShaderTranslatorTest, NullPointerHandling) {
    std::vector<u32> spirv = translator.translate(
        nullptr, 
        0, 
        ShaderType::Vertex
    );
    
    EXPECT_TRUE(spirv.empty());
}

// Test: Statistics tracking
TEST_F(ShaderTranslatorTest, StatisticsTracking) {
    auto stats_before = translator.get_stats();
    
    translator.translate(
        simple_vertex_shader, 
        sizeof(simple_vertex_shader), 
        ShaderType::Vertex
    );
    
    auto stats_after = translator.get_stats();
    
    EXPECT_GT(stats_after.shaders_translated, stats_before.shaders_translated);
    EXPECT_GT(stats_after.total_microcode_size, stats_before.total_microcode_size);
}

// Test: Cache clear
TEST_F(ShaderTranslatorTest, CacheClear) {
    // Translate a shader to populate cache
    translator.translate(
        simple_vertex_shader, 
        sizeof(simple_vertex_shader), 
        ShaderType::Vertex
    );
    
    // Clear cache
    translator.clear_cache();
    
    // Next translation should be a cache miss
    auto stats_before = translator.get_stats();
    
    translator.translate(
        simple_vertex_shader, 
        sizeof(simple_vertex_shader), 
        ShaderType::Vertex
    );
    
    auto stats_after = translator.get_stats();
    EXPECT_GT(stats_after.cache_misses, stats_before.cache_misses);
}

// Test: Multiple shader types don't interfere
TEST_F(ShaderTranslatorTest, MultipleShaderTypes) {
    std::vector<u32> vs_spirv = translator.translate(
        simple_vertex_shader, 
        sizeof(simple_vertex_shader), 
        ShaderType::Vertex
    );
    
    std::vector<u32> ps_spirv = translator.translate(
        simple_pixel_shader, 
        sizeof(simple_pixel_shader), 
        ShaderType::Pixel
    );
    
    // Both should produce valid SPIR-V
    ASSERT_GT(vs_spirv.size(), 5u);
    ASSERT_GT(ps_spirv.size(), 5u);
    
    // Both should have valid SPIR-V magic
    EXPECT_EQ(vs_spirv[0], 0x07230203u);
    EXPECT_EQ(ps_spirv[0], 0x07230203u);
}

// Test: Shader microcode parsing
TEST(ShaderMicrocodeTest, BasicParsing) {
    ShaderMicrocode microcode;
    
    Status status = microcode.parse(
        simple_vertex_shader,
        sizeof(simple_vertex_shader),
        ShaderType::Vertex
    );
    
    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(microcode.type(), ShaderType::Vertex);
}

// Test: Invalid microcode handling
TEST(ShaderMicrocodeTest, InvalidMicrocode) {
    ShaderMicrocode microcode;
    
    // Null pointer
    Status status = microcode.parse(nullptr, 0, ShaderType::Vertex);
    EXPECT_NE(status, Status::Ok);
    
    // Too small
    u8 tiny[4] = {0};
    status = microcode.parse(tiny, 4, ShaderType::Vertex);
    EXPECT_NE(status, Status::Ok);
}
