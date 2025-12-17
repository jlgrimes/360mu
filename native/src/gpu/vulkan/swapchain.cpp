/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Vulkan Swapchain Implementation
 */

#include "swapchain.h"
#include <algorithm>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-swapchain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[SWAPCHAIN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[SWAPCHAIN ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

VulkanSwapchain::~VulkanSwapchain() {
    shutdown();
}

Status VulkanSwapchain::initialize(VkDevice device, VkPhysicalDevice physical_device,
                                    VkSurfaceKHR surface, const SwapchainConfig& config) {
    device_ = device;
    physical_device_ = physical_device;
    surface_ = surface;
    config_ = config;
    
    // Get surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);
    
    // Choose optimal settings
    VkSurfaceFormatKHR surface_format = choose_surface_format();
    VkPresentModeKHR present_mode = choose_present_mode();
    extent_ = choose_extent(capabilities);
    format_ = surface_format.format;
    
    // Determine image count
    u32 image_count = config_.min_image_count;
    if (image_count < capabilities.minImageCount) {
        image_count = capabilities.minImageCount;
    }
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    
    // Create swapchain
    VkSwapchainCreateInfoKHR create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface_;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent_;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = config_.usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = VK_NULL_HANDLE;
    
    VkResult result = vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create swapchain: %d", result);
        return Status::ErrorSwapchain;
    }
    
    // Get swapchain images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, images_.data());
    
    // Create image views
    image_views_.resize(image_count);
    for (u32 i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format_;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        
        result = vkCreateImageView(device_, &view_info, nullptr, &image_views_[i]);
        if (result != VK_SUCCESS) {
            LOGE("Failed to create image view %u: %d", i, result);
            cleanup();
            return Status::ErrorSwapchain;
        }
    }
    
    needs_recreation_ = false;
    
    LOGI("Swapchain created: %ux%u, %u images, format %d, present mode %d",
         extent_.width, extent_.height, image_count, format_, present_mode);
    
    return Status::Ok;
}

void VulkanSwapchain::shutdown() {
    cleanup();
    device_ = VK_NULL_HANDLE;
}

void VulkanSwapchain::cleanup() {
    if (device_ == VK_NULL_HANDLE) return;
    
    for (auto view : image_views_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    image_views_.clear();
    images_.clear();
    
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

Status VulkanSwapchain::recreate(u32 width, u32 height) {
    if (device_ == VK_NULL_HANDLE) return Status::Error;
    
    // Wait for device idle
    vkDeviceWaitIdle(device_);
    
    // Update config
    config_.width = width;
    config_.height = height;
    
    // Store old swapchain for recreation
    VkSwapchainKHR old_swapchain = swapchain_;
    
    // Get surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);
    
    VkSurfaceFormatKHR surface_format = choose_surface_format();
    VkPresentModeKHR present_mode = choose_present_mode();
    extent_ = choose_extent(capabilities);
    format_ = surface_format.format;
    
    u32 image_count = config_.min_image_count;
    if (image_count < capabilities.minImageCount) {
        image_count = capabilities.minImageCount;
    }
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    
    // Create new swapchain
    VkSwapchainCreateInfoKHR create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface_;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent_;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = config_.usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = old_swapchain;
    
    VkResult result = vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_);
    
    // Destroy old resources
    for (auto view : image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
    }
    
    if (result != VK_SUCCESS) {
        LOGE("Failed to recreate swapchain: %d", result);
        swapchain_ = VK_NULL_HANDLE;
        return Status::ErrorSwapchain;
    }
    
    // Get new images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, images_.data());
    
    // Create new image views
    image_views_.resize(image_count);
    for (u32 i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format_;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        
        result = vkCreateImageView(device_, &view_info, nullptr, &image_views_[i]);
        if (result != VK_SUCCESS) {
            LOGE("Failed to create image view %u: %d", i, result);
            return Status::ErrorSwapchain;
        }
    }
    
    needs_recreation_ = false;
    
    LOGI("Swapchain recreated: %ux%u", extent_.width, extent_.height);
    return Status::Ok;
}

Status VulkanSwapchain::acquire_next_image(VkSemaphore image_available, u32& image_index) {
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                            image_available, VK_NULL_HANDLE, &image_index);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_recreation_ = true;
        return Status::ErrorSwapchain;
    } else if (result == VK_SUBOPTIMAL_KHR) {
        needs_recreation_ = true;
        // Continue with current image
    } else if (result != VK_SUCCESS) {
        LOGE("Failed to acquire swapchain image: %d", result);
        return Status::Error;
    }
    
    return Status::Ok;
}

Status VulkanSwapchain::present(VkQueue present_queue, VkSemaphore render_finished, u32 image_index) {
    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &image_index;
    
    VkResult result = vkQueuePresentKHR(present_queue, &present_info);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        needs_recreation_ = true;
        return Status::ErrorSwapchain;
    } else if (result != VK_SUCCESS) {
        LOGE("Failed to present: %d", result);
        return Status::Error;
    }
    
    return Status::Ok;
}

VkSurfaceFormatKHR VulkanSwapchain::choose_surface_format() {
    u32 format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());
    
    // Prefer the configured format
    for (const auto& fmt : formats) {
        if (fmt.format == config_.format && fmt.colorSpace == config_.color_space) {
            return fmt;
        }
    }
    
    // Fallback to sRGB if available
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB && 
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return fmt;
        }
    }
    
    // Just use the first available
    return formats[0];
}

VkPresentModeKHR VulkanSwapchain::choose_present_mode() {
    u32 mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, modes.data());
    
    // Prefer mailbox for low-latency
    for (const auto& mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    
    // Check for configured mode
    for (const auto& mode : modes) {
        if (mode == config_.present_mode) {
            return mode;
        }
    }
    
    // FIFO is always available
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::choose_extent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    
    VkExtent2D extent = {config_.width, config_.height};
    extent.width = std::clamp(extent.width, 
                              capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height,
                               capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height);
    return extent;
}

} // namespace x360mu
