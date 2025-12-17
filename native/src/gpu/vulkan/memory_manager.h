/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Vulkan Memory Manager
 * 
 * Handles GPU memory allocation for buffers and images.
 * Provides a unified interface for resource creation and staging uploads.
 */

#pragma once

#include "x360mu/types.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

namespace x360mu {

/**
 * Vulkan buffer wrapper with memory
 */
struct ManagedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    
    bool is_valid() const { return buffer != VK_NULL_HANDLE; }
};

/**
 * Vulkan image wrapper with view and memory
 */
struct ManagedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
    
    bool is_valid() const { return image != VK_NULL_HANDLE; }
};

/**
 * Staging buffer pool for efficient uploads
 */
struct StagingBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize offset = 0;  // Current allocation offset
    bool in_use = false;
};

/**
 * Vulkan Memory Manager
 * 
 * Provides high-level memory allocation and transfer operations
 * for the Vulkan backend.
 */
class VulkanMemoryManager {
public:
    VulkanMemoryManager() = default;
    ~VulkanMemoryManager();
    
    /**
     * Initialize the memory manager
     */
    Status initialize(VkDevice device, VkPhysicalDevice physical_device,
                     VkQueue transfer_queue, u32 queue_family_index);
    
    /**
     * Shutdown and release all resources
     */
    void shutdown();
    
    /**
     * Create a buffer with specified usage and memory properties
     */
    ManagedBuffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags properties);
    
    /**
     * Destroy a buffer and free its memory
     */
    void destroy_buffer(ManagedBuffer& buffer);
    
    /**
     * Create an image with specified parameters
     */
    ManagedImage create_image(u32 width, u32 height, VkFormat format,
                              VkImageUsageFlags usage, u32 mip_levels = 1);
    
    /**
     * Destroy an image and free its resources
     */
    void destroy_image(ManagedImage& image);
    
    /**
     * Upload data to a device-local buffer using staging
     */
    void upload_to_buffer(ManagedBuffer& buffer, const void* data, 
                         VkDeviceSize size, VkDeviceSize offset = 0);
    
    /**
     * Upload data to an image using staging
     */
    void upload_to_image(ManagedImage& image, const void* data, size_t size);
    
    /**
     * Create a staging buffer of specified size
     */
    ManagedBuffer create_staging_buffer(VkDeviceSize size);
    
    /**
     * Flush any pending upload operations
     */
    void flush_uploads();
    
    /**
     * Get memory type index for given requirements and properties
     */
    u32 find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties) const;
    
    // Statistics
    struct Stats {
        u64 total_allocated = 0;
        u64 buffer_count = 0;
        u64 image_count = 0;
        u64 staging_uploads = 0;
    };
    
    const Stats& get_stats() const { return stats_; }
    
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    u32 queue_family_index_ = 0;
    
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd_ = VK_NULL_HANDLE;
    VkFence upload_fence_ = VK_NULL_HANDLE;
    
    VkPhysicalDeviceMemoryProperties memory_properties_;
    
    // Staging buffer pool
    static constexpr VkDeviceSize STAGING_BUFFER_SIZE = 64 * 1024 * 1024;  // 64MB
    std::vector<StagingBuffer> staging_buffers_;
    
    Stats stats_;
    
    // Get or create a staging buffer for upload
    StagingBuffer* get_staging_buffer(VkDeviceSize size);
    
    // Begin/end transfer commands
    VkCommandBuffer begin_transfer();
    void end_transfer(VkCommandBuffer cmd);
    
    // Image layout transitions
    void transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                VkImageLayout old_layout, VkImageLayout new_layout,
                                u32 mip_levels = 1);
};

} // namespace x360mu
