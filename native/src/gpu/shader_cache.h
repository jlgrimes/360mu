/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Shader Cache - Manages VkShaderModules and pipeline caching
 *
 * Bridges the ShaderTranslator (Xenos -> SPIR-V) with Vulkan pipelines.
 * Supports per-game disk caching with LRU eviction for fast second boot.
 */

#pragma once

#include "x360mu/types.h"
#include "xenos/gpu.h"
#include "vulkan/vulkan_backend.h"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <list>
#include <mutex>
#include <chrono>

namespace x360mu {

// Bump when shader translator output format changes to invalidate stale caches
static constexpr u32 SHADER_CACHE_VERSION = 2;

// Default max disk cache size per game (256 MB)
static constexpr u64 DEFAULT_MAX_CACHE_SIZE = 256 * 1024 * 1024;

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

    // Memexport support
    struct {
        bool uses_memexport = false;
    } info;
};

/**
 * Disk cache entry for LRU tracking
 */
struct DiskCacheEntry {
    u64 hash;
    u64 disk_size;         // Size of the .spv file on disk in bytes
    u64 last_access_time;  // Unix timestamp of last use
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

    // Vertex input state (from fetch constants)
    VertexInputConfig vertex_input;

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
               color_blend_op == other.color_blend_op &&
               vertex_input.compute_hash() == other.vertex_input.compute_hash();
    }

    u64 compute_hash() const {
        u64 hash = 14695981039346656037ULL;
        const u8* data = reinterpret_cast<const u8*>(this);
        // Hash up to but not including vertex_input
        size_t fixed_size = offsetof(PipelineKey, vertex_input);
        for (size_t i = 0; i < fixed_size; i++) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        // Combine with vertex input hash
        hash ^= vertex_input.compute_hash();
        hash *= 1099511628211ULL;
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
 * Supports per-game disk caching with LRU eviction and VkPipelineCache persistence.
 */
class ShaderCache {
public:
    ShaderCache();
    ~ShaderCache();

    Status initialize(VulkanBackend* vulkan, ShaderTranslator* translator,
                     const std::string& cache_path);

    /**
     * Set current game's title ID for per-game cache directories.
     * Creates the directory and loads existing cache for that game.
     */
    void set_title_id(u32 title_id);

    void shutdown();

    const CachedShader* get_shader(const void* microcode, u32 size, ShaderType type);

    VkPipeline get_pipeline(const CachedShader* vertex_shader,
                           const CachedShader* pixel_shader,
                           const PipelineKey& key);

    void clear();
    void save_cache();
    void load_cache();

    void set_max_cache_size(u64 max_bytes) { max_cache_size_ = max_bytes; }
    u64 get_disk_cache_size() const { return total_disk_size_; }

    struct Stats {
        u64 shader_compilations;
        u64 shader_cache_hits;
        u64 shader_disk_hits;
        u64 pipeline_creations;
        u64 pipeline_cache_hits;
        u64 shaders_evicted;
    };
    Stats get_stats() const { return stats_; }

private:
    VulkanBackend* vulkan_ = nullptr;
    ShaderTranslator* translator_ = nullptr;
    std::string base_cache_path_;
    std::string game_cache_path_;
    u32 title_id_ = 0;
    u64 max_cache_size_ = DEFAULT_MAX_CACHE_SIZE;
    u64 total_disk_size_ = 0;

    // Shader cache (hash -> cached shader)
    std::unordered_map<u64, CachedShader> shader_cache_;

    // Pipeline cache (key hash -> cached pipeline)
    std::unordered_map<u64, CachedPipeline> pipeline_cache_;

    // Disk cache LRU tracking
    std::unordered_map<u64, DiskCacheEntry> disk_index_;
    std::list<u64> lru_order_;  // Front = most recently used

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

    // Disk cache helpers
    std::string shader_file_path(u64 hash) const;
    bool save_shader_to_disk(const CachedShader& shader);
    bool load_shader_from_disk(u64 hash, CachedShader& out);
    void save_index();
    void load_index();
    void touch_lru(u64 hash);
    void evict_lru();
    void ensure_directory(const std::string& path);
};

} // namespace x360mu
