/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Vulkan Rendering Backend
 * 
 * Handles all GPU rendering using Vulkan API on Android.
 * Translates Xenos GPU commands to Vulkan draw calls.
 */

#include "vulkan_backend.h"
#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>

#ifdef __ANDROID__
#include <android/log.h>
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>
#define LOG_TAG "360mu-vulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[VULKAN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[VULKAN ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGW(...) fprintf(stderr, "[VULKAN WARN] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// Vulkan Debug Callback
//=============================================================================

// Always compiled - setup_debug_utils() dynamically probes for the extension
static VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void* user_data)
{
    (void)user_data;

    // Classify message type
    const char* type_str = "GENERAL";
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
        type_str = "VALIDATION";
    else if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
        type_str = "PERFORMANCE";

    // Build object list if any are labeled
    char obj_buf[256] = {};
    if (callback_data->objectCount > 0) {
        int pos = 0;
        pos += snprintf(obj_buf + pos, sizeof(obj_buf) - pos, " [objects:");
        for (u32 i = 0; i < callback_data->objectCount && i < 4; i++) {
            const auto& obj = callback_data->pObjects[i];
            if (obj.pObjectName) {
                pos += snprintf(obj_buf + pos, sizeof(obj_buf) - pos,
                               " %s", obj.pObjectName);
            } else {
                pos += snprintf(obj_buf + pos, sizeof(obj_buf) - pos,
                               " 0x%llx", (unsigned long long)obj.objectHandle);
            }
        }
        snprintf(obj_buf + pos, sizeof(obj_buf) - pos, "]");
    }

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOGE("Vulkan %s [%d]: %s%s", type_str,
             callback_data->messageIdNumber, callback_data->pMessage, obj_buf);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOGI("Vulkan %s WARN [%d]: %s%s", type_str,
             callback_data->messageIdNumber, callback_data->pMessage, obj_buf);
    }
    return VK_FALSE;
}

//=============================================================================
// VulkanBackend Implementation
//=============================================================================

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend() {
    shutdown();
}

Status VulkanBackend::initialize(void* native_window, u32 width, u32 height) {
    width_ = width;
    height_ = height;
    
    // Create Vulkan instance
    VkResult result = create_instance();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create Vulkan instance: %d", result);
        return Status::ErrorInit;
    }

    // Set up debug utils (validation messenger, object naming, debug labels)
    setup_debug_utils();

    // Create surface
    result = create_surface(native_window);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create surface: %d", result);
        return Status::ErrorInit;
    }
    
    // Select physical device and create logical device
    result = create_device();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create device: %d", result);
        return Status::ErrorInit;
    }
    
    // Create swapchain
    result = create_swapchain();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create swapchain: %d", result);
        return Status::ErrorInit;
    }
    
    // Create depth resources
    result = create_depth_resources();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create depth resources: %d", result);
        return Status::ErrorInit;
    }

    // Create render pass
    result = create_render_pass();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create render pass: %d", result);
        return Status::ErrorInit;
    }

    // Create MRT render passes
    result = create_mrt_render_passes();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create MRT render passes: %d", result);
        return Status::ErrorInit;
    }

    // Create framebuffers
    result = create_framebuffers();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create framebuffers: %d", result);
        return Status::ErrorInit;
    }
    
    // Create command pool and buffers
    result = create_command_resources();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create command resources: %d", result);
        return Status::ErrorInit;
    }
    
    // Create synchronization primitives
    result = create_sync_objects();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create sync objects: %d", result);
        return Status::ErrorInit;
    }
    
    // Create descriptor pool and layouts
    result = create_descriptor_resources();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create descriptor resources: %d", result);
        return Status::ErrorInit;
    }
    
    // Create eDRAM emulation buffers
    result = create_edram_resources();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create eDRAM resources: %d", result);
        return Status::ErrorInit;
    }

    // Create Vulkan pipeline cache (accelerates pipeline creation)
    {
        VkPipelineCacheCreateInfo cache_info = {};
        cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        result = vkCreatePipelineCache(device_, &cache_info, nullptr, &vk_pipeline_cache_);
        if (result != VK_SUCCESS) {
            LOGW("Failed to create pipeline cache: %d (non-fatal)", result);
            vk_pipeline_cache_ = VK_NULL_HANDLE;
        }
    }

    LOGI("Vulkan backend initialized (%ux%u)", width, height);
    return Status::Ok;
}

void VulkanBackend::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    
    vkDeviceWaitIdle(device_);
    
    // Destroy query pool
    destroy_query_pool();

    // Destroy memexport SSBO
    destroy_buffer(memexport_buffer_);
    memexport_descriptor_set_ = VK_NULL_HANDLE;

    // Destroy eDRAM resources
    if (edram_buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, edram_buffer_, nullptr);
        vkFreeMemory(device_, edram_memory_, nullptr);
    }
    
    // Destroy descriptor resources
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    }
    for (auto layout : descriptor_set_layouts_) {
        vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    }
    
    // Destroy pipelines
    for (auto& [hash, pipeline] : pipeline_cache_) {
        vkDestroyPipeline(device_, pipeline, nullptr);
    }

    // Destroy Vulkan pipeline cache
    if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, vk_pipeline_cache_, nullptr);
        vk_pipeline_cache_ = VK_NULL_HANDLE;
    }
    
    // Destroy sync objects
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(device_, image_available_semaphores_[i], nullptr);
        vkDestroySemaphore(device_, render_finished_semaphores_[i], nullptr);
        vkDestroyFence(device_, in_flight_fences_[i], nullptr);
    }
    
    // Destroy command resources
    vkDestroyCommandPool(device_, command_pool_, nullptr);
    
    // Destroy framebuffers
    for (auto fb : framebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }

    // Destroy depth resources
    destroy_image(depth_image_);

    // Destroy MRT render passes
    for (u32 i = 0; i < MAX_MRT_TARGETS - 1; i++) {
        if (mrt_render_passes_[i] != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, mrt_render_passes_[i], nullptr);
        }
    }

    // Destroy render pass
    vkDestroyRenderPass(device_, render_pass_, nullptr);
    
    // Destroy swapchain image views
    for (auto view : swapchain_image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    
    // Destroy swapchain
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    
    // Destroy device
    vkDestroyDevice(device_, nullptr);
    
    // Destroy surface
    vkDestroySurfaceKHR(instance_, surface_, nullptr);

    // Destroy debug messenger (must be before instance destruction)
    destroy_debug_utils();

    // Destroy instance
    vkDestroyInstance(instance_, nullptr);
    
    LOGI("Vulkan backend shutdown");
}

VkResult VulkanBackend::create_instance() {
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "360mu";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "360mu";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_1;
    
    // Required extensions
    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef __ANDROID__
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#endif
    };
    
#ifdef X360MU_VULKAN_DEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
#endif
    
    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
#ifdef X360MU_VULKAN_DEBUG
    create_info.enabledLayerCount = 1;
    create_info.ppEnabledLayerNames = layers;
#endif
    
    return vkCreateInstance(&create_info, nullptr, &instance_);
}

VkResult VulkanBackend::create_surface(void* native_window) {
#ifdef __ANDROID__
    VkAndroidSurfaceCreateInfoKHR create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    create_info.window = static_cast<ANativeWindow*>(native_window);
    return vkCreateAndroidSurfaceKHR(instance_, &create_info, nullptr, &surface_);
#else
    return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
}

VkResult VulkanBackend::create_device() {
    // Enumerate physical devices
    u32 device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) {
        LOGE("No Vulkan devices found");
        return VK_ERROR_DEVICE_LOST;
    }
    
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    
    // Select best device (prefer discrete GPU)
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        
        physical_device_ = device;
        
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            break;
        }
    }
    
    // Find queue families
    u32 queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());
    
    graphics_queue_family_ = UINT32_MAX;
    for (u32 i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present_support);
            if (present_support) {
                graphics_queue_family_ = i;
                break;
            }
        }
    }
    
    if (graphics_queue_family_ == UINT32_MAX) {
        LOGE("No suitable queue family found");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    
    // Create logical device
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info = {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = graphics_queue_family_;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;
    
    VkPhysicalDeviceFeatures features = {};
    features.samplerAnisotropy = VK_TRUE;
    features.fillModeNonSolid = VK_TRUE;
    
    const char* device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    
    VkDeviceCreateInfo device_create_info = {};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;
    device_create_info.enabledExtensionCount = 1;
    device_create_info.ppEnabledExtensionNames = device_extensions;
    device_create_info.pEnabledFeatures = &features;
    
    VkResult result = vkCreateDevice(physical_device_, &device_create_info, nullptr, &device_);
    if (result != VK_SUCCESS) return result;
    
    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
    
    return VK_SUCCESS;
}

VkResult VulkanBackend::create_swapchain() {
    // Get surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);
    
    // Get surface formats
    u32 format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());
    
    // Choose format
    VkSurfaceFormatKHR surface_format = formats[0];
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB && 
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = fmt;
            break;
        }
    }
    swapchain_format_ = surface_format.format;
    
    // Choose extent
    VkExtent2D extent;
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp(width_, capabilities.minImageExtent.width, 
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(height_, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }
    swapchain_extent_ = extent;
    
    // Image count
    u32 image_count = capabilities.minImageCount + 1;
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
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode_;
    create_info.clipped = VK_TRUE;
    
    VkResult result = vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_);
    if (result != VK_SUCCESS) return result;
    
    // Get swapchain images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());
    
    // Create image views
    swapchain_image_views_.resize(image_count);
    for (size_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = swapchain_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain_format_;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        
        result = vkCreateImageView(device_, &view_info, nullptr, &swapchain_image_views_[i]);
        if (result != VK_SUCCESS) return result;

        char sc_name[48];
        snprintf(sc_name, sizeof(sc_name), "Swapchain_Image_%zu", i);
        set_object_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(swapchain_images_[i]), sc_name);
    }

    return VK_SUCCESS;
}

VkFormat VulkanBackend::find_depth_format() {
    VkFormat candidates[] = {
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D32_SFLOAT
    };
    for (auto format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_device_, format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }
    return VK_FORMAT_D32_SFLOAT;
}

VkResult VulkanBackend::create_depth_resources() {
    depth_format_ = find_depth_format();
    depth_image_ = create_image(swapchain_extent_.width, swapchain_extent_.height,
                                depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if (depth_image_.image == VK_NULL_HANDLE) {
        LOGE("Failed to create depth image");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LOGD("Depth resources created: format=%d", depth_format_);
    return VK_SUCCESS;
}

VkResult VulkanBackend::create_render_pass() {
    // Attachments: color + depth
    VkAttachmentDescription attachments[2] = {};

    // Color attachment
    attachments[0].format = swapchain_format_;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Depth attachment
    attachments[1].format = depth_format_;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = 2;
    create_info.pAttachments = attachments;
    create_info.subpassCount = 1;
    create_info.pSubpasses = &subpass;
    create_info.dependencyCount = 1;
    create_info.pDependencies = &dependency;

    VkResult result = vkCreateRenderPass(device_, &create_info, nullptr, &render_pass_);
    if (result == VK_SUCCESS) {
        set_object_name(VK_OBJECT_TYPE_RENDER_PASS,
                        reinterpret_cast<u64>(render_pass_),
                        "RenderPass_Main_1Color+Depth");
    }
    return result;
}

VkResult VulkanBackend::create_mrt_render_passes() {
    // Create render passes for 2, 3, 4 color attachments + depth
    for (u32 n = 2; n <= MAX_MRT_TARGETS; n++) {
        u32 total_attachments = n + 1;  // n color + 1 depth
        std::vector<VkAttachmentDescription> att(total_attachments);
        std::vector<VkAttachmentReference> color_refs(n);

        // Color attachments (offscreen MRT targets)
        for (u32 i = 0; i < n; i++) {
            att[i] = {};
            att[i].format = swapchain_format_;
            att[i].samples = VK_SAMPLE_COUNT_1_BIT;
            att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            att[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            color_refs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }

        // Depth attachment (last)
        att[n] = {};
        att[n].format = depth_format_;
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depth_ref = {n, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = n;
        subpass.pColorAttachments = color_refs.data();
        subpass.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        create_info.attachmentCount = total_attachments;
        create_info.pAttachments = att.data();
        create_info.subpassCount = 1;
        create_info.pSubpasses = &subpass;
        create_info.dependencyCount = 1;
        create_info.pDependencies = &dependency;

        VkResult result = vkCreateRenderPass(device_, &create_info, nullptr,
                                              &mrt_render_passes_[n - 2]);
        if (result != VK_SUCCESS) return result;

        char rp_name[64];
        snprintf(rp_name, sizeof(rp_name), "RenderPass_MRT_%uColor+Depth", n);
        set_object_name(VK_OBJECT_TYPE_RENDER_PASS,
                        reinterpret_cast<u64>(mrt_render_passes_[n - 2]),
                        rp_name);
    }

    LOGD("MRT render passes created (2-4 color attachments)");
    return VK_SUCCESS;
}

VkResult VulkanBackend::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());

    for (size_t i = 0; i < swapchain_image_views_.size(); i++) {
        VkImageView fb_attachments[] = {
            swapchain_image_views_[i],
            depth_image_.view
        };

        VkFramebufferCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        create_info.renderPass = render_pass_;
        create_info.attachmentCount = 2;
        create_info.pAttachments = fb_attachments;
        create_info.width = swapchain_extent_.width;
        create_info.height = swapchain_extent_.height;
        create_info.layers = 1;

        VkResult result = vkCreateFramebuffer(device_, &create_info, nullptr, &framebuffers_[i]);
        if (result != VK_SUCCESS) return result;
    }

    return VK_SUCCESS;
}

VkResult VulkanBackend::create_command_resources() {
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = graphics_queue_family_;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    
    VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
    if (result != VK_SUCCESS) return result;
    
    command_buffers_.resize(MAX_FRAMES_IN_FLIGHT);
    
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    
    return vkAllocateCommandBuffers(device_, &alloc_info, command_buffers_.data());
}

VkResult VulkanBackend::create_sync_objects() {
    image_available_semaphores_.resize(MAX_FRAMES_IN_FLIGHT);
    render_finished_semaphores_.resize(MAX_FRAMES_IN_FLIGHT);
    in_flight_fences_.resize(MAX_FRAMES_IN_FLIGHT);
    
    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkResult result = vkCreateSemaphore(device_, &semaphore_info, nullptr, 
                                            &image_available_semaphores_[i]);
        if (result != VK_SUCCESS) return result;
        
        result = vkCreateSemaphore(device_, &semaphore_info, nullptr,
                                   &render_finished_semaphores_[i]);
        if (result != VK_SUCCESS) return result;
        
        result = vkCreateFence(device_, &fence_info, nullptr, &in_flight_fences_[i]);
        if (result != VK_SUCCESS) return result;
    }
    
    return VK_SUCCESS;
}

VkResult VulkanBackend::create_descriptor_resources() {
    // Set 0: Uniform buffers for Xbox 360 shader constants
    VkDescriptorSetLayoutBinding uniform_bindings[4] = {};

    // Vertex constants (256 vec4)
    uniform_bindings[0].binding = 0;
    uniform_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniform_bindings[0].descriptorCount = 1;
    uniform_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    // Pixel constants (256 vec4)
    uniform_bindings[1].binding = 1;
    uniform_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniform_bindings[1].descriptorCount = 1;
    uniform_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Bool constants
    uniform_bindings[2].binding = 2;
    uniform_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniform_bindings[2].descriptorCount = 1;
    uniform_bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // Loop constants
    uniform_bindings[3].binding = 3;
    uniform_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniform_bindings[3].descriptorCount = 1;
    uniform_bindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo uniform_layout_info = {};
    uniform_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    uniform_layout_info.bindingCount = 4;
    uniform_layout_info.pBindings = uniform_bindings;

    VkDescriptorSetLayout uniform_layout;
    VkResult result = vkCreateDescriptorSetLayout(device_, &uniform_layout_info, nullptr, &uniform_layout);
    if (result != VK_SUCCESS) return result;
    descriptor_set_layouts_.push_back(uniform_layout);

    // Set 1: Combined image samplers (16 texture units)
    VkDescriptorSetLayoutBinding sampler_bindings[16] = {};
    for (u32 i = 0; i < 16; i++) {
        sampler_bindings[i].binding = i;
        sampler_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_bindings[i].descriptorCount = 1;
        sampler_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo sampler_layout_info = {};
    sampler_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sampler_layout_info.bindingCount = 16;
    sampler_layout_info.pBindings = sampler_bindings;

    VkDescriptorSetLayout sampler_layout;
    result = vkCreateDescriptorSetLayout(device_, &sampler_layout_info, nullptr, &sampler_layout);
    if (result != VK_SUCCESS) return result;
    descriptor_set_layouts_.push_back(sampler_layout);

    // Set 2: Storage buffer for memexport (SSBO)
    VkDescriptorSetLayoutBinding ssbo_binding{};
    ssbo_binding.binding = 0;
    ssbo_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ssbo_binding.descriptorCount = 1;
    ssbo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo ssbo_layout_info{};
    ssbo_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ssbo_layout_info.bindingCount = 1;
    ssbo_layout_info.pBindings = &ssbo_binding;

    VkDescriptorSetLayout ssbo_layout;
    result = vkCreateDescriptorSetLayout(device_, &ssbo_layout_info, nullptr, &ssbo_layout);
    if (result != VK_SUCCESS) return result;
    descriptor_set_layouts_.push_back(ssbo_layout);

    // Create pipeline layout with all descriptor set layouts
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = static_cast<u32>(descriptor_set_layouts_.size());
    pipeline_layout_info.pSetLayouts = descriptor_set_layouts_.data();

    result = vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &pipeline_layout_);
    if (result != VK_SUCCESS) return result;

    // Create descriptor pool
    VkDescriptorPoolSize pool_sizes[3] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 256;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 1024;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[2].descriptorCount = 16;

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    pool_info.maxSets = 256;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    return vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_);
}

VkResult VulkanBackend::create_edram_resources() {
    // Xbox 360 has 10MB of embedded DRAM (eDRAM) for render targets
    constexpr u32 EDRAM_SIZE = 10 * 1024 * 1024;
    
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = EDRAM_SIZE;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult result = vkCreateBuffer(device_, &buffer_info, nullptr, &edram_buffer_);
    if (result != VK_SUCCESS) return result;
    
    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device_, edram_buffer_, &mem_reqs);
    
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    
    u32 memory_type_index = UINT32_MAX;
    for (u32 i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_reqs.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memory_type_index = i;
            break;
        }
    }
    
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = memory_type_index;
    
    result = vkAllocateMemory(device_, &alloc_info, nullptr, &edram_memory_);
    if (result != VK_SUCCESS) return result;
    
    VkResult bind_result = vkBindBufferMemory(device_, edram_buffer_, edram_memory_, 0);
    if (bind_result == VK_SUCCESS) {
        set_object_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(edram_buffer_), "eDRAM_10MB");
    }
    return bind_result;
}

//=============================================================================
// Rendering
//=============================================================================

Status VulkanBackend::begin_frame() {
    // Check if swapchain is initialized
    if (swapchain_ == VK_NULL_HANDLE) {
        return Status::ErrorSwapchain;
    }

    // Recreate swapchain if present mode changed
    if (swapchain_needs_recreation_) {
        vkDeviceWaitIdle(device_);
        cleanup_swapchain();
        create_swapchain();
        create_depth_resources();
        create_framebuffers();
        swapchain_needs_recreation_ = false;
        LOGI("Swapchain recreated with present mode %d", present_mode_);
    }

    // Wait for previous frame
    vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);

    // Acquire next image
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                            image_available_semaphores_[current_frame_],
                                            VK_NULL_HANDLE, &current_image_index_);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Need to recreate swapchain
        return Status::ErrorSwapchain;
    }
    
    vkResetFences(device_, 1, &in_flight_fences_[current_frame_]);
    
    // Begin command buffer
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);
    
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin_info);

    // Debug label: frame boundary (visible in RenderDoc/AGI)
    frame_number_++;
    char frame_label[64];
    snprintf(frame_label, sizeof(frame_label), "Frame %llu", (unsigned long long)frame_number_);
    cmd_begin_label(frame_label, 0.2f, 0.8f, 0.2f);

    // Begin render pass
    VkRenderPassBeginInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass = render_pass_;
    rp_info.framebuffer = framebuffers_[current_image_index_];
    rp_info.renderArea.offset = {0, 0};
    rp_info.renderArea.extent = swapchain_extent_;
    
    VkClearValue clear_values[2] = {};
    clear_values[0].color = {{0.2f, 0.0f, 0.3f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};
    rp_info.clearValueCount = 2;
    rp_info.pClearValues = clear_values;
    
    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    
    // Set viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchain_extent_.width);
    viewport.height = static_cast<float>(swapchain_extent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = swapchain_extent_;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    
    return Status::Ok;
}

Status VulkanBackend::end_frame() {
    VkCommandBuffer cmd = command_buffers_[current_frame_];

    vkCmdEndRenderPass(cmd);

    // Close frame debug label
    cmd_end_label();

    cmd_insert_label("Present", 0.8f, 0.8f, 0.2f);

    vkEndCommandBuffer(cmd);
    
    // Submit
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    
    VkSemaphore wait_semaphores[] = {image_available_semaphores_[current_frame_]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    
    VkSemaphore signal_semaphores[] = {render_finished_semaphores_[current_frame_]};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;
    
    vkQueueSubmit(graphics_queue_, 1, &submit_info, in_flight_fences_[current_frame_]);
    
    // Present
    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &current_image_index_;
    
    VkResult result = vkQueuePresentKHR(graphics_queue_, &present_info);
    
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return Status::ErrorSwapchain;
    }
    
    return Status::Ok;
}

VkPipeline VulkanBackend::get_or_create_pipeline(const PipelineState& state,
                                                   VkShaderModule vertex_shader,
                                                   VkShaderModule fragment_shader) {
    // Check cache
    u64 hash = state.compute_hash();
    auto it = pipeline_cache_.find(hash);
    if (it != pipeline_cache_.end()) {
        return it->second;
    }
    
    // Create new pipeline
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    
    // Vertex input from Xbox 360 fetch constants
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = state.vertex_input.binding_count;
    vertex_input.pVertexBindingDescriptions = state.vertex_input.binding_count > 0 ?
        state.vertex_input.bindings : nullptr;
    vertex_input.vertexAttributeDescriptionCount = state.vertex_input.attribute_count;
    vertex_input.pVertexAttributeDescriptions = state.vertex_input.attribute_count > 0 ?
        state.vertex_input.attributes : nullptr;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = state.primitive_topology;
    input_assembly.primitiveRestartEnable = VK_FALSE;
    
    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    
    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = state.polygon_mode;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = state.cull_mode;
    rasterizer.frontFace = state.front_face;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = state.depth_test_enable;
    depth_stencil.depthWriteEnable = state.depth_write_enable;
    depth_stencil.depthCompareOp = state.depth_compare_op;
    depth_stencil.stencilTestEnable = state.stencil_test_enable;
    
    // MRT support: create blend attachment for each color target
    std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(state.color_attachment_count);
    for (u32 i = 0; i < state.color_attachment_count; i++) {
        blend_attachments[i] = {};
        blend_attachments[i].colorWriteMask = state.color_write_mask;
        blend_attachments[i].blendEnable = state.blend_enable;
        blend_attachments[i].srcColorBlendFactor = state.src_color_blend;
        blend_attachments[i].dstColorBlendFactor = state.dst_color_blend;
        blend_attachments[i].colorBlendOp = state.color_blend_op;
        blend_attachments[i].srcAlphaBlendFactor = state.src_alpha_blend;
        blend_attachments[i].dstAlphaBlendFactor = state.dst_alpha_blend;
        blend_attachments[i].alphaBlendOp = state.alpha_blend_op;
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.attachmentCount = state.color_attachment_count;
    color_blending.pAttachments = blend_attachments.data();
    
    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    
    VkPipelineDynamicStateCreateInfo dynamic_state = {};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;
    
    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = active_pipeline_layout();
    pipeline_info.renderPass = get_render_pass(state.color_attachment_count);
    pipeline_info.subpass = 0;
    
    VkPipeline pipeline;
    VkResult result = vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &pipeline_info,
                                                 nullptr, &pipeline);
    
    if (result == VK_SUCCESS) {
        pipeline_cache_[hash] = pipeline;

        // Label pipeline for debugger (hash identifies shader combination)
        char name[64];
        snprintf(name, sizeof(name), "Pipeline %016llx", (unsigned long long)hash);
        set_object_name(VK_OBJECT_TYPE_PIPELINE,
                       reinterpret_cast<u64>(pipeline), name);

        return pipeline;
    }

    LOGE("Failed to create pipeline: %d", result);
    return VK_NULL_HANDLE;
}

VkShaderModule VulkanBackend::create_shader_module(const std::vector<u32>& spirv) {
    VkShaderModuleCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = spirv.size() * sizeof(u32);
    create_info.pCode = spirv.data();
    
    VkShaderModule module;
    VkResult result = vkCreateShaderModule(device_, &create_info, nullptr, &module);
    
    if (result != VK_SUCCESS) {
        LOGE("Failed to create shader module: %d", result);
        return VK_NULL_HANDLE;
    }
    
    return module;
}

void VulkanBackend::destroy_shader_module(VkShaderModule module) {
    vkDestroyShaderModule(device_, module, nullptr);
}

//=============================================================================
// Draw Commands
//=============================================================================

void VulkanBackend::draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdDraw(cmd, vertex_count, instance_count, first_vertex, first_instance);
}

void VulkanBackend::draw_indexed(u32 index_count, u32 instance_count, u32 first_index,
                                  s32 vertex_offset, u32 first_instance) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdDrawIndexed(cmd, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VulkanBackend::bind_pipeline(VkPipeline pipeline) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
}

void VulkanBackend::bind_vertex_buffer(VkBuffer buffer, VkDeviceSize offset) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
}

void VulkanBackend::bind_vertex_buffers(u32 first_binding, u32 count,
                                         const VkBuffer* buffers, const VkDeviceSize* offsets) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdBindVertexBuffers(cmd, first_binding, count, buffers, offsets);
}

void VulkanBackend::bind_index_buffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdBindIndexBuffer(cmd, buffer, offset, type);
}

void VulkanBackend::bind_descriptor_set(VkDescriptorSet set, u32 first_set) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, active_pipeline_layout(), first_set, 1, &set, 0, nullptr);
}

void VulkanBackend::set_viewport(float x, float y, float w, float h,
                                  float min_depth, float max_depth) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    VkViewport viewport = {x, y, w, h, min_depth, max_depth};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
}

void VulkanBackend::set_scissor(s32 x, s32 y, u32 w, u32 h) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    VkRect2D scissor = {{x, y}, {w, h}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

VkRenderPass VulkanBackend::get_render_pass(u32 color_attachment_count) const {
    if (color_attachment_count <= 1) return render_pass_;
    if (color_attachment_count > MAX_MRT_TARGETS) return render_pass_;
    return mrt_render_passes_[color_attachment_count - 2];
}

//=============================================================================
// Memory Management
//=============================================================================

u32 VulkanBackend::find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    
    for (u32 i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    LOGE("Failed to find suitable memory type");
    return UINT32_MAX;
}

VulkanBuffer VulkanBackend::create_buffer(u64 size, VkBufferUsageFlags usage,
                                           VkMemoryPropertyFlags properties) {
    VulkanBuffer buffer;
    buffer.size = size;
    
    // Create buffer
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult result = vkCreateBuffer(device_, &buffer_info, nullptr, &buffer.buffer);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create buffer: %d", result);
        return buffer;
    }
    
    // Get memory requirements
    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device_, buffer.buffer, &mem_reqs);
    
    // Allocate memory
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, properties);
    
    if (alloc_info.memoryTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
        return buffer;
    }
    
    result = vkAllocateMemory(device_, &alloc_info, nullptr, &buffer.memory);
    if (result != VK_SUCCESS) {
        LOGE("Failed to allocate buffer memory: %d", result);
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
        return buffer;
    }
    
    // Bind memory
    vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0);
    
    // Map memory if host-visible
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        vkMapMemory(device_, buffer.memory, 0, size, 0, &buffer.mapped);
    }
    
    // Label buffer for GPU captures
    const char* usage_str = "Buffer";
    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) usage_str = "VertexBuffer";
    else if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) usage_str = "IndexBuffer";
    else if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) usage_str = "UniformBuffer";
    else if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) usage_str = "StorageBuffer";
    else if (usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) usage_str = "StagingBuffer";
    char buf_name[64];
    snprintf(buf_name, sizeof(buf_name), "%s_%lluB", usage_str, (unsigned long long)size);
    set_object_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(buffer.buffer), buf_name);

    LOGD("Created buffer: size=%llu, usage=%u", (unsigned long long)size, usage);
    return buffer;
}

void VulkanBackend::destroy_buffer(VulkanBuffer& buffer) {
    if (buffer.buffer == VK_NULL_HANDLE) return;
    
    if (buffer.mapped) {
        vkUnmapMemory(device_, buffer.memory);
        buffer.mapped = nullptr;
    }
    
    vkDestroyBuffer(device_, buffer.buffer, nullptr);
    vkFreeMemory(device_, buffer.memory, nullptr);
    
    buffer.buffer = VK_NULL_HANDLE;
    buffer.memory = VK_NULL_HANDLE;
    buffer.size = 0;
}

VkDescriptorSet VulkanBackend::get_memexport_descriptor_set() {
    // Lazy init: create buffer and descriptor set on first use
    if (memexport_descriptor_set_ != VK_NULL_HANDLE) {
        return memexport_descriptor_set_;
    }

    // Create SSBO buffer for memexport writes
    memexport_buffer_ = create_buffer(
        MEMEXPORT_BUFFER_SIZE,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (memexport_buffer_.buffer == VK_NULL_HANDLE) {
        LOGE("Failed to create memexport SSBO buffer");
        return VK_NULL_HANDLE;
    }

    // Allocate descriptor set from set layout index 2 (SSBO layout)
    if (descriptor_set_layouts_.size() < 3) {
        LOGE("SSBO descriptor set layout not available (need set index 2)");
        return VK_NULL_HANDLE;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_set_layouts_[2];

    if (vkAllocateDescriptorSets(device_, &alloc_info, &memexport_descriptor_set_) != VK_SUCCESS) {
        LOGE("Failed to allocate memexport descriptor set");
        return VK_NULL_HANDLE;
    }

    // Write SSBO binding
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = memexport_buffer_.buffer;
    buffer_info.offset = 0;
    buffer_info.range = MEMEXPORT_BUFFER_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = memexport_descriptor_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    LOGI("Memexport SSBO initialized (%llu bytes)", (unsigned long long)MEMEXPORT_BUFFER_SIZE);
    return memexport_descriptor_set_;
}

VulkanImage VulkanBackend::create_image(u32 width, u32 height, VkFormat format,
                                         VkImageUsageFlags usage) {
    VulkanImage image;
    image.width = width;
    image.height = height;
    image.format = format;
    
    // Create image
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    
    VkResult result = vkCreateImage(device_, &image_info, nullptr, &image.image);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create image: %d", result);
        return image;
    }
    
    // Get memory requirements
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device_, image.image, &mem_reqs);
    
    // Allocate memory
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, 
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (alloc_info.memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device_, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
        return image;
    }
    
    result = vkAllocateMemory(device_, &alloc_info, nullptr, &image.memory);
    if (result != VK_SUCCESS) {
        LOGE("Failed to allocate image memory: %d", result);
        vkDestroyImage(device_, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
        return image;
    }
    
    // Bind memory
    vkBindImageMemory(device_, image.image, image.memory, 0);
    
    // Create image view
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    
    // Adjust aspect mask for depth formats
    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
        format == VK_FORMAT_D16_UNORM) {
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    
    result = vkCreateImageView(device_, &view_info, nullptr, &image.view);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create image view: %d", result);
        vkDestroyImage(device_, image.image, nullptr);
        vkFreeMemory(device_, image.memory, nullptr);
        image.image = VK_NULL_HANDLE;
        image.memory = VK_NULL_HANDLE;
        return image;
    }
    
    // Label image for GPU captures
    char img_name[64];
    snprintf(img_name, sizeof(img_name), "Image_%ux%u_fmt%d", width, height, format);
    set_object_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(image.image), img_name);

    LOGD("Created image: %ux%u, format=%d", width, height, format);
    return image;
}

void VulkanBackend::destroy_image(VulkanImage& image) {
    if (image.image == VK_NULL_HANDLE) return;
    
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
    }
    vkDestroyImage(device_, image.image, nullptr);
    vkFreeMemory(device_, image.memory, nullptr);
    
    image.image = VK_NULL_HANDLE;
    image.memory = VK_NULL_HANDLE;
    image.view = VK_NULL_HANDLE;
    image.width = 0;
    image.height = 0;
}

void VulkanBackend::upload_to_buffer(VulkanBuffer& buffer, const void* data, size_t size) {
    if (buffer.mapped) {
        // Buffer is host-visible, direct copy
        memcpy(buffer.mapped, data, size);
        
        // Flush if not coherent
        VkMappedMemoryRange range = {};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device_, 1, &range);
    } else {
        // Need staging buffer
        VulkanBuffer staging = create_buffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        memcpy(staging.mapped, data, size);
        
        // Copy via command buffer
        VkCommandBufferAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandPool = command_pool_;
        alloc_info.commandBufferCount = 1;
        
        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(device_, &alloc_info, &cmd);
        
        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin_info);
        
        VkBufferCopy copy_region = {};
        copy_region.size = size;
        vkCmdCopyBuffer(cmd, staging.buffer, buffer.buffer, 1, &copy_region);
        
        vkEndCommandBuffer(cmd);
        
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd;
        
        vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphics_queue_);
        
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        destroy_buffer(staging);
    }
}

void VulkanBackend::upload_to_image(VulkanImage& image, const void* data, size_t size) {
    // Create staging buffer
    VulkanBuffer staging = create_buffer(size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    memcpy(staging.mapped, data, size);
    
    // Create command buffer for transfer
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool_;
    alloc_info.commandBufferCount = 1;
    
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc_info, &cmd);
    
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);
    
    // Transition to transfer destination
    transition_image_layout(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, 
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    // Copy buffer to image
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {image.width, image.height, 1};
    
    vkCmdCopyBufferToImage(cmd, staging.buffer, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // Transition to shader read
    transition_image_layout(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    vkEndCommandBuffer(cmd);
    
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    
    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    destroy_buffer(staging);
}

//=============================================================================
// Image Layout Transitions
//=============================================================================

void VulkanBackend::transition_image_layout(VkImage image, VkImageLayout old_layout, 
                                             VkImageLayout new_layout) {
    // Create one-time command buffer
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool_;
    alloc_info.commandBufferCount = 1;
    
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc_info, &cmd);
    
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);
    
    transition_image_layout(cmd, image, old_layout, new_layout);
    
    vkEndCommandBuffer(cmd);
    
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    
    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

void VulkanBackend::transition_image_layout(VkCommandBuffer cmd, VkImage image, 
                                             VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    
    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;
    
    // Determine access masks and stages based on layouts
    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && 
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && 
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && 
               new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && 
               new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && 
               new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    } else {
        // Generic transition
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 
                         0, nullptr, 0, nullptr, 1, &barrier);
}

//=============================================================================
// Swapchain Management
//=============================================================================

void VulkanBackend::cleanup_swapchain() {
    // Destroy framebuffers
    for (auto fb : framebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();

    // Destroy depth resources
    destroy_image(depth_image_);

    // Destroy image views
    for (auto view : swapchain_image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_image_views_.clear();
    
    // Destroy swapchain
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    
    swapchain_images_.clear();
}

Status VulkanBackend::resize(u32 width, u32 height) {
    if (device_ == VK_NULL_HANDLE) return Status::Error;

    // Wait for device idle
    vkDeviceWaitIdle(device_);

    // 0,0 means recreate at current dimensions (swapchain error recovery)
    if (width > 0) width_ = width;
    if (height > 0) height_ = height;
    
    // Cleanup old swapchain resources
    cleanup_swapchain();
    
    // Recreate swapchain
    VkResult result = create_swapchain();
    if (result != VK_SUCCESS) {
        LOGE("Failed to recreate swapchain: %d", result);
        return Status::ErrorSwapchain;
    }

    // Recreate depth resources
    result = create_depth_resources();
    if (result != VK_SUCCESS) {
        LOGE("Failed to recreate depth resources: %d", result);
        return Status::ErrorSwapchain;
    }

    // Recreate framebuffers
    result = create_framebuffers();
    if (result != VK_SUCCESS) {
        LOGE("Failed to recreate framebuffers: %d", result);
        return Status::ErrorSwapchain;
    }
    
    LOGI("Swapchain resized to %ux%u", width, height);
    return Status::Ok;
}

//=============================================================================
// Test/Debug Functions
//=============================================================================

void VulkanBackend::set_present_mode(VkPresentModeKHR mode) {
    if (present_mode_ != mode) {
        present_mode_ = mode;
        swapchain_needs_recreation_ = true;
        LOGI("Present mode changed to %d, swapchain will be recreated", mode);
    }
}

void VulkanBackend::clear_screen(float r, float g, float b) {
    // Wait for previous frame
    vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);
    
    // Acquire next image
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                            image_available_semaphores_[current_frame_],
                                            VK_NULL_HANDLE, &current_image_index_);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        LOGE("Swapchain out of date in clear_screen");
        return;
    }
    
    vkResetFences(device_, 1, &in_flight_fences_[current_frame_]);
    
    // Begin command buffer
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);
    
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin_info);
    
    // Clear color value
    VkClearColorValue clear_color = {{r, g, b, 1.0f}};
    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    
    // Transition image to TRANSFER_DST
    transition_image_layout(cmd, swapchain_images_[current_image_index_],
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    // Clear the image
    vkCmdClearColorImage(cmd, swapchain_images_[current_image_index_],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &clear_color, 1, &range);
    
    // Transition to PRESENT
    transition_image_layout(cmd, swapchain_images_[current_image_index_],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    
    vkEndCommandBuffer(cmd);
    
    // Submit
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    
    VkSemaphore wait_semaphores[] = {image_available_semaphores_[current_frame_]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_TRANSFER_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    
    VkSemaphore signal_semaphores[] = {render_finished_semaphores_[current_frame_]};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;
    
    vkQueueSubmit(graphics_queue_, 1, &submit_info, in_flight_fences_[current_frame_]);
    
    // Present
    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &current_image_index_;
    
    vkQueuePresentKHR(graphics_queue_, &present_info);
    
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

//=============================================================================
// Occlusion Query Management
//=============================================================================

Status VulkanBackend::create_query_pool(u32 max_queries) {
    if (device_ == VK_NULL_HANDLE) return Status::Error;
    if (query_pool_ != VK_NULL_HANDLE) destroy_query_pool();

    VkQueryPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
    pool_info.queryCount = max_queries;

    VkResult result = vkCreateQueryPool(device_, &pool_info, nullptr, &query_pool_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create occlusion query pool: %d", result);
        return Status::Error;
    }

    query_pool_size_ = max_queries;
    LOGI("Created occlusion query pool with %u queries", max_queries);

    // Create result buffer for conditional rendering fallback
    query_result_buffer_ = create_buffer(
        max_queries * sizeof(u64),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Probe for VK_EXT_conditional_rendering
    pfn_begin_conditional_ = reinterpret_cast<PFN_vkCmdBeginConditionalRenderingEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdBeginConditionalRenderingEXT"));
    pfn_end_conditional_ = reinterpret_cast<PFN_vkCmdEndConditionalRenderingEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdEndConditionalRenderingEXT"));
    has_conditional_rendering_ext_ = (pfn_begin_conditional_ != nullptr &&
                                       pfn_end_conditional_ != nullptr);

    if (has_conditional_rendering_ext_) {
        LOGI("VK_EXT_conditional_rendering available");
    } else {
        LOGI("VK_EXT_conditional_rendering not available, using CPU fallback");
    }

    return Status::Ok;
}

void VulkanBackend::destroy_query_pool() {
    if (device_ == VK_NULL_HANDLE) return;
    if (query_pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, query_pool_, nullptr);
        query_pool_ = VK_NULL_HANDLE;
        query_pool_size_ = 0;
    }
    if (query_result_buffer_.buffer != VK_NULL_HANDLE) {
        destroy_buffer(query_result_buffer_);
    }
}

void VulkanBackend::begin_occlusion_query(u32 query_index) {
    if (query_pool_ == VK_NULL_HANDLE || query_index >= query_pool_size_) return;
    VkCommandBuffer cmd = current_command_buffer();
    if (cmd == VK_NULL_HANDLE) return;

    vkCmdBeginQuery(cmd, query_pool_, query_index, VK_QUERY_CONTROL_PRECISE_BIT);
}

void VulkanBackend::end_occlusion_query(u32 query_index) {
    if (query_pool_ == VK_NULL_HANDLE || query_index >= query_pool_size_) return;
    VkCommandBuffer cmd = current_command_buffer();
    if (cmd == VK_NULL_HANDLE) return;

    vkCmdEndQuery(cmd, query_pool_, query_index);
}

void VulkanBackend::reset_queries(u32 first_query, u32 count) {
    if (query_pool_ == VK_NULL_HANDLE) return;
    if (first_query + count > query_pool_size_) {
        count = query_pool_size_ - first_query;
    }
    VkCommandBuffer cmd = current_command_buffer();
    if (cmd == VK_NULL_HANDLE) return;

    vkCmdResetQueryPool(cmd, query_pool_, first_query, count);
}

bool VulkanBackend::get_query_result(u32 query_index, u64& result) {
    if (query_pool_ == VK_NULL_HANDLE || query_index >= query_pool_size_) return false;

    VkResult vk_result = vkGetQueryPoolResults(
        device_, query_pool_, query_index, 1,
        sizeof(u64), &result, sizeof(u64),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    return vk_result == VK_SUCCESS;
}

void VulkanBackend::begin_conditional_rendering(u32 query_index, bool inverted) {
    if (query_pool_ == VK_NULL_HANDLE || query_index >= query_pool_size_) return;
    VkCommandBuffer cmd = current_command_buffer();
    if (cmd == VK_NULL_HANDLE) return;

    if (has_conditional_rendering_ext_ && query_result_buffer_.buffer != VK_NULL_HANDLE) {
        // Copy query result to the conditional rendering buffer
        vkCmdCopyQueryPoolResults(cmd, query_pool_, query_index, 1,
                                   query_result_buffer_.buffer,
                                   query_index * sizeof(u64),
                                   sizeof(u64),
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        // Memory barrier: transfer write → conditional read
        VkBufferMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;
        barrier.buffer = query_result_buffer_.buffer;
        barrier.offset = query_index * sizeof(u64);
        barrier.size = sizeof(u64);
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);

        VkConditionalRenderingBeginInfoEXT cond_info = {};
        cond_info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
        cond_info.buffer = query_result_buffer_.buffer;
        cond_info.offset = query_index * sizeof(u64);
        if (inverted) {
            cond_info.flags = VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT;
        }
        pfn_begin_conditional_(cmd, &cond_info);
    }
    // CPU fallback is handled by CommandProcessor checking query results directly
}

void VulkanBackend::end_conditional_rendering() {
    if (!has_conditional_rendering_ext_) return;
    VkCommandBuffer cmd = current_command_buffer();
    if (cmd == VK_NULL_HANDLE) return;

    pfn_end_conditional_(cmd);
}

bool VulkanBackend::save_pipeline_cache(const std::string& path) {
    if (device_ == VK_NULL_HANDLE || vk_pipeline_cache_ == VK_NULL_HANDLE) {
        return false;
    }

    size_t data_size = 0;
    VkResult result = vkGetPipelineCacheData(device_, vk_pipeline_cache_, &data_size, nullptr);
    if (result != VK_SUCCESS || data_size == 0) {
        LOGW("Failed to get pipeline cache size: %d", result);
        return false;
    }

    std::vector<u8> data(data_size);
    result = vkGetPipelineCacheData(device_, vk_pipeline_cache_, &data_size, data.data());
    if (result != VK_SUCCESS) {
        LOGW("Failed to get pipeline cache data: %d", result);
        return false;
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        LOGW("Failed to open pipeline cache file for writing: %s", path.c_str());
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()), data_size);
    LOGI("Saved pipeline cache: %zu bytes to %s", data_size, path.c_str());
    return true;
}

bool VulkanBackend::load_pipeline_cache(const std::string& path) {
    if (device_ == VK_NULL_HANDLE) {
        return false;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;  // No cache file yet, not an error
    }

    size_t data_size = static_cast<size_t>(file.tellg());
    if (data_size == 0) {
        return false;
    }

    file.seekg(0);
    std::vector<u8> data(data_size);
    file.read(reinterpret_cast<char*>(data.data()), data_size);

    // Destroy old cache and create new one with loaded data
    if (vk_pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, vk_pipeline_cache_, nullptr);
    }

    VkPipelineCacheCreateInfo cache_info = {};
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cache_info.initialDataSize = data_size;
    cache_info.pInitialData = data.data();

    VkResult result = vkCreatePipelineCache(device_, &cache_info, nullptr, &vk_pipeline_cache_);
    if (result != VK_SUCCESS) {
        LOGW("Failed to create pipeline cache from file: %d", result);
        // Create empty cache as fallback
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = nullptr;
        vkCreatePipelineCache(device_, &cache_info, nullptr, &vk_pipeline_cache_);
        return false;
    }

    LOGI("Loaded pipeline cache: %zu bytes from %s", data_size, path.c_str());
    return true;
}

//=============================================================================
// Debug Utils Implementation
//=============================================================================

void VulkanBackend::setup_debug_utils() {
    if (instance_ == VK_NULL_HANDLE) return;

    // Load debug utils function pointers (available when VK_EXT_debug_utils is enabled)
    pfn_create_debug_messenger_ = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    pfn_destroy_debug_messenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    pfn_set_debug_name_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(instance_, "vkSetDebugUtilsObjectNameEXT"));
    pfn_cmd_begin_label_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance_, "vkCmdBeginDebugUtilsLabelEXT"));
    pfn_cmd_end_label_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance_, "vkCmdEndDebugUtilsLabelEXT"));
    pfn_cmd_insert_label_ = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance_, "vkCmdInsertDebugUtilsLabelEXT"));

    if (!pfn_create_debug_messenger_) {
        LOGD("VK_EXT_debug_utils not available (release build or unsupported driver)");
        return;
    }

    // Create debug messenger with comprehensive severity/type filtering
    VkDebugUtilsMessengerCreateInfoEXT messenger_info = {};
    messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    messenger_info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = vulkan_debug_callback;
    messenger_info.pUserData = nullptr;

    VkResult result = pfn_create_debug_messenger_(instance_, &messenger_info, nullptr, &debug_messenger_);
    if (result != VK_SUCCESS) {
        LOGW("Failed to create debug messenger: %d", result);
        debug_messenger_ = VK_NULL_HANDLE;
    } else {
        LOGI("Vulkan debug messenger created (validation errors + warnings)");
    }
}

void VulkanBackend::destroy_debug_utils() {
    if (debug_messenger_ != VK_NULL_HANDLE && pfn_destroy_debug_messenger_) {
        pfn_destroy_debug_messenger_(instance_, debug_messenger_, nullptr);
        debug_messenger_ = VK_NULL_HANDLE;
    }
    pfn_set_debug_name_ = nullptr;
    pfn_cmd_begin_label_ = nullptr;
    pfn_cmd_end_label_ = nullptr;
    pfn_cmd_insert_label_ = nullptr;
    pfn_create_debug_messenger_ = nullptr;
    pfn_destroy_debug_messenger_ = nullptr;
}

void VulkanBackend::set_object_name(VkObjectType type, u64 handle, const char* name) {
    if (!pfn_set_debug_name_ || device_ == VK_NULL_HANDLE || !name) return;

    VkDebugUtilsObjectNameInfoEXT name_info = {};
    name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    name_info.objectType = type;
    name_info.objectHandle = handle;
    name_info.pObjectName = name;

    pfn_set_debug_name_(device_, &name_info);
}

void VulkanBackend::cmd_begin_label(const char* label, float r, float g, float b, float a) {
    if (!pfn_cmd_begin_label_ || current_frame_ >= command_buffers_.size()) return;

    VkDebugUtilsLabelEXT label_info = {};
    label_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label_info.pLabelName = label;
    label_info.color[0] = r;
    label_info.color[1] = g;
    label_info.color[2] = b;
    label_info.color[3] = a;

    pfn_cmd_begin_label_(command_buffers_[current_frame_], &label_info);
}

void VulkanBackend::cmd_end_label() {
    if (!pfn_cmd_end_label_ || current_frame_ >= command_buffers_.size()) return;
    pfn_cmd_end_label_(command_buffers_[current_frame_]);
}

void VulkanBackend::cmd_insert_label(const char* label, float r, float g, float b, float a) {
    if (!pfn_cmd_insert_label_ || current_frame_ >= command_buffers_.size()) return;

    VkDebugUtilsLabelEXT label_info = {};
    label_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label_info.pLabelName = label;
    label_info.color[0] = r;
    label_info.color[1] = g;
    label_info.color[2] = b;
    label_info.color[3] = a;

    pfn_cmd_insert_label_(command_buffers_[current_frame_], &label_info);
}


} // namespace x360mu

