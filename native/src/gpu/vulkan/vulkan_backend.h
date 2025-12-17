/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Vulkan rendering backend
 */

#pragma once

#include "x360mu/types.h"
#include <vector>
#include <memory>
#include <array>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace x360mu {

class Memory;
struct RenderState;

constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;

/**
 * Vulkan buffer with memory
 */
struct VulkanBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    u64 size = 0;
    void* mapped = nullptr;
};

/**
 * Vulkan image with view
 */
struct VulkanImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    u32 width = 0;
    u32 height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

/**
 * Pipeline render state (used to create graphics pipelines)
 */
struct PipelineState {
    VkPrimitiveTopology primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    
    VkBool32 depth_test_enable = VK_TRUE;
    VkBool32 depth_write_enable = VK_TRUE;
    VkCompareOp depth_compare_op = VK_COMPARE_OP_LESS;
    
    VkBool32 stencil_test_enable = VK_FALSE;
    VkStencilOp stencil_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp stencil_pass_op = VK_STENCIL_OP_KEEP;
    VkCompareOp stencil_compare_op = VK_COMPARE_OP_ALWAYS;
    
    VkBool32 blend_enable = VK_FALSE;
    VkBlendFactor src_color_blend = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_color_blend = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_blend_op = VK_BLEND_OP_ADD;
    VkBlendFactor src_alpha_blend = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_alpha_blend = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alpha_blend_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags color_write_mask = VK_COLOR_COMPONENT_R_BIT | 
                                              VK_COLOR_COMPONENT_G_BIT | 
                                              VK_COLOR_COMPONENT_B_BIT | 
                                              VK_COLOR_COMPONENT_A_BIT;
    
    u64 compute_hash() const {
        // FNV-1a hash of the state
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
 * Vulkan rendering backend
 * 
 * Handles all Vulkan rendering for the emulator, translating
 * Xbox 360 Xenos GPU commands to Vulkan draw calls.
 */
class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();
    
    /**
     * Initialize Vulkan with a native window
     */
    Status initialize(void* native_window, u32 width, u32 height);
    
    /**
     * Shutdown and release resources
     */
    void shutdown();
    
    /**
     * Handle window resize
     */
    Status resize(u32 width, u32 height);
    
    /**
     * Begin a new frame
     */
    Status begin_frame();
    
    /**
     * End frame and present
     */
    Status end_frame();
    
    /**
     * Get or create a graphics pipeline
     */
    VkPipeline get_or_create_pipeline(const PipelineState& state,
                                       VkShaderModule vertex_shader,
                                       VkShaderModule fragment_shader);
    
    /**
     * Create shader module from SPIR-V
     */
    VkShaderModule create_shader_module(const std::vector<u32>& spirv);
    
    /**
     * Destroy shader module
     */
    void destroy_shader_module(VkShaderModule module);
    
    /**
     * Bind graphics pipeline
     */
    void bind_pipeline(VkPipeline pipeline);
    
    /**
     * Bind vertex buffer
     */
    void bind_vertex_buffer(VkBuffer buffer, VkDeviceSize offset = 0);
    
    /**
     * Bind index buffer
     */
    void bind_index_buffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
    
    /**
     * Bind descriptor set
     */
    void bind_descriptor_set(VkDescriptorSet set);
    
    /**
     * Draw primitives
     */
    void draw(u32 vertex_count, u32 instance_count = 1, 
              u32 first_vertex = 0, u32 first_instance = 0);
    
    /**
     * Draw indexed primitives
     */
    void draw_indexed(u32 index_count, u32 instance_count = 1,
                      u32 first_index = 0, s32 vertex_offset = 0, 
                      u32 first_instance = 0);
    
    /**
     * Create buffer
     */
    VulkanBuffer create_buffer(u64 size, VkBufferUsageFlags usage, 
                               VkMemoryPropertyFlags properties);
    
    /**
     * Destroy buffer
     */
    void destroy_buffer(VulkanBuffer& buffer);
    
    /**
     * Create image
     */
    VulkanImage create_image(u32 width, u32 height, VkFormat format,
                             VkImageUsageFlags usage);
    
    /**
     * Destroy image
     */
    void destroy_image(VulkanImage& image);
    
    /**
     * Upload data to a buffer using a staging buffer
     */
    void upload_to_buffer(VulkanBuffer& buffer, const void* data, size_t size);
    
    /**
     * Upload data to an image using a staging buffer
     */
    void upload_to_image(VulkanImage& image, const void* data, size_t size);
    
    /**
     * Clear screen to a solid color (test function)
     */
    void clear_screen(float r, float g, float b);
    
    // Accessors
    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkCommandBuffer current_command_buffer() const { return command_buffers_[current_frame_]; }
    u32 graphics_queue_family() const { return graphics_queue_family_; }
    VkDescriptorPool descriptor_pool() const { return descriptor_pool_; }
    
private:
    // Instance and device
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    
    // Queues
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    u32 graphics_queue_family_ = 0;
    
    // Surface and swapchain
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_ = {};
    u32 current_image_index_ = 0;
    
    // Render pass and framebuffers
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    
    // Command pool and buffers
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    
    // Synchronization
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence> in_flight_fences_;
    u32 current_frame_ = 0;
    
    // Descriptor resources
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts_;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    
    // Pipeline cache
    std::unordered_map<u64, VkPipeline> pipeline_cache_;
    
    // eDRAM emulation (Xbox 360 has 10MB embedded DRAM)
    VkBuffer edram_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory edram_memory_ = VK_NULL_HANDLE;
    
    // Window dimensions
    u32 width_ = 0;
    u32 height_ = 0;
    
    // Helper methods
    VkResult create_instance();
    VkResult create_surface(void* native_window);
    VkResult create_device();
    VkResult create_swapchain();
    VkResult create_render_pass();
    VkResult create_framebuffers();
    VkResult create_command_resources();
    VkResult create_sync_objects();
    VkResult create_descriptor_resources();
    VkResult create_edram_resources();
    
    u32 find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties);
    void transition_image_layout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout);
    void transition_image_layout(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout);
    void cleanup_swapchain();
};

} // namespace x360mu
