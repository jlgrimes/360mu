/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Vulkan Backend Unit Tests
 * 
 * Note: These tests require Vulkan to be available on the system.
 * Some tests may be skipped on systems without Vulkan support.
 */

#include <gtest/gtest.h>
#include "gpu/vulkan/vulkan_backend.h"
#include "gpu/vulkan/memory_manager.h"
#include "gpu/vulkan/swapchain.h"

namespace x360mu {
namespace {

// Test fixture for Vulkan tests
class VulkanTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Check if Vulkan is available
        u32 instance_extension_count = 0;
        VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, nullptr);
        vulkan_available_ = (result == VK_SUCCESS && instance_extension_count > 0);
        
        if (!vulkan_available_) {
            GTEST_SKIP() << "Vulkan not available on this system";
        }
    }
    
    void TearDown() override {
        // Cleanup if needed
    }
    
    bool vulkan_available_ = false;
};

// Test fixture for headless Vulkan tests (no surface required)
class VulkanHeadlessTest : public ::testing::Test {
protected:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    bool initialized_ = false;
    
    void SetUp() override {
        // Create minimal Vulkan instance for headless testing
        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "360mu-test";
        app_info.apiVersion = VK_API_VERSION_1_1;
        
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        
        VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            GTEST_SKIP() << "Failed to create Vulkan instance";
            return;
        }
        
        // Get physical device
        u32 device_count = 0;
        vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
        if (device_count == 0) {
            GTEST_SKIP() << "No Vulkan devices found";
            return;
        }
        
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
        physical_device_ = devices[0];
        
        // Find graphics queue
        u32 queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());
        
        for (u32 i = 0; i < queue_family_count; i++) {
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queue_family_ = i;
                break;
            }
        }
        
        // Create logical device
        float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        
        VkDeviceCreateInfo device_info = {};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        
        result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
        if (result != VK_SUCCESS) {
            GTEST_SKIP() << "Failed to create Vulkan device";
            return;
        }
        
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        initialized_ = true;
    }
    
    void TearDown() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
    }
};

//=============================================================================
// VulkanBackend Tests
//=============================================================================

TEST_F(VulkanTest, BackendCreation) {
    VulkanBackend backend;
    // Backend should be default constructible
    SUCCEED();
}

TEST_F(VulkanTest, BackendInitializeWithNullWindow) {
    VulkanBackend backend;
    // Initialize with null window - should fail gracefully on non-Android
#ifndef __ANDROID__
    Status result = backend.initialize(nullptr, 1280, 720);
    // On non-Android, this is expected to fail due to no surface extension
    EXPECT_NE(result, Status::Ok);
#else
    GTEST_SKIP() << "Test requires Android surface";
#endif
}

//=============================================================================
// Memory Manager Tests (Headless)
//=============================================================================

TEST_F(VulkanHeadlessTest, MemoryManagerInitialize) {
    if (!initialized_) GTEST_SKIP();
    
    VulkanMemoryManager mem_manager;
    Status result = mem_manager.initialize(device_, physical_device_, queue_, queue_family_);
    EXPECT_EQ(result, Status::Ok);
    mem_manager.shutdown();
}

TEST_F(VulkanHeadlessTest, CreateBuffer) {
    if (!initialized_) GTEST_SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT_EQ(mem_manager.initialize(device_, physical_device_, queue_, queue_family_), Status::Ok);
    
    // Create a device-local buffer
    ManagedBuffer buffer = mem_manager.create_buffer(
        1024,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    
    EXPECT_TRUE(buffer.is_valid());
    EXPECT_EQ(buffer.size, 1024u);
    EXPECT_EQ(buffer.mapped, nullptr);  // Device-local is not mapped
    
    mem_manager.destroy_buffer(buffer);
    EXPECT_FALSE(buffer.is_valid());
    
    mem_manager.shutdown();
}

TEST_F(VulkanHeadlessTest, CreateHostVisibleBuffer) {
    if (!initialized_) GTEST_SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT_EQ(mem_manager.initialize(device_, physical_device_, queue_, queue_family_), Status::Ok);
    
    // Create a host-visible buffer
    ManagedBuffer buffer = mem_manager.create_buffer(
        1024,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    
    EXPECT_TRUE(buffer.is_valid());
    EXPECT_NE(buffer.mapped, nullptr);  // Host-visible should be mapped
    
    // Write some data
    memset(buffer.mapped, 0xAB, 1024);
    
    mem_manager.destroy_buffer(buffer);
    mem_manager.shutdown();
}

TEST_F(VulkanHeadlessTest, CreateImage) {
    if (!initialized_) GTEST_SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT_EQ(mem_manager.initialize(device_, physical_device_, queue_, queue_family_), Status::Ok);
    
    // Create a texture image
    ManagedImage image = mem_manager.create_image(
        256, 256,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    );
    
    EXPECT_TRUE(image.is_valid());
    EXPECT_EQ(image.width, 256u);
    EXPECT_EQ(image.height, 256u);
    EXPECT_EQ(image.format, VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_NE(image.view, VK_NULL_HANDLE);
    
    mem_manager.destroy_image(image);
    EXPECT_FALSE(image.is_valid());
    
    mem_manager.shutdown();
}

TEST_F(VulkanHeadlessTest, CreateStagingBuffer) {
    if (!initialized_) GTEST_SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT_EQ(mem_manager.initialize(device_, physical_device_, queue_, queue_family_), Status::Ok);
    
    ManagedBuffer staging = mem_manager.create_staging_buffer(4096);
    
    EXPECT_TRUE(staging.is_valid());
    EXPECT_NE(staging.mapped, nullptr);
    
    mem_manager.destroy_buffer(staging);
    mem_manager.shutdown();
}

TEST_F(VulkanHeadlessTest, UploadToBuffer) {
    if (!initialized_) GTEST_SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT_EQ(mem_manager.initialize(device_, physical_device_, queue_, queue_family_), Status::Ok);
    
    // Create device-local buffer
    ManagedBuffer buffer = mem_manager.create_buffer(
        256,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    ASSERT_TRUE(buffer.is_valid());
    
    // Create test data
    std::vector<float> vertices(64, 1.0f);
    
    // Upload - this should use staging
    mem_manager.upload_to_buffer(buffer, vertices.data(), vertices.size() * sizeof(float));
    
    // Check stats
    EXPECT_GE(mem_manager.get_stats().staging_uploads, 1u);
    
    mem_manager.destroy_buffer(buffer);
    mem_manager.shutdown();
}

TEST_F(VulkanHeadlessTest, MemoryTypeFind) {
    if (!initialized_) GTEST_SKIP();
    
    VulkanMemoryManager mem_manager;
    ASSERT_EQ(mem_manager.initialize(device_, physical_device_, queue_, queue_family_), Status::Ok);
    
    // Should find device-local memory
    u32 device_local = mem_manager.find_memory_type(
        UINT32_MAX, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    EXPECT_NE(device_local, UINT32_MAX);
    
    // Should find host-visible memory
    u32 host_visible = mem_manager.find_memory_type(
        UINT32_MAX, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    EXPECT_NE(host_visible, UINT32_MAX);
    
    mem_manager.shutdown();
}

//=============================================================================
// Pipeline State Tests
//=============================================================================

TEST(PipelineStateTest, HashComputation) {
    PipelineState state1;
    PipelineState state2;
    
    // Same state should have same hash
    EXPECT_EQ(state1.compute_hash(), state2.compute_hash());
    
    // Different states should have different hashes
    state2.cull_mode = VK_CULL_MODE_FRONT_BIT;
    EXPECT_NE(state1.compute_hash(), state2.compute_hash());
}

TEST(PipelineStateTest, DefaultValues) {
    PipelineState state;
    
    EXPECT_EQ(state.primitive_topology, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    EXPECT_EQ(state.polygon_mode, VK_POLYGON_MODE_FILL);
    EXPECT_EQ(state.cull_mode, VK_CULL_MODE_BACK_BIT);
    EXPECT_EQ(state.front_face, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    EXPECT_EQ(state.depth_test_enable, VK_TRUE);
    EXPECT_EQ(state.depth_write_enable, VK_TRUE);
    EXPECT_EQ(state.blend_enable, VK_FALSE);
}

//=============================================================================
// VulkanBuffer/VulkanImage Struct Tests
//=============================================================================

TEST(VulkanBufferTest, DefaultValues) {
    VulkanBuffer buffer;
    
    EXPECT_EQ(buffer.buffer, VK_NULL_HANDLE);
    EXPECT_EQ(buffer.memory, VK_NULL_HANDLE);
    EXPECT_EQ(buffer.size, 0u);
    EXPECT_EQ(buffer.mapped, nullptr);
}

TEST(VulkanImageTest, DefaultValues) {
    VulkanImage image;
    
    EXPECT_EQ(image.image, VK_NULL_HANDLE);
    EXPECT_EQ(image.memory, VK_NULL_HANDLE);
    EXPECT_EQ(image.view, VK_NULL_HANDLE);
    EXPECT_EQ(image.width, 0u);
    EXPECT_EQ(image.height, 0u);
    EXPECT_EQ(image.format, VK_FORMAT_UNDEFINED);
}

TEST(ManagedBufferTest, IsValid) {
    ManagedBuffer buffer;
    EXPECT_FALSE(buffer.is_valid());
    
    // Simulate valid buffer (just for test - don't actually create)
    buffer.buffer = reinterpret_cast<VkBuffer>(1);
    EXPECT_TRUE(buffer.is_valid());
}

TEST(ManagedImageTest, IsValid) {
    ManagedImage image;
    EXPECT_FALSE(image.is_valid());
    
    // Simulate valid image
    image.image = reinterpret_cast<VkImage>(1);
    EXPECT_TRUE(image.is_valid());
}

//=============================================================================
// SwapchainConfig Tests
//=============================================================================

TEST(SwapchainConfigTest, DefaultValues) {
    SwapchainConfig config;
    
    EXPECT_EQ(config.width, 0u);
    EXPECT_EQ(config.height, 0u);
    EXPECT_EQ(config.format, VK_FORMAT_B8G8R8A8_SRGB);
    EXPECT_EQ(config.color_space, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    EXPECT_EQ(config.present_mode, VK_PRESENT_MODE_FIFO_KHR);
    EXPECT_EQ(config.min_image_count, 3u);
}

} // namespace
} // namespace x360mu
