/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Standalone Vulkan Tests
 * 
 * This file can be compiled and run independently to test Vulkan functionality.
 * 
 * Compile with:
 *   clang++ -std=c++20 -I../../include -I../../src test_vulkan_standalone.cpp \
 *           ../../src/gpu/vulkan/memory_manager.cpp \
 *           ../../src/gpu/vulkan/swapchain.cpp \
 *           -lvulkan -o test_vulkan_standalone
 * 
 * Run:
 *   ./test_vulkan_standalone
 */

#include <iostream>
#include <cstring>
#include <vector>
#include <vulkan/vulkan.h>
#include "x360mu/types.h"
#include "gpu/vulkan/memory_manager.h"
#include "gpu/vulkan/swapchain.h"

using namespace x360mu;

// Simple test framework
static int tests_run = 0;
static int tests_passed = 0;
static int tests_skipped = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " #name "... "; \
    tests_run++; \
    try { test_##name(); tests_passed++; std::cout << "PASSED\n"; } \
    catch (const char* msg) { \
        if (strcmp(msg, "SKIP") == 0) { tests_skipped++; std::cout << "SKIPPED\n"; } \
        else { std::cout << "FAILED: " << msg << "\n"; } \
    } \
} while(0)

#define ASSERT(cond) if (!(cond)) throw "Assertion failed: " #cond
#define SKIP() throw "SKIP"

// Global test state
struct TestContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 queue_family = 0;
    bool initialized = false;
    
    bool init() {
        // Create instance
        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "360mu-test";
        app_info.apiVersion = VK_API_VERSION_1_1;
        
        // Check for portability enumeration (required for MoltenVK on macOS)
        u32 ext_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> available_exts(ext_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, available_exts.data());
        
        std::vector<const char*> extensions;
        bool has_portability = false;
        for (const auto& ext : available_exts) {
            if (strcmp(ext.extensionName, "VK_KHR_portability_enumeration") == 0) {
                has_portability = true;
                extensions.push_back("VK_KHR_portability_enumeration");
            }
        }
        
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<u32>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        
        // Enable portability enumeration flag if extension is available
        if (has_portability) {
            create_info.flags |= 0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
        }
        
        VkResult result = vkCreateInstance(&create_info, nullptr, &instance);
        if (result != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan instance: " << result << "\n";
            return false;
        }
        
        // Get physical device
        u32 device_count = 0;
        vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
        if (device_count == 0) {
            std::cerr << "No Vulkan devices found\n";
            return false;
        }
        
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        physical_device = devices[0];
        
        // Print device info
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device, &props);
        std::cout << "Using device: " << props.deviceName << "\n";
        
        // Find graphics queue
        u32 queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());
        
        for (u32 i = 0; i < queue_family_count; i++) {
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queue_family = i;
                break;
            }
        }
        
        // Create logical device
        float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        
        VkDeviceCreateInfo device_info = {};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        
        if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan device\n";
            return false;
        }
        
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        initialized = true;
        return true;
    }
    
    void cleanup() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }
} ctx;

//=============================================================================
// Tests
//=============================================================================

TEST(vulkan_available) {
    u32 count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    ASSERT(result == VK_SUCCESS);
    ASSERT(count > 0);
}

TEST(memory_manager_init) {
    if (!ctx.initialized) SKIP();
    
    VulkanMemoryManager mem_manager;
    Status result = mem_manager.initialize(ctx.device, ctx.physical_device, ctx.queue, ctx.queue_family);
    ASSERT(result == Status::Ok);
    mem_manager.shutdown();
}

TEST(create_device_local_buffer) {
    if (!ctx.initialized) SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT(mem_manager.initialize(ctx.device, ctx.physical_device, ctx.queue, ctx.queue_family) == Status::Ok);
    
    ManagedBuffer buffer = mem_manager.create_buffer(
        1024,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    
    ASSERT(buffer.is_valid());
    ASSERT(buffer.size == 1024);
    ASSERT(buffer.mapped == nullptr);  // Device-local should not be mapped
    
    mem_manager.destroy_buffer(buffer);
    ASSERT(!buffer.is_valid());
    
    mem_manager.shutdown();
}

TEST(create_host_visible_buffer) {
    if (!ctx.initialized) SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT(mem_manager.initialize(ctx.device, ctx.physical_device, ctx.queue, ctx.queue_family) == Status::Ok);
    
    ManagedBuffer buffer = mem_manager.create_buffer(
        1024,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    
    ASSERT(buffer.is_valid());
    ASSERT(buffer.mapped != nullptr);  // Host-visible should be mapped
    
    // Write data
    memset(buffer.mapped, 0xAB, 1024);
    
    mem_manager.destroy_buffer(buffer);
    mem_manager.shutdown();
}

TEST(create_image) {
    if (!ctx.initialized) SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT(mem_manager.initialize(ctx.device, ctx.physical_device, ctx.queue, ctx.queue_family) == Status::Ok);
    
    ManagedImage image = mem_manager.create_image(
        256, 256,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    );
    
    ASSERT(image.is_valid());
    ASSERT(image.width == 256);
    ASSERT(image.height == 256);
    ASSERT(image.format == VK_FORMAT_R8G8B8A8_UNORM);
    ASSERT(image.view != VK_NULL_HANDLE);
    
    mem_manager.destroy_image(image);
    ASSERT(!image.is_valid());
    
    mem_manager.shutdown();
}

TEST(upload_to_buffer) {
    if (!ctx.initialized) SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT(mem_manager.initialize(ctx.device, ctx.physical_device, ctx.queue, ctx.queue_family) == Status::Ok);
    
    ManagedBuffer buffer = mem_manager.create_buffer(
        256,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    ASSERT(buffer.is_valid());
    
    // Create test data
    std::vector<float> vertices(64, 1.0f);
    
    // Upload via staging
    mem_manager.upload_to_buffer(buffer, vertices.data(), vertices.size() * sizeof(float));
    
    ASSERT(mem_manager.get_stats().staging_uploads >= 1);
    
    mem_manager.destroy_buffer(buffer);
    mem_manager.shutdown();
}

TEST(find_memory_types) {
    if (!ctx.initialized) SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT(mem_manager.initialize(ctx.device, ctx.physical_device, ctx.queue, ctx.queue_family) == Status::Ok);
    
    u32 device_local = mem_manager.find_memory_type(UINT32_MAX, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ASSERT(device_local != UINT32_MAX);
    
    u32 host_visible = mem_manager.find_memory_type(UINT32_MAX, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    ASSERT(host_visible != UINT32_MAX);
    
    mem_manager.shutdown();
}

TEST(swapchain_config_defaults) {
    SwapchainConfig config;
    ASSERT(config.width == 0);
    ASSERT(config.height == 0);
    ASSERT(config.format == VK_FORMAT_B8G8R8A8_SRGB);
    ASSERT(config.color_space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    ASSERT(config.present_mode == VK_PRESENT_MODE_FIFO_KHR);
    ASSERT(config.min_image_count == 3);
}

TEST(managed_buffer_defaults) {
    ManagedBuffer buffer;
    ASSERT(!buffer.is_valid());
    ASSERT(buffer.buffer == VK_NULL_HANDLE);
    ASSERT(buffer.memory == VK_NULL_HANDLE);
    ASSERT(buffer.size == 0);
    ASSERT(buffer.mapped == nullptr);
}

TEST(managed_image_defaults) {
    ManagedImage image;
    ASSERT(!image.is_valid());
    ASSERT(image.image == VK_NULL_HANDLE);
    ASSERT(image.memory == VK_NULL_HANDLE);
    ASSERT(image.view == VK_NULL_HANDLE);
    ASSERT(image.width == 0);
    ASSERT(image.height == 0);
}

TEST(multiple_buffers) {
    if (!ctx.initialized) SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT(mem_manager.initialize(ctx.device, ctx.physical_device, ctx.queue, ctx.queue_family) == Status::Ok);
    
    std::vector<ManagedBuffer> buffers;
    for (int i = 0; i < 10; i++) {
        ManagedBuffer buf = mem_manager.create_buffer(
            1024 * (i + 1),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
        ASSERT(buf.is_valid());
        buffers.push_back(buf);
    }
    
    ASSERT(mem_manager.get_stats().buffer_count == 10);
    
    for (auto& buf : buffers) {
        mem_manager.destroy_buffer(buf);
    }
    
    ASSERT(mem_manager.get_stats().buffer_count == 0);
    
    mem_manager.shutdown();
}

TEST(multiple_images) {
    if (!ctx.initialized) SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT(mem_manager.initialize(ctx.device, ctx.physical_device, ctx.queue, ctx.queue_family) == Status::Ok);
    
    std::vector<ManagedImage> images;
    for (int i = 0; i < 5; i++) {
        ManagedImage img = mem_manager.create_image(
            64 << i, 64 << i,  // 64, 128, 256, 512, 1024
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        );
        ASSERT(img.is_valid());
        images.push_back(img);
    }
    
    ASSERT(mem_manager.get_stats().image_count == 5);
    
    for (auto& img : images) {
        mem_manager.destroy_image(img);
    }
    
    ASSERT(mem_manager.get_stats().image_count == 0);
    
    mem_manager.shutdown();
}

//=============================================================================
// Main
//=============================================================================

int main() {
    std::cout << "=== 360μ Vulkan Standalone Tests ===\n\n";
    
    // Initialize Vulkan context
    std::cout << "Initializing Vulkan...\n";
    if (!ctx.init()) {
        std::cout << "\nVulkan initialization failed - some tests will be skipped\n\n";
    } else {
        std::cout << "Vulkan initialized successfully\n\n";
    }
    
    // Run tests
    RUN_TEST(vulkan_available);
    RUN_TEST(managed_buffer_defaults);
    RUN_TEST(managed_image_defaults);
    RUN_TEST(swapchain_config_defaults);
    RUN_TEST(memory_manager_init);
    RUN_TEST(find_memory_types);
    RUN_TEST(create_device_local_buffer);
    RUN_TEST(create_host_visible_buffer);
    RUN_TEST(create_image);
    RUN_TEST(upload_to_buffer);
    RUN_TEST(multiple_buffers);
    RUN_TEST(multiple_images);
    
    // Cleanup
    ctx.cleanup();
    
    // Results
    std::cout << "\n=== Results ===\n";
    std::cout << "Total:   " << tests_run << "\n";
    std::cout << "Passed:  " << tests_passed << "\n";
    std::cout << "Skipped: " << tests_skipped << "\n";
    std::cout << "Failed:  " << (tests_run - tests_passed - tests_skipped) << "\n";
    
    return (tests_run - tests_passed - tests_skipped) == 0 ? 0 : 1;
}
