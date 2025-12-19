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

#ifdef __ANDROID__
#include <android/log.h>
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>
#define LOG_TAG "360mu-vulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[VULKAN] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[VULKAN ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// Vulkan Debug Callback
//=============================================================================

#ifdef X360MU_VULKAN_DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void* user_data)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOGE("Vulkan: %s", callback_data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOGI("Vulkan Warning: %s", callback_data->pMessage);
    }
    return VK_FALSE;
}
#endif

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
    
    // Create render pass
    result = create_render_pass();
    if (result != VK_SUCCESS) {
        LOGE("Failed to create render pass: %d", result);
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
    
    LOGI("Vulkan backend initialized (%ux%u)", width, height);
    return Status::Ok;
}

void VulkanBackend::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    
    vkDeviceWaitIdle(device_);
    
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
    create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // V-Sync
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
    }
    
    return VK_SUCCESS;
}

VkResult VulkanBackend::create_render_pass() {
    VkAttachmentDescription color_attachment = {};
    color_attachment.format = swapchain_format_;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    VkAttachmentReference color_ref = {};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    
    VkRenderPassCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = 1;
    create_info.pAttachments = &color_attachment;
    create_info.subpassCount = 1;
    create_info.pSubpasses = &subpass;
    create_info.dependencyCount = 1;
    create_info.pDependencies = &dependency;
    
    return vkCreateRenderPass(device_, &create_info, nullptr, &render_pass_);
}

VkResult VulkanBackend::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    
    for (size_t i = 0; i < swapchain_image_views_.size(); i++) {
        VkFramebufferCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        create_info.renderPass = render_pass_;
        create_info.attachmentCount = 1;
        create_info.pAttachments = &swapchain_image_views_[i];
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
    // Create descriptor set layout for Xbox 360 shader constants
    VkDescriptorSetLayoutBinding bindings[3] = {};
    
    // Vertex constants (256 vec4)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    
    // Pixel constants (256 vec4)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    // Samplers (16 texture units)
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 16;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    
    VkDescriptorSetLayout layout;
    VkResult result = vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &layout);
    if (result != VK_SUCCESS) return result;
    descriptor_set_layouts_.push_back(layout);
    
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &layout;
    
    result = vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &pipeline_layout_);
    if (result != VK_SUCCESS) return result;
    
    // Create descriptor pool
    VkDescriptorPoolSize pool_sizes[3] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 256;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 1024;
    
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    pool_info.maxSets = 128;
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
    
    return vkBindBufferMemory(device_, edram_buffer_, edram_memory_, 0);
}

//=============================================================================
// Rendering
//=============================================================================

Status VulkanBackend::begin_frame() {
    // Check if swapchain is initialized
    if (swapchain_ == VK_NULL_HANDLE) {
        return Status::ErrorSwapchain;
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
    
    // Begin render pass
    VkRenderPassBeginInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass = render_pass_;
    rp_info.framebuffer = framebuffers_[current_image_index_];
    rp_info.renderArea.offset = {0, 0};
    rp_info.renderArea.extent = swapchain_extent_;
    
    VkClearValue clear_value = {};
    // Use a visible purple color so we can see rendering is working
    // (will be replaced with actual game graphics when GPU emulation is complete)
    clear_value.color = {{0.2f, 0.0f, 0.3f, 1.0f}};
    rp_info.clearValueCount = 1;
    rp_info.pClearValues = &clear_value;
    
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
    
    // Vertex input - Xbox 360 uses custom vertex formats
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    
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
    
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask = state.color_write_mask;
    blend_attachment.blendEnable = state.blend_enable;
    blend_attachment.srcColorBlendFactor = state.src_color_blend;
    blend_attachment.dstColorBlendFactor = state.dst_color_blend;
    blend_attachment.colorBlendOp = state.color_blend_op;
    blend_attachment.srcAlphaBlendFactor = state.src_alpha_blend;
    blend_attachment.dstAlphaBlendFactor = state.dst_alpha_blend;
    blend_attachment.alphaBlendOp = state.alpha_blend_op;
    
    VkPipelineColorBlendStateCreateInfo color_blending = {};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend_attachment;
    
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
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass_;
    pipeline_info.subpass = 0;
    
    VkPipeline pipeline;
    VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                                 nullptr, &pipeline);
    
    if (result == VK_SUCCESS) {
        pipeline_cache_[hash] = pipeline;
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

void VulkanBackend::bind_index_buffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdBindIndexBuffer(cmd, buffer, offset, type);
}

void VulkanBackend::bind_descriptor_set(VkDescriptorSet set) {
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
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
    
    width_ = width;
    height_ = height;
    
    // Cleanup old swapchain resources
    cleanup_swapchain();
    
    // Recreate swapchain
    VkResult result = create_swapchain();
    if (result != VK_SUCCESS) {
        LOGE("Failed to recreate swapchain: %d", result);
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

} // namespace x360mu

