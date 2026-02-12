/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Pipeline Cache and Shader Cache Data Structure Tests
 * Tests PipelineKey hashing, equality, and cache statistics
 * These tests do NOT require a Vulkan runtime
 */

#include <gtest/gtest.h>
#include "gpu/shader_cache.h"
#include <unordered_set>

namespace x360mu {
namespace test {

//=============================================================================
// PipelineKey Tests
//=============================================================================

TEST(PipelineKeyTest, DefaultConstruction) {
    PipelineKey key{};
    EXPECT_EQ(key.vertex_shader_hash, 0u);
    EXPECT_EQ(key.pixel_shader_hash, 0u);
}

TEST(PipelineKeyTest, Equality_Same) {
    PipelineKey a{};
    a.vertex_shader_hash = 0x1234;
    a.pixel_shader_hash = 0x5678;
    a.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    a.cull_mode = VK_CULL_MODE_BACK_BIT;
    a.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    a.depth_test_enable = VK_TRUE;
    a.depth_write_enable = VK_TRUE;
    a.depth_compare_op = VK_COMPARE_OP_LESS;
    a.blend_enable = VK_FALSE;

    PipelineKey b = a;
    EXPECT_TRUE(a == b);
}

TEST(PipelineKeyTest, Inequality_DifferentShader) {
    PipelineKey a{};
    a.vertex_shader_hash = 0x1234;
    a.pixel_shader_hash = 0x5678;

    PipelineKey b = a;
    b.vertex_shader_hash = 0xAAAA;
    EXPECT_FALSE(a == b);
}

TEST(PipelineKeyTest, Inequality_DifferentTopology) {
    PipelineKey a{};
    a.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    PipelineKey b = a;
    b.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    EXPECT_FALSE(a == b);
}

TEST(PipelineKeyTest, Inequality_DifferentCullMode) {
    PipelineKey a{};
    a.cull_mode = VK_CULL_MODE_BACK_BIT;

    PipelineKey b = a;
    b.cull_mode = VK_CULL_MODE_FRONT_BIT;
    EXPECT_FALSE(a == b);
}

TEST(PipelineKeyTest, Inequality_DifferentDepthState) {
    PipelineKey a{};
    a.depth_test_enable = VK_TRUE;
    a.depth_compare_op = VK_COMPARE_OP_LESS;

    PipelineKey b = a;
    b.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    EXPECT_FALSE(a == b);
}

TEST(PipelineKeyTest, Inequality_DifferentBlendState) {
    PipelineKey a{};
    a.blend_enable = VK_FALSE;

    PipelineKey b = a;
    b.blend_enable = VK_TRUE;
    EXPECT_FALSE(a == b);
}

TEST(PipelineKeyTest, Hash_Deterministic) {
    PipelineKey key{};
    key.vertex_shader_hash = 0xDEADBEEF;
    key.pixel_shader_hash = 0xCAFEBABE;
    key.primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    u64 hash1 = key.compute_hash();
    u64 hash2 = key.compute_hash();
    EXPECT_EQ(hash1, hash2);
}

TEST(PipelineKeyTest, Hash_DifferentForDifferentKeys) {
    PipelineKey a{};
    a.vertex_shader_hash = 0x1111;
    a.pixel_shader_hash = 0x2222;

    PipelineKey b{};
    b.vertex_shader_hash = 0x3333;
    b.pixel_shader_hash = 0x4444;

    EXPECT_NE(a.compute_hash(), b.compute_hash());
}

TEST(PipelineKeyTest, Hash_UniqueAcrossVariations) {
    std::unordered_set<u64> hashes;

    // Generate keys with different topologies
    VkPrimitiveTopology topologies[] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
    };

    for (auto topo : topologies) {
        PipelineKey key{};
        key.vertex_shader_hash = 0xAAAA;
        key.pixel_shader_hash = 0xBBBB;
        key.primitive_topology = topo;
        hashes.insert(key.compute_hash());
    }

    // All hashes should be unique
    EXPECT_EQ(hashes.size(), 6u);
}

//=============================================================================
// CachedShader Structure Tests
//=============================================================================

TEST(CachedShaderTest, DefaultState) {
    CachedShader shader{};
    EXPECT_EQ(shader.hash, 0u);
    EXPECT_EQ(shader.module, VK_NULL_HANDLE);
    EXPECT_TRUE(shader.spirv.empty());
}

TEST(CachedShaderTest, MetadataFlags) {
    CachedShader shader{};
    shader.uses_textures = true;
    shader.uses_vertex_fetch = false;
    shader.texture_bindings = 0x07;  // Textures 0, 1, 2
    shader.interpolant_mask = 0xFF;

    EXPECT_TRUE(shader.uses_textures);
    EXPECT_FALSE(shader.uses_vertex_fetch);
    EXPECT_EQ(shader.texture_bindings, 0x07u);
}

//=============================================================================
// ShaderCache Stats Tests
//=============================================================================

TEST(ShaderCacheStatsTest, DefaultZero) {
    ShaderCache::Stats stats{};
    EXPECT_EQ(stats.shader_compilations, 0u);
    EXPECT_EQ(stats.shader_cache_hits, 0u);
    EXPECT_EQ(stats.pipeline_creations, 0u);
    EXPECT_EQ(stats.pipeline_cache_hits, 0u);
}

} // namespace test
} // namespace x360mu
