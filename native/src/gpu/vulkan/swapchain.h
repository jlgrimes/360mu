/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Vulkan Swapchain Management
 * 
 * Handles swapchain creation, presentation, and recreation on resize.
 */

#pragma once

#include "x360mu/types.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace x360mu {

/**
 * Swapchain configuration
 */
struct SwapchainConfig {
    u32 width = 0;
    u32 height = 0;
    VkFormat format = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    u32 min_image_count = 3;  // Triple buffering
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
};

/**
 * Vulkan Swapchain
 * 
 * Manages the swapchain and its resources for presentation.
 */
class VulkanSwapchain {
public:
    VulkanSwapchain() = default;
    ~VulkanSwapchain();
    
    /**
     * Initialize the swapchain
     */
    Status initialize(VkDevice device, VkPhysicalDevice physical_device,
                     VkSurfaceKHR surface, const SwapchainConfig& config);
    
    /**
     * Shutdown and release resources
     */
    void shutdown();
    
    /**
     * Recreate swapchain with new dimensions
     */
    Status recreate(u32 width, u32 height);
    
    /**
     * Acquire the next swapchain image
     * Returns Status::ErrorSwapchain if swapchain needs recreation
     */
    Status acquire_next_image(VkSemaphore image_available, u32& image_index);
    
    /**
     * Present the current frame
     * Returns Status::ErrorSwapchain if swapchain needs recreation
     */
    Status present(VkQueue present_queue, VkSemaphore render_finished, u32 image_index);
    
    // Accessors
    VkSwapchainKHR handle() const { return swapchain_; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    u32 image_count() const { return static_cast<u32>(images_.size()); }
    
    const std::vector<VkImage>& images() const { return images_; }
    const std::vector<VkImageView>& image_views() const { return image_views_; }
    VkImageView image_view(u32 index) const { return image_views_[index]; }
    
    bool is_valid() const { return swapchain_ != VK_NULL_HANDLE; }
    bool needs_recreation() const { return needs_recreation_; }
    
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_ = {};
    
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    
    SwapchainConfig config_;
    bool needs_recreation_ = false;
    
    // Helper methods
    VkSurfaceFormatKHR choose_surface_format();
    VkPresentModeKHR choose_present_mode();
    VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities);
    void cleanup();
};

} // namespace x360mu
