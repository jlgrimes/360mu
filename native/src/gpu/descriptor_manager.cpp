/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Descriptor Manager Implementation
 */

#include "descriptor_manager.h"
#include "vulkan/vulkan_backend.h"
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-descriptors"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[DESCRIPTORS] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[DESCRIPTORS ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

DescriptorManager::DescriptorManager() {
    for (auto& frame : frames_) {
        frame.set = VK_NULL_HANDLE;
        frame.vertex_constants_buffer = VK_NULL_HANDLE;
        frame.vertex_constants_memory = VK_NULL_HANDLE;
        frame.vertex_constants_mapped = nullptr;
        frame.pixel_constants_buffer = VK_NULL_HANDLE;
        frame.pixel_constants_memory = VK_NULL_HANDLE;
        frame.pixel_constants_mapped = nullptr;
        frame.bool_constants_buffer = VK_NULL_HANDLE;
        frame.bool_constants_memory = VK_NULL_HANDLE;
        frame.bool_constants_mapped = nullptr;
        frame.loop_constants_buffer = VK_NULL_HANDLE;
        frame.loop_constants_memory = VK_NULL_HANDLE;
        frame.loop_constants_mapped = nullptr;
        frame.needs_update = true;
    }
}

DescriptorManager::~DescriptorManager() {
    shutdown();
}

Status DescriptorManager::initialize(VulkanBackend* vulkan) {
    vulkan_ = vulkan;
    
    if (create_descriptor_layout() != Status::Ok) {
        LOGE("Failed to create descriptor layout");
        return Status::ErrorInit;
    }
    
    if (create_pipeline_layout() != Status::Ok) {
        LOGE("Failed to create pipeline layout");
        return Status::ErrorInit;
    }
    
    if (create_descriptor_pool() != Status::Ok) {
        LOGE("Failed to create descriptor pool");
        return Status::ErrorInit;
    }
    
    if (create_default_resources() != Status::Ok) {
        LOGE("Failed to create default resources");
        return Status::ErrorInit;
    }
    
    // Create per-frame resources
    for (u32 i = 0; i < MAX_FRAMES; i++) {
        if (create_frame_resources(frames_[i]) != Status::Ok) {
            LOGE("Failed to create frame resources for frame %u", i);
            return Status::ErrorInit;
        }
    }
    
    LOGI("Descriptor manager initialized (%u frames)", MAX_FRAMES);
    return Status::Ok;
}

void DescriptorManager::shutdown() {
    if (vulkan_ == nullptr) return;
    
    VkDevice device = vulkan_->device();
    vkDeviceWaitIdle(device);
    
    // Destroy frame resources
    for (auto& frame : frames_) {
        destroy_frame_resources(frame);
    }
    
    // Destroy default resources
    destroy_default_resources();
    
    // Destroy layout and pool
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    
    vulkan_ = nullptr;
    LOGI("Descriptor manager shutdown");
}

Status DescriptorManager::create_descriptor_layout() {
    // Descriptor set layout:
    // Binding 0: Vertex constants (uniform buffer)
    // Binding 1: Pixel constants (uniform buffer)
    // Binding 2: Bool constants (uniform buffer)
    // Binding 3: Loop constants (uniform buffer)
    // Binding 4-19: Textures (combined image samplers)
    
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    
    // Vertex constants
    VkDescriptorSetLayoutBinding vertex_const{};
    vertex_const.binding = 0;
    vertex_const.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    vertex_const.descriptorCount = 1;
    vertex_const.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings.push_back(vertex_const);
    
    // Pixel constants
    VkDescriptorSetLayoutBinding pixel_const{};
    pixel_const.binding = 1;
    pixel_const.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pixel_const.descriptorCount = 1;
    pixel_const.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(pixel_const);
    
    // Bool constants
    VkDescriptorSetLayoutBinding bool_const{};
    bool_const.binding = 2;
    bool_const.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bool_const.descriptorCount = 1;
    bool_const.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(bool_const);
    
    // Loop constants
    VkDescriptorSetLayoutBinding loop_const{};
    loop_const.binding = 3;
    loop_const.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    loop_const.descriptorCount = 1;
    loop_const.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(loop_const);
    
    // Textures (combined image samplers)
    for (u32 i = 0; i < MAX_TEXTURE_BINDINGS; i++) {
        VkDescriptorSetLayoutBinding texture{};
        texture.binding = 4 + i;
        texture.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texture.descriptorCount = 1;
        texture.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(texture);
    }
    
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<u32>(bindings.size());
    layout_info.pBindings = bindings.data();
    
    if (vkCreateDescriptorSetLayout(vulkan_->device(), &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    return Status::Ok;
}

Status DescriptorManager::create_pipeline_layout() {
    // Push constants for per-draw data
    VkPushConstantRange push_constant{};
    push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_constant.offset = 0;
    push_constant.size = 64;  // 16 floats for misc per-draw data
    
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constant;
    
    if (vkCreatePipelineLayout(vulkan_->device(), &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    return Status::Ok;
}

Status DescriptorManager::create_descriptor_pool() {
    std::vector<VkDescriptorPoolSize> pool_sizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * MAX_FRAMES},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURE_BINDINGS * MAX_FRAMES}
    };
    
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = static_cast<u32>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    pool_info.maxSets = MAX_FRAMES;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    
    if (vkCreateDescriptorPool(vulkan_->device(), &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    return Status::Ok;
}

Status DescriptorManager::create_default_resources() {
    VkDevice device = vulkan_->device();
    
    // Create default sampler
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    
    if (vkCreateSampler(device, &sampler_info, nullptr, &default_sampler_) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    // Create default 1x1 texture
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {1, 1, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    if (vkCreateImage(device, &image_info, nullptr, &default_texture_) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, default_texture_, &mem_reqs);
    
    u32 mem_type = vulkan_->find_memory_type(mem_reqs.memoryTypeBits, 
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = mem_type;
    
    if (vkAllocateMemory(device, &alloc_info, nullptr, &default_texture_memory_) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    vkBindImageMemory(device, default_texture_, default_texture_memory_, 0);
    
    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = default_texture_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(device, &view_info, nullptr, &default_texture_view_) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    return Status::Ok;
}

void DescriptorManager::destroy_default_resources() {
    VkDevice device = vulkan_->device();
    
    if (default_texture_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, default_texture_view_, nullptr);
        default_texture_view_ = VK_NULL_HANDLE;
    }
    if (default_texture_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, default_texture_, nullptr);
        default_texture_ = VK_NULL_HANDLE;
    }
    if (default_texture_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, default_texture_memory_, nullptr);
        default_texture_memory_ = VK_NULL_HANDLE;
    }
    if (default_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device, default_sampler_, nullptr);
        default_sampler_ = VK_NULL_HANDLE;
    }
}

VkBuffer DescriptorManager::create_buffer(u64 size, VkBufferUsageFlags usage, 
                                           VkDeviceMemory& memory, void** mapped) {
    VkDevice device = vulkan_->device();
    
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkBuffer buffer;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    
    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device, buffer, &mem_reqs);
    
    u32 mem_type = vulkan_->find_memory_type(
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = mem_type;
    
    if (vkAllocateMemory(device, &alloc_info, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        return VK_NULL_HANDLE;
    }
    
    vkBindBufferMemory(device, buffer, memory, 0);
    
    if (mapped) {
        vkMapMemory(device, memory, 0, size, 0, mapped);
    }
    
    return buffer;
}

void DescriptorManager::destroy_buffer(VkBuffer& buffer, VkDeviceMemory& memory, void** mapped) {
    VkDevice device = vulkan_->device();
    
    if (mapped && *mapped) {
        vkUnmapMemory(device, memory);
        *mapped = nullptr;
    }
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
}

Status DescriptorManager::create_frame_resources(FrameDescriptors& frame) {
    VkDevice device = vulkan_->device();
    
    // Create constant buffers
    u64 vertex_const_size = MAX_FLOAT_CONSTANTS * 4 * sizeof(f32);  // 256 vec4
    u64 pixel_const_size = MAX_FLOAT_CONSTANTS * 4 * sizeof(f32);
    u64 bool_const_size = MAX_BOOL_CONSTANTS * sizeof(u32);
    u64 loop_const_size = MAX_LOOP_CONSTANTS * sizeof(u32);
    
    frame.vertex_constants_buffer = create_buffer(
        vertex_const_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        frame.vertex_constants_memory, &frame.vertex_constants_mapped
    );
    if (frame.vertex_constants_buffer == VK_NULL_HANDLE) return Status::ErrorInit;
    
    frame.pixel_constants_buffer = create_buffer(
        pixel_const_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        frame.pixel_constants_memory, &frame.pixel_constants_mapped
    );
    if (frame.pixel_constants_buffer == VK_NULL_HANDLE) return Status::ErrorInit;
    
    frame.bool_constants_buffer = create_buffer(
        bool_const_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        frame.bool_constants_memory, &frame.bool_constants_mapped
    );
    if (frame.bool_constants_buffer == VK_NULL_HANDLE) return Status::ErrorInit;
    
    frame.loop_constants_buffer = create_buffer(
        loop_const_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        frame.loop_constants_memory, &frame.loop_constants_mapped
    );
    if (frame.loop_constants_buffer == VK_NULL_HANDLE) return Status::ErrorInit;
    
    // Initialize buffers to zero
    memset(frame.vertex_constants_mapped, 0, vertex_const_size);
    memset(frame.pixel_constants_mapped, 0, pixel_const_size);
    memset(frame.bool_constants_mapped, 0, bool_const_size);
    memset(frame.loop_constants_mapped, 0, loop_const_size);
    
    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout_;
    
    if (vkAllocateDescriptorSets(device, &alloc_info, &frame.set) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    // Write initial descriptor bindings
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffer_infos;
    std::vector<VkDescriptorImageInfo> image_infos;
    
    buffer_infos.resize(4);
    image_infos.resize(MAX_TEXTURE_BINDINGS);
    
    // Vertex constants
    buffer_infos[0] = {frame.vertex_constants_buffer, 0, vertex_const_size};
    VkWriteDescriptorSet write_vertex{};
    write_vertex.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_vertex.dstSet = frame.set;
    write_vertex.dstBinding = 0;
    write_vertex.descriptorCount = 1;
    write_vertex.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write_vertex.pBufferInfo = &buffer_infos[0];
    writes.push_back(write_vertex);
    
    // Pixel constants
    buffer_infos[1] = {frame.pixel_constants_buffer, 0, pixel_const_size};
    VkWriteDescriptorSet write_pixel{};
    write_pixel.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_pixel.dstSet = frame.set;
    write_pixel.dstBinding = 1;
    write_pixel.descriptorCount = 1;
    write_pixel.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write_pixel.pBufferInfo = &buffer_infos[1];
    writes.push_back(write_pixel);
    
    // Bool constants
    buffer_infos[2] = {frame.bool_constants_buffer, 0, bool_const_size};
    VkWriteDescriptorSet write_bool{};
    write_bool.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_bool.dstSet = frame.set;
    write_bool.dstBinding = 2;
    write_bool.descriptorCount = 1;
    write_bool.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write_bool.pBufferInfo = &buffer_infos[2];
    writes.push_back(write_bool);
    
    // Loop constants
    buffer_infos[3] = {frame.loop_constants_buffer, 0, loop_const_size};
    VkWriteDescriptorSet write_loop{};
    write_loop.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_loop.dstSet = frame.set;
    write_loop.dstBinding = 3;
    write_loop.descriptorCount = 1;
    write_loop.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write_loop.pBufferInfo = &buffer_infos[3];
    writes.push_back(write_loop);
    
    // Default textures
    for (u32 i = 0; i < MAX_TEXTURE_BINDINGS; i++) {
        image_infos[i] = {default_sampler_, default_texture_view_, 
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        
        VkWriteDescriptorSet write_tex{};
        write_tex.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_tex.dstSet = frame.set;
        write_tex.dstBinding = 4 + i;
        write_tex.descriptorCount = 1;
        write_tex.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_tex.pImageInfo = &image_infos[i];
        writes.push_back(write_tex);
    }
    
    vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    
    frame.needs_update = false;
    return Status::Ok;
}

void DescriptorManager::destroy_frame_resources(FrameDescriptors& frame) {
    destroy_buffer(frame.vertex_constants_buffer, frame.vertex_constants_memory, 
                   &frame.vertex_constants_mapped);
    destroy_buffer(frame.pixel_constants_buffer, frame.pixel_constants_memory,
                   &frame.pixel_constants_mapped);
    destroy_buffer(frame.bool_constants_buffer, frame.bool_constants_memory,
                   &frame.bool_constants_mapped);
    destroy_buffer(frame.loop_constants_buffer, frame.loop_constants_memory,
                   &frame.loop_constants_mapped);
    
    frame.set = VK_NULL_HANDLE;
}

VkDescriptorSet DescriptorManager::begin_frame(u32 frame_index) {
    if (frame_index >= MAX_FRAMES) {
        return VK_NULL_HANDLE;
    }
    return frames_[frame_index].set;
}

void DescriptorManager::update_vertex_constants(u32 frame_index, const f32* constants, u32 count) {
    if (frame_index >= MAX_FRAMES || !constants || count == 0) return;
    
    auto& frame = frames_[frame_index];
    if (frame.vertex_constants_mapped) {
        u32 copy_count = count < MAX_FLOAT_CONSTANTS * 4 ? count : MAX_FLOAT_CONSTANTS * 4;
        memcpy(frame.vertex_constants_mapped, constants, copy_count * sizeof(f32));
    }
}

void DescriptorManager::update_pixel_constants(u32 frame_index, const f32* constants, u32 count) {
    if (frame_index >= MAX_FRAMES || !constants || count == 0) return;
    
    auto& frame = frames_[frame_index];
    if (frame.pixel_constants_mapped) {
        u32 copy_count = count < MAX_FLOAT_CONSTANTS * 4 ? count : MAX_FLOAT_CONSTANTS * 4;
        memcpy(frame.pixel_constants_mapped, constants, copy_count * sizeof(f32));
    }
}

void DescriptorManager::update_bool_constants(u32 frame_index, const u32* constants, u32 count) {
    if (frame_index >= MAX_FRAMES || !constants || count == 0) return;
    
    auto& frame = frames_[frame_index];
    if (frame.bool_constants_mapped) {
        u32 copy_count = count < MAX_BOOL_CONSTANTS ? count : MAX_BOOL_CONSTANTS;
        memcpy(frame.bool_constants_mapped, constants, copy_count * sizeof(u32));
    }
}

void DescriptorManager::update_loop_constants(u32 frame_index, const u32* constants, u32 count) {
    if (frame_index >= MAX_FRAMES || !constants || count == 0) return;
    
    auto& frame = frames_[frame_index];
    if (frame.loop_constants_mapped) {
        u32 copy_count = count < MAX_LOOP_CONSTANTS ? count : MAX_LOOP_CONSTANTS;
        memcpy(frame.loop_constants_mapped, constants, copy_count * sizeof(u32));
    }
}

void DescriptorManager::bind_textures(u32 frame_index, const VkImageView* views,
                                       const VkSampler* samplers, u32 count) {
    if (frame_index >= MAX_FRAMES || count == 0) return;
    
    VkDevice device = vulkan_->device();
    auto& frame = frames_[frame_index];
    
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> image_infos(count);
    
    for (u32 i = 0; i < count && i < MAX_TEXTURE_BINDINGS; i++) {
        VkImageView view = views[i] ? views[i] : default_texture_view_;
        VkSampler sampler = samplers[i] ? samplers[i] : default_sampler_;
        
        image_infos[i] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frame.set;
        write.dstBinding = 4 + i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_infos[i];
        writes.push_back(write);
    }
    
    if (!writes.empty()) {
        vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }
}

} // namespace x360mu
