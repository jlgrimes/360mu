/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Shader Cache - Manages VkShaderModules and pipeline caching
 * 
 * Bridges the ShaderTranslator (Xenos -> SPIR-V) with Vulkan pipelines
 */

#pragma once

#include "x360mu/types.h"
#include "xenos/gpu.h"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace x360mu {

class VulkanBackend;
class ShaderTranslator;
class Memory;

/**
 * Cached shader entry with VkShaderModule
 */
struct CachedShader {
    u64 hash;
    ShaderType type;
    VkShaderModule module;
    std::vector<u32> spirv;
    
    // Shader metadata
    bool uses_textures;
    bool uses_vertex_fetch;
    u32 texture_bindings;       // Bitmask of used texture bindings
    u32 vertex_fetch_bindings;  // Bitmask of used vertex fetch slots
    u32 interpolant_mask;       // Bitmask of interpolants used
};

/**
 * Pipeline key for caching graphics pipelines
 */
struct PipelineKey {
    u64 vertex_shader_hash;
    u64 pixel_shader_hash;
    
    // Render state that affects pipeline
    VkPrimitiveTopology primitive_topology;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkBool32 depth_test_enable;
    VkBool32 depth_write_enable;
    VkCompareOp depth_compare_op;
    VkBool32 blend_enable;
    VkBlendFactor src_color_blend;
    VkBlendFactor dst_color_blend;
    VkBlendOp color_blend_op;
    
    bool operator==(const PipelineKey& other) const {
        return vertex_shader_hash == other.vertex_shader_hash &&
               pixel_shader_hash == other.pixel_shader_hash &&
               primitive_topology == other.primitive_topology &&
               cull_mode == other.cull_mode &&
               front_face == other.front_face &&
               depth_test_enable == other.depth_test_enable &&
               depth_write_enable == other.depth_write_enable &&
               depth_compare_op == other.depth_compare_op &&
               blend_enable == other.blend_enable &&
               src_color_blend == other.src_color_blend &&
               dst_color_blend == other.dst_color_blend &&
               color_blend_op == other.color_blend_op;
    }
    
    u64 compute_hash() const {
        u64 hash = 14695981039346656037ULL;
        const u8* data = reinterpret_cast<const u8*>(this);
        for (size_t i = 0; i < sizeof(*this); i++) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

/**
 * Cached pipeline entry
 */
struct CachedPipeline {
    PipelineKey key;
    VkPipeline pipeline;
    VkPipelineLayout layout;
};

/**
 * Shader and Pipeline Cache
 * 
 * Manages the translation of Xenos shaders to Vulkan and caches
 * both shader modules and complete graphics pipelines.
 */
class ShaderCache {
public:
    ShaderCache();
    ~ShaderCache();
    
    /**
     * Initialize with dependencies
     */
    Status initialize(VulkanBackend* vulkan, ShaderTranslator* translator,
                     const std::string& cache_path);
    
    /**
     * Shutdown and cleanup
     */
    void shutdown();
    
    /**
     * Get or create shader module from Xenos microcode
     */
    const CachedShader* get_shader(const void* microcode, u32 size, ShaderType type);
    
    /**
     * Get or create graphics pipeline for the given state
     */
    VkPipeline get_pipeline(const CachedShader* vertex_shader,
                           const CachedShader* pixel_shader,
                           const PipelineKey& key);
    
    /**
     * Invalidate all cached shaders
     */
    void clear();
    
    /**
     * Save shader cache to disk
     */
    void save_cache();
    
    /**
     * Load shader cache from disk  
     */
    void load_cache();
    
    /**
     * Statistics
     */
    struct Stats {
        u64 shader_compilations;
        u64 shader_cache_hits;
        u64 pipeline_creations;
        u64 pipeline_cache_hits;
    };
    Stats get_stats() const { return stats_; }
    
private:
    VulkanBackend* vulkan_ = nullptr;
    ShaderTranslator* translator_ = nullptr;
    std::string cache_path_;
    
    // Shader cache (hash -> cached shader)
    std::unordered_map<u64, CachedShader> shader_cache_;
    
    // Pipeline cache (key hash -> cached pipeline)
    std::unordered_map<u64, CachedPipeline> pipeline_cache_;
    
    // Thread safety
    std::mutex shader_mutex_;
    std::mutex pipeline_mutex_;
    
    // Stats
    Stats stats_{};
    
    // Helpers
    u64 compute_microcode_hash(const void* data, u32 size);
    VkShaderModule create_shader_module(const std::vector<u32>& spirv);
    VkPipeline create_graphics_pipeline(const CachedShader* vs,
                                        const CachedShader* ps,
                                        const PipelineKey& key);
};

} // namespace x360mu
