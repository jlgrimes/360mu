/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Vulkan rendering backend
 */

#pragma once

#include "x360mu/types.h"
#include <vector>
#include <memory>
#include <unordered_map>

// Vulkan forward declarations
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef struct VkCommandPool_T* VkCommandPool;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
typedef struct VkSwapchainKHR_T* VkSwapchainKHR;
typedef struct VkImage_T* VkImage;
typedef struct VkImageView_T* VkImageView;
typedef struct VkRenderPass_T* VkRenderPass;
typedef struct VkFramebuffer_T* VkFramebuffer;
typedef struct VkPipeline_T* VkPipeline;
typedef struct VkPipelineLayout_T* VkPipelineLayout;
typedef struct VkPipelineCache_T* VkPipelineCache;
typedef struct VkDescriptorSetLayout_T* VkDescriptorSetLayout;
typedef struct VkDescriptorPool_T* VkDescriptorPool;
typedef struct VkDescriptorSet_T* VkDescriptorSet;
typedef struct VkShaderModule_T* VkShaderModule;
typedef struct VkBuffer_T* VkBuffer;
typedef struct VkDeviceMemory_T* VkDeviceMemory;
typedef struct VkSampler_T* VkSampler;
typedef struct VkFence_T* VkFence;
typedef struct VkSemaphore_T* VkSemaphore;

namespace x360mu {

class Memory;
struct RenderState;

/**
 * Vulkan buffer with memory
 */
struct VulkanBuffer {
    VkBuffer buffer = nullptr;
    VkDeviceMemory memory = nullptr;
    u64 size = 0;
    void* mapped = nullptr;
};

/**
 * Vulkan image with view
 */
struct VulkanImage {
    VkImage image = nullptr;
    VkDeviceMemory memory = nullptr;
    VkImageView view = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 format = 0;
};

/**
 * Per-frame resources (double/triple buffering)
 */
struct FrameResources {
    VkCommandBuffer command_buffer = nullptr;
    VkFence fence = nullptr;
    VkSemaphore image_available = nullptr;
    VkSemaphore render_finished = nullptr;
    
    // Per-frame descriptor pool
    VkDescriptorPool descriptor_pool = nullptr;
    
    // Dynamic buffers for this frame
    VulkanBuffer uniform_buffer;
    VulkanBuffer vertex_buffer;
    VulkanBuffer index_buffer;
};

/**
 * Compiled pipeline state
 */
struct PipelineState {
    u64 hash;
    VkPipeline pipeline = nullptr;
    VkPipelineLayout layout = nullptr;
};

/**
 * Vulkan rendering backend
 */
class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();
    
    /**
     * Initialize Vulkan
     */
    Status initialize(const std::string& cache_path);
    
    /**
     * Shutdown and release resources
     */
    void shutdown();
    
    /**
     * Set native window surface
     */
    Status set_surface(void* native_window);
    
    /**
     * Handle surface resize
     */
    void resize(u32 width, u32 height);
    
    /**
     * Begin a new frame
     */
    Status begin_frame();
    
    /**
     * End frame and present
     */
    void end_frame();
    
    /**
     * Begin render pass
     */
    void begin_render_pass(const VulkanImage& color_target, const VulkanImage* depth_target);
    
    /**
     * End render pass
     */
    void end_render_pass();
    
    /**
     * Bind pipeline for draw
     */
    void bind_pipeline(const PipelineState& pipeline);
    
    /**
     * Set viewport and scissor
     */
    void set_viewport(f32 x, f32 y, f32 width, f32 height, f32 min_depth, f32 max_depth);
    void set_scissor(u32 x, u32 y, u32 width, u32 height);
    
    /**
     * Bind vertex/index buffers
     */
    void bind_vertex_buffer(const VulkanBuffer& buffer, u64 offset);
    void bind_index_buffer(const VulkanBuffer& buffer, u64 offset, bool use_32bit);
    
    /**
     * Draw commands
     */
    void draw(u32 vertex_count, u32 first_vertex);
    void draw_indexed(u32 index_count, u32 first_index, s32 vertex_offset);
    
    /**
     * Create shader module from SPIR-V
     */
    VkShaderModule create_shader_module(const std::vector<u32>& spirv);
    
    /**
     * Create pipeline
     */
    PipelineState create_pipeline(
        VkShaderModule vertex_shader,
        VkShaderModule fragment_shader,
        const RenderState& state
    );
    
    /**
     * Create buffer
     */
    VulkanBuffer create_buffer(u64 size, u32 usage, bool host_visible);
    void destroy_buffer(VulkanBuffer& buffer);
    
    /**
     * Create image
     */
    VulkanImage create_image(u32 width, u32 height, u32 format, u32 usage);
    void destroy_image(VulkanImage& image);
    
    /**
     * Upload data to buffer
     */
    void upload_buffer(const VulkanBuffer& buffer, const void* data, u64 size, u64 offset = 0);
    
    /**
     * Upload data to image
     */
    void upload_image(const VulkanImage& image, const void* data, u32 width, u32 height);
    
    // Accessors
    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    u32 graphics_queue_family() const { return graphics_queue_family_; }
    
private:
    // Instance and device
    VkInstance instance_ = nullptr;
    VkPhysicalDevice physical_device_ = nullptr;
    VkDevice device_ = nullptr;
    
    // Queues
    VkQueue graphics_queue_ = nullptr;
    VkQueue present_queue_ = nullptr;
    u32 graphics_queue_family_ = 0;
    
    // Surface and swapchain
    VkSurfaceKHR surface_ = nullptr;
    VkSwapchainKHR swapchain_ = nullptr;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    u32 swapchain_width_ = 0;
    u32 swapchain_height_ = 0;
    u32 current_image_ = 0;
    
    // Command pools
    VkCommandPool command_pool_ = nullptr;
    
    // Per-frame resources (double buffering)
    static constexpr u32 FRAMES_IN_FLIGHT = 2;
    std::array<FrameResources, FRAMES_IN_FLIGHT> frames_;
    u32 current_frame_ = 0;
    
    // Render pass
    VkRenderPass render_pass_ = nullptr;
    std::vector<VkFramebuffer> framebuffers_;
    
    // Pipeline cache
    VkPipelineCache pipeline_cache_ = nullptr;
    std::unordered_map<u64, PipelineState> pipeline_map_;
    
    // Default resources
    VkSampler default_sampler_ = nullptr;
    VkDescriptorSetLayout descriptor_set_layout_ = nullptr;
    
    // State
    std::string cache_path_;
    bool in_frame_ = false;
    bool in_render_pass_ = false;
    
    // Helper methods
    Status create_instance();
    Status select_physical_device();
    Status create_device();
    Status create_swapchain();
    Status create_render_pass();
    Status create_framebuffers();
    Status create_command_pool();
    Status create_frame_resources();
    Status create_pipeline_cache();
    Status create_default_resources();
    
    void cleanup_swapchain();
    void save_pipeline_cache();
    
    u32 find_memory_type(u32 type_filter, u32 properties);
    VkCommandBuffer begin_single_time_commands();
    void end_single_time_commands(VkCommandBuffer cmd);
};

} // namespace x360mu

