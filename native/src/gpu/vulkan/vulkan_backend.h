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
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace x360mu {

class Memory;
struct RenderState;

constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;
constexpr u32 MAX_MRT_TARGETS = 4;

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
 * Vertex input configuration for pipeline creation
 * Built from Xbox 360 vertex fetch constants
 */
struct VertexInputConfig {
    static constexpr u32 MAX_BINDINGS = 16;
    static constexpr u32 MAX_ATTRIBUTES = 16;

    VkVertexInputBindingDescription bindings[MAX_BINDINGS];
    VkVertexInputAttributeDescription attributes[MAX_ATTRIBUTES];
    u32 binding_count = 0;
    u32 attribute_count = 0;

    u64 compute_hash() const {
        u64 hash = 14695981039346656037ULL;
        for (u32 i = 0; i < binding_count; i++) {
            hash ^= bindings[i].binding;
            hash *= 1099511628211ULL;
            hash ^= bindings[i].stride;
            hash *= 1099511628211ULL;
        }
        for (u32 i = 0; i < attribute_count; i++) {
            hash ^= attributes[i].location;
            hash *= 1099511628211ULL;
            hash ^= attributes[i].format;
            hash *= 1099511628211ULL;
            hash ^= attributes[i].offset;
            hash *= 1099511628211ULL;
        }
        return hash;
    }
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
    u32 color_attachment_count = 1;
    VkColorComponentFlags color_write_mask = VK_COLOR_COMPONENT_R_BIT |
                                              VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT |
                                              VK_COLOR_COMPONENT_A_BIT;

    // Vertex input state (from Xbox 360 fetch constants)
    VertexInputConfig vertex_input;

    u64 compute_hash() const {
        // FNV-1a hash of fixed fields (exclude vertex_input, hash separately)
        u64 hash = 14695981039346656037ULL;
        // Hash up to but not including vertex_input
        const u8* data = reinterpret_cast<const u8*>(this);
        size_t fixed_size = offsetof(PipelineState, vertex_input);
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
     * Bind multiple vertex buffers at consecutive binding slots
     */
    void bind_vertex_buffers(u32 first_binding, u32 count,
                             const VkBuffer* buffers, const VkDeviceSize* offsets);
    
    /**
     * Bind index buffer
     */
    void bind_index_buffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
    
    /**
     * Bind descriptor set
     */
    void bind_descriptor_set(VkDescriptorSet set, u32 first_set = 0);

    /**
     * Set dynamic viewport
     */
    void set_viewport(float x, float y, float width, float height,
                      float min_depth = 0.0f, float max_depth = 1.0f);

    /**
     * Set dynamic scissor
     */
    void set_scissor(s32 x, s32 y, u32 width, u32 height);

    /**
     * Get render pass for given color attachment count (1-4)
     */
    VkRenderPass get_render_pass(u32 color_attachment_count = 1) const;
    
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
    
    // ----- Occlusion query management -----

    /**
     * Initialize occlusion query pool
     * @param max_queries Maximum number of simultaneous queries (typically 256)
     */
    Status create_query_pool(u32 max_queries = 256);

    /**
     * Destroy query pool
     */
    void destroy_query_pool();

    /**
     * Begin an occlusion query
     * @param query_index Index into query pool (0..max_queries-1)
     */
    void begin_occlusion_query(u32 query_index);

    /**
     * End the active occlusion query
     * @param query_index Index that was passed to begin_occlusion_query
     */
    void end_occlusion_query(u32 query_index);

    /**
     * Reset query results before reuse
     * @param first_query Starting index
     * @param count Number of queries to reset
     */
    void reset_queries(u32 first_query, u32 count);

    /**
     * Get occlusion query result (blocking)
     * @param query_index Index to read
     * @param result Output: number of samples that passed
     * @return true if result available
     */
    bool get_query_result(u32 query_index, u64& result);

    /**
     * Begin conditional rendering based on query result
     * Uses VK_EXT_conditional_rendering if available, else CPU fallback
     * @param query_index Query whose result controls rendering
     * @param inverted If true, render when query result is zero
     */
    void begin_conditional_rendering(u32 query_index, bool inverted = false);

    /**
     * End conditional rendering
     */
    void end_conditional_rendering();

    /**
     * Check if VK_EXT_conditional_rendering is supported
     */
    bool has_conditional_rendering() const { return has_conditional_rendering_ext_; }

    /**
     * Clear screen to a solid color (test function)
     */
    void clear_screen(float r, float g, float b);

    // ----- Debug Utils (VK_EXT_debug_utils) -----

    /**
     * Set a debug name on a Vulkan object (pipelines, images, buffers, etc.)
     * No-op if debug utils extension is not loaded.
     */
    void set_object_name(VkObjectType type, u64 handle, const char* name);

    /**
     * Begin a debug label region in the current command buffer.
     * Shows up in RenderDoc / AGI GPU captures.
     */
    void cmd_begin_label(const char* label, float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);

    /**
     * End the current debug label region.
     */
    void cmd_end_label();

    /**
     * Insert a single debug label (marker) at the current point.
     */
    void cmd_insert_label(const char* label, float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);

    /**
     * Check if debug utils extension is available
     */
    bool has_debug_utils() const { return pfn_set_debug_name_ != nullptr; }

    /**
     * Frame counter (incremented each begin_frame)
     */
    u64 frame_number() const { return frame_number_; }

    /**
     * Set the Vulkan present mode (VSync control)
     * Takes effect on next swapchain recreation
     */
    void set_present_mode(VkPresentModeKHR mode);

    /**
     * Get the current present mode
     */
    VkPresentModeKHR present_mode() const { return present_mode_; }

    // Accessors
    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkCommandBuffer current_command_buffer() const { return command_buffers_[current_frame_]; }
    u32 graphics_queue_family() const { return graphics_queue_family_; }
    VkDescriptorPool descriptor_pool() const { return descriptor_pool_; }
    u32 current_frame() const { return current_frame_; }
    VkPipelineLayout pipeline_layout() const { return active_pipeline_layout(); }
    VkRenderPass render_pass() const { return render_pass_; }
    VkFormat depth_format() const { return depth_format_; }

    /**
     * Override the pipeline layout used for pipeline creation and descriptor binding.
     * When set, this layout is used instead of the backend's internal layout.
     * Ensures DescriptorManager's layout is used consistently everywhere.
     */
    void set_pipeline_layout_override(VkPipelineLayout layout) {
        pipeline_layout_override_ = layout;
    }
    
    // ----- Memexport SSBO management -----

    /**
     * Get or create the memexport SSBO descriptor set for binding at set=2.
     * Allocates a device-local buffer for shader memory export writes.
     */
    VkDescriptorSet get_memexport_descriptor_set();

    /**
     * Get the memexport SSBO buffer (for readback or barrier purposes)
     */
    VkBuffer memexport_buffer() const { return memexport_buffer_.buffer; }
    u64 memexport_buffer_size() const { return memexport_buffer_.size; }

    /**
     * Save/load Vulkan pipeline cache to/from disk
     */
    bool save_pipeline_cache(const std::string& path);
    bool load_pipeline_cache(const std::string& path);

    /**
     * Find suitable memory type for allocation
     */
    u32 find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties);
    
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
    VkRenderPass mrt_render_passes_[MAX_MRT_TARGETS - 1] = {};  // For 2, 3, 4 color attachments
    std::vector<VkFramebuffer> framebuffers_;

    // Depth resources
    VulkanImage depth_image_;
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;
    
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
    VkPipelineLayout pipeline_layout_override_ = VK_NULL_HANDLE;

    // Pipeline cache
    VkPipelineCache vk_pipeline_cache_ = VK_NULL_HANDLE;
    std::unordered_map<u64, VkPipeline> pipeline_cache_;
    
    // eDRAM emulation (Xbox 360 has 10MB embedded DRAM)
    VkBuffer edram_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory edram_memory_ = VK_NULL_HANDLE;

    // Memexport SSBO (for Xenos eM0-eM3 memory export writes)
    VulkanBuffer memexport_buffer_;
    VkDescriptorSet memexport_descriptor_set_ = VK_NULL_HANDLE;
    static constexpr u64 MEMEXPORT_BUFFER_SIZE = 4 * 1024 * 1024;  // 4 MB
    
    // Window dimensions
    u32 width_ = 0;
    u32 height_ = 0;

    // Present mode (VSync control)
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool swapchain_needs_recreation_ = false;

    // Occlusion query pool
    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    u32 query_pool_size_ = 0;

    // Query result buffer (for conditional rendering CPU fallback)
    VulkanBuffer query_result_buffer_;

    // Frame counter for debug labels
    u64 frame_number_ = 0;

    // Conditional rendering extension support
    bool has_conditional_rendering_ext_ = false;
    PFN_vkCmdBeginConditionalRenderingEXT pfn_begin_conditional_ = nullptr;
    PFN_vkCmdEndConditionalRenderingEXT pfn_end_conditional_ = nullptr;

    // Debug utils extension support
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    PFN_vkSetDebugUtilsObjectNameEXT pfn_set_debug_name_ = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT pfn_cmd_begin_label_ = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT pfn_cmd_end_label_ = nullptr;
    PFN_vkCmdInsertDebugUtilsLabelEXT pfn_cmd_insert_label_ = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT pfn_create_debug_messenger_ = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pfn_destroy_debug_messenger_ = nullptr;
    
    // Helper methods
    VkResult create_instance();
    VkResult create_surface(void* native_window);
    VkResult create_device();
    VkResult create_swapchain();
    VkResult create_render_pass();
    VkResult create_mrt_render_passes();
    VkResult create_depth_resources();
    VkResult create_framebuffers();
    VkFormat find_depth_format();
    VkResult create_command_resources();
    VkResult create_sync_objects();
    VkResult create_descriptor_resources();
    VkResult create_edram_resources();
    
    // find_memory_type is declared public above
    VkPipelineLayout active_pipeline_layout() const {
        return pipeline_layout_override_ != VK_NULL_HANDLE ? pipeline_layout_override_ : pipeline_layout_;
    }

    void transition_image_layout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout);
    void transition_image_layout(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout);
    void cleanup_swapchain();
    void setup_debug_utils();
    void destroy_debug_utils();
};

} // namespace x360mu
