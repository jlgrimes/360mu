# Task: Vulkan Backend Initialization

## Priority: 🔴 CRITICAL (Blocking)
## Estimated Time: 2-3 weeks
## Dependencies: None

---

## Objective

Initialize Vulkan, create a swapchain, and get pixels on screen. This task does NOT include shader translation - just the rendering infrastructure.

---

## What To Build

### Location
- `native/src/gpu/vulkan/`

### Files to Create/Modify

1. **`vulkan_backend.cpp`** - Device creation, swapchain
2. **`memory_manager.cpp`** - Vulkan memory allocation
3. **`swapchain.cpp`** - Presentation

---

## Specific Implementation

### 1. Vulkan Initialization (`vulkan_backend.cpp`)

```cpp
Status VulkanBackend::initialize(ANativeWindow* window) {
    // 1. Create Vulkan instance
    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "360mu";
    app_info.apiVersion = VK_API_VERSION_1_1;
    
    // 2. Enable required extensions
    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    
    VkInstanceCreateInfo create_info = {};
    create_info.enabledExtensionCount = extensions.size();
    create_info.ppEnabledExtensionNames = extensions.data();
    
    vkCreateInstance(&create_info, nullptr, &instance_);
    
    // 3. Create surface from Android window
    VkAndroidSurfaceCreateInfoKHR surface_info = {};
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = window;
    vkCreateAndroidSurfaceKHR(instance_, &surface_info, nullptr, &surface_);
    
    // 4. Pick physical device (prefer discrete GPU)
    pick_physical_device();
    
    // 5. Create logical device with graphics + compute queues
    create_logical_device();
    
    // 6. Create swapchain
    create_swapchain();
    
    // 7. Create command pool and buffers
    create_command_buffers();
    
    return Status::Ok;
}
```

### 2. Swapchain Creation

```cpp
void VulkanBackend::create_swapchain() {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);
    
    VkSwapchainCreateInfoKHR create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface_;
    create_info.minImageCount = 3; // Triple buffering
    create_info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    create_info.imageExtent = {width_, height_};
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; // Low latency
    
    vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_);
    
    // Get swapchain images
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr);
    swapchain_images_.resize(image_count_);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, swapchain_images_.data());
    
    // Create image views
    for (auto& image : swapchain_images_) {
        create_image_view(image);
    }
}
```

### 3. Frame Rendering Loop

```cpp
void VulkanBackend::begin_frame() {
    // Wait for previous frame
    vkWaitForFences(device_, 1, &frame_fences_[current_frame_], VK_TRUE, UINT64_MAX);
    
    // Acquire next swapchain image
    vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, 
                          image_available_semaphores_[current_frame_],
                          VK_NULL_HANDLE, &image_index_);
    
    vkResetFences(device_, 1, &frame_fences_[current_frame_]);
    
    // Begin command buffer
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(command_buffers_[current_frame_], &begin_info);
}

void VulkanBackend::end_frame() {
    vkEndCommandBuffer(command_buffers_[current_frame_]);
    
    // Submit
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_available_semaphores_[current_frame_];
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffers_[current_frame_];
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_finished_semaphores_[current_frame_];
    
    vkQueueSubmit(graphics_queue_, 1, &submit_info, frame_fences_[current_frame_]);
    
    // Present
    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished_semaphores_[current_frame_];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &image_index_;
    
    vkQueuePresentKHR(present_queue_, &present_info);
    
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}
```

### 4. Memory Manager

```cpp
class VulkanMemoryManager {
public:
    // Allocate buffer
    VulkanBuffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                               VkMemoryPropertyFlags properties);
    
    // Allocate image
    VulkanImage create_image(u32 width, u32 height, VkFormat format,
                             VkImageUsageFlags usage);
    
    // Staging buffer for uploads
    void upload_to_buffer(VulkanBuffer& buffer, const void* data, size_t size);
    void upload_to_image(VulkanImage& image, const void* data, size_t size);
    
private:
    u32 find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties);
};
```

### 5. Test Clear Screen

```cpp
void VulkanBackend::clear_screen(float r, float g, float b) {
    begin_frame();
    
    VkClearColorValue clear_color = {{r, g, b, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    
    // Transition image to TRANSFER_DST
    transition_image_layout(swapchain_images_[image_index_], 
                           VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    vkCmdClearColorImage(command_buffers_[current_frame_],
                        swapchain_images_[image_index_],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &clear_color, 1, &range);
    
    // Transition to PRESENT
    transition_image_layout(swapchain_images_[image_index_],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    
    end_frame();
}
```

---

## CMake Setup

Add to `CMakeLists.txt`:

```cmake
find_package(Vulkan REQUIRED)
target_link_libraries(x360mu_core Vulkan::Vulkan)

if(ANDROID)
    target_link_libraries(x360mu_core android)
endif()
```

---

## Test Cases

```cpp
TEST(VulkanTest, Initialize) {
    VulkanBackend backend;
    // Note: Need mock window for desktop testing
    EXPECT_EQ(backend.initialize(nullptr), Status::Ok);
}

TEST(VulkanTest, CreateBuffer) {
    VulkanBackend backend;
    backend.initialize(nullptr);
    
    auto buffer = backend.create_buffer(1024, 
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    EXPECT_NE(buffer.handle, VK_NULL_HANDLE);
}
```

---

## Do NOT Touch

- Shader translation (`shader_translator.cpp`)
- Command processor parsing (`command_processor.cpp`)
- eDRAM emulation (`edram.cpp`)
- CPU code (`src/cpu/`)
- Audio code (`src/apu/`)

---

## Success Criteria

1. ✅ Vulkan instance creates successfully
2. ✅ Swapchain created at correct resolution
3. ✅ Can clear screen to solid color
4. ✅ Frame timing works (vsync or mailbox)
5. ✅ Memory allocator works for buffers/images

---

## Header Structure

```cpp
// vulkan_backend.h
class VulkanBackend {
public:
    Status initialize(void* native_window);
    void shutdown();
    
    void begin_frame();
    void end_frame();
    void clear_screen(float r, float g, float b);
    
    // Resource creation
    VulkanBuffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage);
    VulkanImage create_image(u32 width, u32 height, VkFormat format);
    
    VkDevice get_device() const { return device_; }
    VkCommandBuffer get_command_buffer() const { return command_buffers_[current_frame_]; }
    
private:
    VkInstance instance_;
    VkPhysicalDevice physical_device_;
    VkDevice device_;
    VkSurfaceKHR surface_;
    VkSwapchainKHR swapchain_;
    VkQueue graphics_queue_;
    VkQueue present_queue_;
    VkCommandPool command_pool_;
    std::vector<VkCommandBuffer> command_buffers_;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    
    u32 current_frame_ = 0;
    u32 image_index_ = 0;
    u32 width_, height_;
};
```

---

*This task focuses only on Vulkan infrastructure. Shader translation and GPU command processing are separate tasks.*

