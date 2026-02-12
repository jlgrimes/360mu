/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Texture Cache - Manages GPU textures from Xenos fetch constants
 * 
 * Handles texture format conversion, tiling, and Vulkan resource management
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
class Memory;

/**
 * Cached texture entry
 */
struct CachedTexture {
    u64 hash;
    GuestAddr address;
    u32 width;
    u32 height;
    u32 depth;
    TextureFormat format;
    
    // Vulkan resources
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    
    // Metadata
    bool is_tiled;
    u32 mip_levels;
    u32 array_layers;
    u64 last_use_frame;
    
    bool is_valid() const { return image != VK_NULL_HANDLE; }
};

/**
 * Vulkan sampler state for texture sampling
 * Named VkSamplerConfig to avoid conflict with SamplerState in texture.h
 */
struct VkSamplerConfig {
    VkFilter min_filter;
    VkFilter mag_filter;
    VkSamplerMipmapMode mipmap_mode;
    VkSamplerAddressMode address_u;
    VkSamplerAddressMode address_v;
    VkSamplerAddressMode address_w;
    f32 mip_lod_bias;
    f32 max_anisotropy;
    VkBorderColor border_color;
    
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
 * Texture Cache
 * 
 * Manages GPU textures, handling format conversion and tiling.
 */
class TextureCacheImpl {
public:
    TextureCacheImpl();
    ~TextureCacheImpl();
    
    /**
     * Initialize with dependencies
     */
    Status initialize(VulkanBackend* vulkan, Memory* memory);
    
    /**
     * Shutdown and cleanup
     */
    void shutdown();
    
    /**
     * Get or create texture from fetch constant
     */
    const CachedTexture* get_texture(const FetchConstant& fetch);
    
    /**
     * Get or create sampler for state
     */
    VkSampler get_sampler(const VkSamplerConfig& state);
    
    /**
     * Invalidate textures in address range
     */
    void invalidate(GuestAddr base, u64 size);
    
    /**
     * Clear all cached textures
     */
    void clear();
    
    /**
     * Called each frame to manage texture lifetime
     */
    void begin_frame(u64 frame_number);
    
    /**
     * Statistics
     */
    struct Stats {
        u64 textures_created;
        u64 texture_cache_hits;
        u64 textures_invalidated;
        u64 bytes_uploaded;
    };
    Stats get_stats() const { return stats_; }
    
private:
    VulkanBackend* vulkan_ = nullptr;
    Memory* memory_ = nullptr;
    
    // Texture cache (hash -> cached texture)
    std::unordered_map<u64, CachedTexture> texture_cache_;
    
    // Sampler cache (hash -> VkSampler)
    std::unordered_map<u64, VkSampler> sampler_cache_;
    
    // Thread safety
    std::mutex texture_mutex_;
    std::mutex sampler_mutex_;
    
    // Current frame number
    u64 current_frame_ = 0;
    
    // Stats
    Stats stats_{};
    
    // Staging buffer for texture uploads
    VkBuffer staging_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    void* staging_mapped_ = nullptr;
    static constexpr u64 STAGING_SIZE = 64 * 1024 * 1024;  // 64MB staging buffer
    
    // Helpers
    u64 compute_texture_hash(const FetchConstant& fetch);
    CachedTexture create_texture(const FetchConstant& fetch);
    void destroy_texture(CachedTexture& texture);

    VkFormat translate_format(TextureFormat format);
    u32 get_bytes_per_block(TextureFormat format);
    u32 get_block_size(TextureFormat format);
    u64 compute_texture_data_size(u32 width, u32 height, TextureFormat format, u32 mip_levels);

    void detile_texture(const void* src, void* dst, u32 width, u32 height,
                       TextureFormat format, bool is_tiled);
    void detile_texture_3d(const void* src, void* dst, u32 width, u32 height,
                           u32 depth, TextureFormat format, bool is_tiled);
    void detile_texture_cube(const void* src, void* dst, u32 face_size, u32 height,
                             TextureFormat format, bool is_tiled);
    void detile_packed_mip_tail(const void* src, void* dst, u32 base_width, u32 base_height,
                                u32 start_mip, u32 mip_count, TextureFormat format);
    void byte_swap_texture_data(void* data, u64 size, TextureFormat format);
    void upload_texture(CachedTexture& texture, const void* data, u64 size);
    void upload_texture_mips(CachedTexture& texture, const void* data, u64 total_size);

    VkSamplerConfig translate_sampler(const FetchConstant& fetch);
    static VkSamplerAddressMode translate_address_mode(TextureAddressMode mode);
    static VkFilter translate_filter(TextureFilter filter);

    Status create_staging_buffer();
    void destroy_staging_buffer();
};

} // namespace x360mu
