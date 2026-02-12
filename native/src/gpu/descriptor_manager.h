/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Descriptor Set Manager - Manages Vulkan descriptor sets for shader resources
 * 
 * Handles uniform buffers, textures, and samplers for GPU rendering
 */

#pragma once

#include "x360mu/types.h"
#include "xenos/gpu.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <array>
#include <unordered_map>

namespace x360mu {

class VulkanBackend;

/**
 * Per-frame descriptor set resources
 */
struct FrameDescriptors {
    VkDescriptorSet set;
    
    // Uniform buffers for shader constants
    VkBuffer vertex_constants_buffer;
    VkDeviceMemory vertex_constants_memory;
    void* vertex_constants_mapped;
    
    VkBuffer pixel_constants_buffer;
    VkDeviceMemory pixel_constants_memory;
    void* pixel_constants_mapped;
    
    // Bool and loop constants
    VkBuffer bool_constants_buffer;
    VkDeviceMemory bool_constants_memory;
    void* bool_constants_mapped;
    
    VkBuffer loop_constants_buffer;
    VkDeviceMemory loop_constants_memory;
    void* loop_constants_mapped;
    
    bool needs_update;
};

/**
 * Descriptor Manager
 * 
 * Manages Vulkan descriptor sets for Xbox 360 GPU emulation.
 * Handles constant buffers, textures, and samplers.
 */
class DescriptorManager {
public:
    DescriptorManager();
    ~DescriptorManager();
    
    /**
     * Initialize with Vulkan backend
     */
    Status initialize(VulkanBackend* vulkan);
    
    /**
     * Shutdown and cleanup
     */
    void shutdown();
    
    /**
     * Begin frame - get descriptor set for current frame
     */
    VkDescriptorSet begin_frame(u32 frame_index);
    
    /**
     * Update vertex shader constants
     */
    void update_vertex_constants(u32 frame_index, const f32* constants, u32 count);
    
    /**
     * Update pixel shader constants
     */
    void update_pixel_constants(u32 frame_index, const f32* constants, u32 count);
    
    /**
     * Update boolean constants
     */
    void update_bool_constants(u32 frame_index, const u32* constants, u32 count);
    
    /**
     * Update loop constants
     */
    void update_loop_constants(u32 frame_index, const u32* constants, u32 count);
    
    /**
     * Bind textures for the current draw
     * @param frame_index Current frame index
     * @param views Array of image views (up to 16)
     * @param samplers Array of samplers (up to 16)
     * @param count Number of textures to bind
     */
    void bind_textures(u32 frame_index, const VkImageView* views, 
                      const VkSampler* samplers, u32 count);
    
    /**
     * Get descriptor set layout (set 0: UBOs + samplers)
     */
    VkDescriptorSetLayout get_layout() const { return layout_; }

    /**
     * Get SSBO descriptor set layout (set 2: memexport storage buffers)
     */
    VkDescriptorSetLayout get_ssbo_layout() const { return ssbo_layout_; }

    /**
     * Get pipeline layout
     */
    VkPipelineLayout get_pipeline_layout() const { return pipeline_layout_; }
    
    // Constants
    static constexpr u32 MAX_FRAMES = 3;
    static constexpr u32 MAX_FLOAT_CONSTANTS = 256;   // 256 vec4 constants
    static constexpr u32 MAX_BOOL_CONSTANTS = 8;      // 256 bits = 8 u32
    static constexpr u32 MAX_LOOP_CONSTANTS = 32;
    static constexpr u32 MAX_TEXTURE_BINDINGS = 16;
    
private:
    VulkanBackend* vulkan_ = nullptr;
    
    // Descriptor pool and layout
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ssbo_layout_ = VK_NULL_HANDLE;  // Set 2: memexport SSBO
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    
    // Per-frame resources
    std::array<FrameDescriptors, MAX_FRAMES> frames_;
    
    // Default sampler for unbound texture slots
    VkSampler default_sampler_ = VK_NULL_HANDLE;
    VkImage default_texture_ = VK_NULL_HANDLE;
    VkDeviceMemory default_texture_memory_ = VK_NULL_HANDLE;
    VkImageView default_texture_view_ = VK_NULL_HANDLE;
    
    // Helpers
    Status create_descriptor_pool();
    Status create_descriptor_layout();
    Status create_pipeline_layout();
    Status create_frame_resources(FrameDescriptors& frame);
    Status create_default_resources();
    void destroy_frame_resources(FrameDescriptors& frame);
    void destroy_default_resources();
    
    VkBuffer create_buffer(u64 size, VkBufferUsageFlags usage, VkDeviceMemory& memory, void** mapped);
    void destroy_buffer(VkBuffer& buffer, VkDeviceMemory& memory, void** mapped);
};

} // namespace x360mu
