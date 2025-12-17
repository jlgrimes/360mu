/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Vulkan Memory Manager Implementation
 */

#include "memory_manager.h"
#include <cstring>
#include <algorithm>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-vkmem"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[VKMEM] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[VKMEM ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

VulkanMemoryManager::~VulkanMemoryManager() {
    shutdown();
}

Status VulkanMemoryManager::initialize(VkDevice device, VkPhysicalDevice physical_device,
                                        VkQueue transfer_queue, u32 queue_family_index) {
    device_ = device;
    physical_device_ = physical_device;
    transfer_queue_ = transfer_queue;
    queue_family_index_ = queue_family_index;
    
    // Get memory properties
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties_);
    
    // Create command pool for transfers
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue_family_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                     VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    
    VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create command pool: %d", result);
        return Status::ErrorInit;
    }
    
    // Allocate upload command buffer
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    
    result = vkAllocateCommandBuffers(device_, &alloc_info, &upload_cmd_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to allocate command buffer: %d", result);
        return Status::ErrorInit;
    }
    
    // Create upload fence
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    
    result = vkCreateFence(device_, &fence_info, nullptr, &upload_fence_);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create fence: %d", result);
        return Status::ErrorInit;
    }
    
    LOGI("Vulkan memory manager initialized");
    return Status::Ok;
}

void VulkanMemoryManager::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    
    vkDeviceWaitIdle(device_);
    
    // Destroy staging buffers
    for (auto& staging : staging_buffers_) {
        if (staging.mapped) {
            vkUnmapMemory(device_, staging.memory);
        }
        vkDestroyBuffer(device_, staging.buffer, nullptr);
        vkFreeMemory(device_, staging.memory, nullptr);
    }
    staging_buffers_.clear();
    
    // Destroy fence
    if (upload_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, upload_fence_, nullptr);
        upload_fence_ = VK_NULL_HANDLE;
    }
    
    // Destroy command pool
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    
    LOGI("Vulkan memory manager shutdown (allocated: %llu bytes, buffers: %llu, images: %llu)",
         (unsigned long long)stats_.total_allocated,
         (unsigned long long)stats_.buffer_count,
         (unsigned long long)stats_.image_count);
    
    device_ = VK_NULL_HANDLE;
}

u32 VulkanMemoryManager::find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties) const {
    for (u32 i = 0; i < memory_properties_.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (memory_properties_.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    LOGE("Failed to find suitable memory type");
    return UINT32_MAX;
}

ManagedBuffer VulkanMemoryManager::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                  VkMemoryPropertyFlags properties) {
    ManagedBuffer buffer;
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
    
    stats_.total_allocated += mem_reqs.size;
    stats_.buffer_count++;
    
    LOGD("Created buffer: size=%llu", (unsigned long long)size);
    return buffer;
}

void VulkanMemoryManager::destroy_buffer(ManagedBuffer& buffer) {
    if (!buffer.is_valid()) return;
    
    if (buffer.mapped) {
        vkUnmapMemory(device_, buffer.memory);
    }
    
    vkDestroyBuffer(device_, buffer.buffer, nullptr);
    vkFreeMemory(device_, buffer.memory, nullptr);
    
    stats_.total_allocated -= buffer.size;
    stats_.buffer_count--;
    
    buffer = ManagedBuffer{};
}

ManagedImage VulkanMemoryManager::create_image(u32 width, u32 height, VkFormat format,
                                                VkImageUsageFlags usage, u32 mip_levels) {
    ManagedImage image;
    image.width = width;
    image.height = height;
    image.format = format;
    image.mip_levels = mip_levels;
    
    // Create image
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = mip_levels;
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
    view_info.subresourceRange.levelCount = mip_levels;
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
    
    stats_.total_allocated += mem_reqs.size;
    stats_.image_count++;
    
    LOGD("Created image: %ux%u, format=%d, mips=%u", width, height, format, mip_levels);
    return image;
}

void VulkanMemoryManager::destroy_image(ManagedImage& image) {
    if (!image.is_valid()) return;
    
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
    }
    vkDestroyImage(device_, image.image, nullptr);
    vkFreeMemory(device_, image.memory, nullptr);
    
    stats_.image_count--;
    
    image = ManagedImage{};
}

ManagedBuffer VulkanMemoryManager::create_staging_buffer(VkDeviceSize size) {
    return create_buffer(size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

StagingBuffer* VulkanMemoryManager::get_staging_buffer(VkDeviceSize size) {
    // Find existing buffer with enough space
    for (auto& staging : staging_buffers_) {
        if (!staging.in_use && (staging.size - staging.offset) >= size) {
            staging.in_use = true;
            return &staging;
        }
    }
    
    // Create new staging buffer
    VkDeviceSize buffer_size = std::max(size, STAGING_BUFFER_SIZE);
    
    StagingBuffer staging;
    staging.size = buffer_size;
    staging.offset = 0;
    staging.in_use = true;
    
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkResult result = vkCreateBuffer(device_, &buffer_info, nullptr, &staging.buffer);
    if (result != VK_SUCCESS) {
        LOGE("Failed to create staging buffer: %d", result);
        return nullptr;
    }
    
    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device_, staging.buffer, &mem_reqs);
    
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    result = vkAllocateMemory(device_, &alloc_info, nullptr, &staging.memory);
    if (result != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging.buffer, nullptr);
        return nullptr;
    }
    
    vkBindBufferMemory(device_, staging.buffer, staging.memory, 0);
    vkMapMemory(device_, staging.memory, 0, buffer_size, 0, &staging.mapped);
    
    staging_buffers_.push_back(staging);
    return &staging_buffers_.back();
}

VkCommandBuffer VulkanMemoryManager::begin_transfer() {
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkResetCommandBuffer(upload_cmd_, 0);
    vkBeginCommandBuffer(upload_cmd_, &begin_info);
    
    return upload_cmd_;
}

void VulkanMemoryManager::end_transfer(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    
    vkResetFences(device_, 1, &upload_fence_);
    vkQueueSubmit(transfer_queue_, 1, &submit_info, upload_fence_);
    vkWaitForFences(device_, 1, &upload_fence_, VK_TRUE, UINT64_MAX);
    
    // Reset staging buffers
    for (auto& staging : staging_buffers_) {
        staging.in_use = false;
        staging.offset = 0;
    }
    
    stats_.staging_uploads++;
}

void VulkanMemoryManager::upload_to_buffer(ManagedBuffer& buffer, const void* data,
                                            VkDeviceSize size, VkDeviceSize offset) {
    if (buffer.mapped) {
        // Direct copy for host-visible memory
        memcpy(static_cast<char*>(buffer.mapped) + offset, data, size);
        return;
    }
    
    // Use staging buffer
    StagingBuffer* staging = get_staging_buffer(size);
    if (!staging) {
        LOGE("Failed to get staging buffer");
        return;
    }
    
    memcpy(static_cast<char*>(staging->mapped) + staging->offset, data, size);
    
    VkCommandBuffer cmd = begin_transfer();
    
    VkBufferCopy copy_region = {};
    copy_region.srcOffset = staging->offset;
    copy_region.dstOffset = offset;
    copy_region.size = size;
    vkCmdCopyBuffer(cmd, staging->buffer, buffer.buffer, 1, &copy_region);
    
    staging->offset += size;
    
    end_transfer(cmd);
}

void VulkanMemoryManager::upload_to_image(ManagedImage& image, const void* data, size_t size) {
    StagingBuffer* staging = get_staging_buffer(size);
    if (!staging) {
        LOGE("Failed to get staging buffer for image upload");
        return;
    }
    
    memcpy(static_cast<char*>(staging->mapped) + staging->offset, data, size);
    
    VkCommandBuffer cmd = begin_transfer();
    
    // Transition to transfer destination
    transition_image_layout(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.mip_levels);
    
    // Copy buffer to image
    VkBufferImageCopy region = {};
    region.bufferOffset = staging->offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {image.width, image.height, 1};
    
    vkCmdCopyBufferToImage(cmd, staging->buffer, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // Transition to shader read
    transition_image_layout(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, image.mip_levels);
    
    staging->offset += size;
    
    end_transfer(cmd);
}

void VulkanMemoryManager::transition_image_layout(VkCommandBuffer cmd, VkImage image,
                                                   VkImageLayout old_layout, 
                                                   VkImageLayout new_layout,
                                                   u32 mip_levels) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    
    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;
    
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
    } else {
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanMemoryManager::flush_uploads() {
    // Currently uploads are synchronous, so nothing to flush
    // This could be extended to batch multiple uploads
}

} // namespace x360mu
