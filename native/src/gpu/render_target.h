/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Render Target Manager - Manages Vulkan render targets for Xbox 360 GPU emulation
 * 
 * Integrates eDRAM emulation with Vulkan framebuffer management
 */

#pragma once

#include "x360mu/types.h"
#include "xenos/gpu.h"
#include "xenos/edram.h"
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <unordered_map>
#include <memory>

namespace x360mu {

class VulkanBackend;
class Memory;
class EdramManager;

/**
 * Vulkan render target
 */
struct VulkanRenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 edram_base = 0;
    bool is_depth = false;
    bool needs_clear = false;
    
    bool is_valid() const { return image != VK_NULL_HANDLE; }
};

/**
 * Framebuffer configuration
 */
struct FramebufferKey {
    std::array<u64, 4> color_rt_hashes;
    u64 depth_rt_hash;
    u32 width;
    u32 height;
    
    bool operator==(const FramebufferKey& other) const;
    u64 compute_hash() const;
};

struct FramebufferKeyHash {
    size_t operator()(const FramebufferKey& key) const {
        return static_cast<size_t>(key.compute_hash());
    }
};

/**
 * Cached framebuffer
 */
struct CachedFramebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    u32 width = 0;
    u32 height = 0;
    std::vector<VkImageView> attachments;
};

/**
 * Render Target Manager
 * 
 * Manages Xbox 360 render targets using Vulkan.
 * Handles:
 * - Render target creation from eDRAM configuration
 * - Framebuffer caching
 * - eDRAM resolve to main memory
 * - Format conversion
 */
class RenderTargetManager {
public:
    RenderTargetManager();
    ~RenderTargetManager();
    
    /**
     * Initialize with Vulkan backend
     */
    Status initialize(VulkanBackend* vulkan, Memory* memory, EdramManager* edram);
    
    /**
     * Shutdown and cleanup
     */
    void shutdown();
    
    /**
     * Begin frame - update render targets from GPU state
     */
    void begin_frame();
    
    /**
     * End frame - handle any pending resolves
     */
    void end_frame();
    
    /**
     * Set render target configuration from GPU registers
     */
    void set_color_target(u32 index, u32 edram_base, u32 pitch, 
                          SurfaceFormat format, u32 width, u32 height);
    
    /**
     * Set depth/stencil target configuration
     */
    void set_depth_target(u32 edram_base, u32 pitch,
                          SurfaceFormat format, u32 width, u32 height);
    
    /**
     * Get or create framebuffer for current render target state
     */
    CachedFramebuffer* get_current_framebuffer();
    
    /**
     * Get render pass for current configuration
     */
    VkRenderPass get_current_render_pass();
    
    /**
     * Clear render targets
     */
    void clear_color_target(u32 index, f32 r, f32 g, f32 b, f32 a);
    void clear_depth_stencil(f32 depth, u8 stencil);
    
    /**
     * Resolve render target to main memory
     * Called when game issues a resolve command
     */
    void resolve_to_memory(u32 rt_index, GuestAddr dest_address, 
                           u32 dest_pitch, u32 width, u32 height);
    
    /**
     * Copy from main memory to eDRAM
     * For restoring previous framebuffer contents
     */
    void copy_from_memory(u32 rt_index, GuestAddr src_address,
                          u32 src_pitch, u32 width, u32 height);
    
    /**
     * Get color render target image view
     */
    VkImageView get_color_view(u32 index);
    
    /**
     * Get depth render target image view
     */
    VkImageView get_depth_view();
    
    // Configuration
    static constexpr u32 MAX_COLOR_TARGETS = 4;
    
private:
    VulkanBackend* vulkan_ = nullptr;
    Memory* memory_ = nullptr;
    EdramManager* edram_ = nullptr;
    
    // Current render targets
    std::array<VulkanRenderTarget, MAX_COLOR_TARGETS> color_targets_;
    VulkanRenderTarget depth_target_;
    
    // Current configuration
    struct {
        u32 color_edram_base[MAX_COLOR_TARGETS];
        u32 color_pitch[MAX_COLOR_TARGETS];
        SurfaceFormat color_format[MAX_COLOR_TARGETS];
        u32 color_width[MAX_COLOR_TARGETS];
        u32 color_height[MAX_COLOR_TARGETS];
        bool color_enabled[MAX_COLOR_TARGETS];
        
        u32 depth_edram_base;
        u32 depth_pitch;
        SurfaceFormat depth_format;
        u32 depth_width;
        u32 depth_height;
        bool depth_enabled;
    } config_;
    
    // Framebuffer cache
    std::unordered_map<FramebufferKey, CachedFramebuffer, FramebufferKeyHash> framebuffer_cache_;
    
    // Render pass cache
    std::unordered_map<u64, VkRenderPass> render_pass_cache_;
    
    // Current framebuffer
    CachedFramebuffer* current_framebuffer_ = nullptr;
    
    // Frame tracking for cleanup
    u64 current_frame_ = 0;
    
    // Staging buffer for resolves
    VkBuffer staging_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    void* staging_mapped_ = nullptr;
    static constexpr u64 STAGING_SIZE = 32 * 1024 * 1024;  // 32MB
    
    // Helper functions
    Status create_staging_buffer();
    void destroy_staging_buffer();
    
    VulkanRenderTarget create_render_target(u32 width, u32 height, 
                                            VkFormat format, bool is_depth);
    void destroy_render_target(VulkanRenderTarget& rt);
    
    VkFormat translate_surface_format(SurfaceFormat format, bool is_depth);
    
    u64 compute_render_target_hash(u32 edram_base, u32 pitch, 
                                   SurfaceFormat format, u32 width, u32 height);
    
    VkRenderPass create_render_pass(const std::array<VkFormat, MAX_COLOR_TARGETS>& color_formats,
                                    u32 color_count, VkFormat depth_format, bool has_depth);
    
    CachedFramebuffer create_framebuffer(VkRenderPass render_pass,
                                         const std::vector<VkImageView>& attachments,
                                         u32 width, u32 height);
    
    void resolve_render_target_to_buffer(const VulkanRenderTarget& rt, 
                                         VkBuffer dest, u64 offset);
};

} // namespace x360mu
