/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Texture Cache Implementation
 */

#include "texture_cache.h"
#include "vulkan/vulkan_backend.h"
#include "memory/memory.h"
#include <cstring>
#include <algorithm>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-texcache"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[TEXCACHE] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[TEXCACHE ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// Xbox 360 Texture Tiling Constants
//=============================================================================

// Xbox 360 textures use a specific tiled memory layout for performance
// This is based on Xenia and ATI documentation

namespace tiling {

// Get the morton code for a 2D coordinate
inline u32 encode_morton_2d(u32 x, u32 y) {
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    
    y = (y | (y << 8)) & 0x00FF00FF;
    y = (y | (y << 4)) & 0x0F0F0F0F;
    y = (y | (y << 2)) & 0x33333333;
    y = (y | (y << 1)) & 0x55555555;
    
    return x | (y << 1);
}

// Tile dimensions for different formats
constexpr u32 TILE_WIDTH = 32;
constexpr u32 TILE_HEIGHT = 32;
constexpr u32 MICRO_TILE_WIDTH = 8;
constexpr u32 MICRO_TILE_HEIGHT = 8;

}  // namespace tiling

//=============================================================================
// TextureCacheImpl Implementation
//=============================================================================

TextureCacheImpl::TextureCacheImpl() = default;

TextureCacheImpl::~TextureCacheImpl() {
    shutdown();
}

Status TextureCacheImpl::initialize(VulkanBackend* vulkan, Memory* memory) {
    vulkan_ = vulkan;
    memory_ = memory;
    
    // Create staging buffer for texture uploads
    if (create_staging_buffer() != Status::Ok) {
        LOGE("Failed to create staging buffer");
        return Status::ErrorInit;
    }
    
    LOGI("Texture cache initialized (staging buffer: %lluMB)", 
         (unsigned long long)(STAGING_SIZE / (1024 * 1024)));
    return Status::Ok;
}

void TextureCacheImpl::shutdown() {
    if (vulkan_ == nullptr) return;
    
    clear();
    destroy_staging_buffer();
    
    // Destroy all samplers
    VkDevice device = vulkan_->device();
    for (auto& [hash, sampler] : sampler_cache_) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
        }
    }
    sampler_cache_.clear();
    
    vulkan_ = nullptr;
    memory_ = nullptr;
    
    LOGI("Texture cache shutdown (%llu textures created, %llu bytes uploaded)",
         stats_.textures_created, stats_.bytes_uploaded);
}

Status TextureCacheImpl::create_staging_buffer() {
    VulkanBuffer buf = vulkan_->create_buffer(
        STAGING_SIZE,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    
    if (buf.buffer == VK_NULL_HANDLE) {
        return Status::ErrorInit;
    }
    
    staging_buffer_ = buf.buffer;
    staging_memory_ = buf.memory;
    staging_mapped_ = buf.mapped;
    
    return Status::Ok;
}

void TextureCacheImpl::destroy_staging_buffer() {
    if (vulkan_ == nullptr) return;
    
    VkDevice device = vulkan_->device();
    
    if (staging_mapped_) {
        vkUnmapMemory(device, staging_memory_);
        staging_mapped_ = nullptr;
    }
    
    if (staging_buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, staging_buffer_, nullptr);
        staging_buffer_ = VK_NULL_HANDLE;
    }
    
    if (staging_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, staging_memory_, nullptr);
        staging_memory_ = VK_NULL_HANDLE;
    }
}

u64 TextureCacheImpl::compute_texture_hash(const FetchConstant& fetch) {
    // Hash based on address, dimensions, and format
    u64 hash = 14695981039346656037ULL;
    
    GuestAddr addr = fetch.texture_address();
    u32 width = fetch.texture_width();
    u32 height = fetch.texture_height();
    TextureFormat fmt = fetch.texture_format();
    
    hash ^= addr;
    hash *= 1099511628211ULL;
    hash ^= (static_cast<u64>(width) << 32) | height;
    hash *= 1099511628211ULL;
    hash ^= static_cast<u32>(fmt);
    hash *= 1099511628211ULL;
    
    return hash;
}

const CachedTexture* TextureCacheImpl::get_texture(const FetchConstant& fetch) {
    if (!vulkan_ || !memory_) {
        return nullptr;
    }
    
    GuestAddr addr = fetch.texture_address();
    if (addr == 0) {
        return nullptr;
    }
    
    u64 hash = compute_texture_hash(fetch);
    
    // Check cache
    {
        std::lock_guard<std::mutex> lock(texture_mutex_);
        auto it = texture_cache_.find(hash);
        if (it != texture_cache_.end()) {
            it->second.last_use_frame = current_frame_;
            stats_.texture_cache_hits++;
            return &it->second;
        }
    }
    
    // Create new texture
    CachedTexture texture = create_texture(fetch);
    if (!texture.is_valid()) {
        return nullptr;
    }
    
    texture.last_use_frame = current_frame_;
    
    std::lock_guard<std::mutex> lock(texture_mutex_);
    auto [it, inserted] = texture_cache_.emplace(hash, std::move(texture));
    
    stats_.textures_created++;
    
    LOGD("Created texture: %ux%u, format=%u, addr=%08x",
         it->second.width, it->second.height,
         static_cast<u32>(it->second.format), addr);
    
    return &it->second;
}

CachedTexture TextureCacheImpl::create_texture(const FetchConstant& fetch) {
    CachedTexture texture{};
    
    texture.address = fetch.texture_address();
    texture.width = fetch.texture_width();
    texture.height = fetch.texture_height();
    texture.depth = 1;  // TODO: 3D textures
    texture.format = fetch.texture_format();
    texture.mip_levels = 1;  // TODO: mipmaps
    texture.array_layers = 1;
    texture.is_tiled = true;  // Xbox 360 textures are usually tiled
    
    VkFormat vk_format = translate_format(texture.format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        LOGE("Unsupported texture format: %u", static_cast<u32>(texture.format));
        return texture;
    }
    
    VkDevice device = vulkan_->device();
    
    // Create image
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = vk_format;
    image_info.extent.width = texture.width;
    image_info.extent.height = texture.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = texture.mip_levels;
    image_info.arrayLayers = texture.array_layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    if (vkCreateImage(device, &image_info, nullptr, &texture.image) != VK_SUCCESS) {
        LOGE("Failed to create image");
        return texture;
    }
    
    // Allocate memory
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, texture.image, &mem_reqs);
    
    u32 mem_type = vulkan_->find_memory_type(mem_reqs.memoryTypeBits, 
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = mem_type;
    
    if (vkAllocateMemory(device, &alloc_info, nullptr, &texture.memory) != VK_SUCCESS) {
        LOGE("Failed to allocate image memory");
        vkDestroyImage(device, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
        return texture;
    }
    
    vkBindImageMemory(device, texture.image, texture.memory, 0);
    
    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = vk_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = texture.mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = texture.array_layers;
    
    if (vkCreateImageView(device, &view_info, nullptr, &texture.view) != VK_SUCCESS) {
        LOGE("Failed to create image view");
        vkDestroyImage(device, texture.image, nullptr);
        vkFreeMemory(device, texture.memory, nullptr);
        texture.image = VK_NULL_HANDLE;
        texture.memory = VK_NULL_HANDLE;
        return texture;
    }
    
    // Upload texture data
    u32 bytes_per_block = get_bytes_per_block(texture.format);
    u32 block_size = get_block_size(texture.format);
    
    u32 blocks_x = (texture.width + block_size - 1) / block_size;
    u32 blocks_y = (texture.height + block_size - 1) / block_size;
    u64 data_size = blocks_x * blocks_y * bytes_per_block;
    
    if (data_size > STAGING_SIZE) {
        LOGE("Texture too large for staging buffer");
        // Continue anyway, texture will have garbage
    } else if (memory_ && staging_mapped_) {
        // Read texture data from guest memory
        const void* src_data = memory_->get_host_ptr(texture.address);
        if (src_data) {
            // Detile if necessary
            detile_texture(src_data, staging_mapped_, texture.width, texture.height,
                          texture.format, texture.is_tiled);
            
            // Upload to GPU
            upload_texture(texture, staging_mapped_, data_size);
            stats_.bytes_uploaded += data_size;
        }
    }
    
    texture.hash = compute_texture_hash(fetch);
    return texture;
}

void TextureCacheImpl::upload_texture(CachedTexture& texture, const void* data, u64 size) {
    VkDevice device = vulkan_->device();
    
    // Get command buffer for transfer
    VkCommandPool pool;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = vulkan_->graphics_queue_family();
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(device, &pool_info, nullptr, &pool);
    
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &alloc_info, &cmd);
    
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);
    
    // Transition image to transfer destination
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = texture.mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = texture.array_layers;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Copy buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {texture.width, texture.height, 1};
    
    vkCmdCopyBufferToImage(cmd, staging_buffer_, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // Transition to shader read
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    vkEndCommandBuffer(cmd);
    
    // Submit and wait
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    
    VkQueue queue;
    vkGetDeviceQueue(device, vulkan_->graphics_queue_family(), 0, &queue);
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    
    vkDestroyCommandPool(device, pool, nullptr);
}

void TextureCacheImpl::destroy_texture(CachedTexture& texture) {
    if (!vulkan_) return;
    
    VkDevice device = vulkan_->device();
    
    if (texture.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, texture.sampler, nullptr);
    }
    if (texture.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, texture.view, nullptr);
    }
    if (texture.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, texture.image, nullptr);
    }
    if (texture.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, texture.memory, nullptr);
    }
    
    texture.image = VK_NULL_HANDLE;
    texture.memory = VK_NULL_HANDLE;
    texture.view = VK_NULL_HANDLE;
    texture.sampler = VK_NULL_HANDLE;
}

VkFormat TextureCacheImpl::translate_format(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_8:
            return VK_FORMAT_R8_UNORM;
        case TextureFormat::k_1_5_5_5:
            return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case TextureFormat::k_5_6_5:
            return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case TextureFormat::k_8_8_8_8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::k_2_10_10_10:
            return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case TextureFormat::k_8_A:
            return VK_FORMAT_R8_UNORM;
        case TextureFormat::k_8_B:
            return VK_FORMAT_R8_UNORM;
        case TextureFormat::k_8_8:
            return VK_FORMAT_R8G8_UNORM;
        case TextureFormat::k_DXT1:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case TextureFormat::k_DXT2_3:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case TextureFormat::k_DXT4_5:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case TextureFormat::k_CTX1:
            return VK_FORMAT_R8G8_UNORM;  // Approximation
        case TextureFormat::k_DXN:
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case TextureFormat::k_16:
            return VK_FORMAT_R16_UNORM;
        case TextureFormat::k_16_16:
            return VK_FORMAT_R16G16_UNORM;
        case TextureFormat::k_16_16_16_16:
            return VK_FORMAT_R16G16B16A16_UNORM;
        case TextureFormat::k_16_FLOAT:
            return VK_FORMAT_R16_SFLOAT;
        case TextureFormat::k_16_16_FLOAT:
            return VK_FORMAT_R16G16_SFLOAT;
        case TextureFormat::k_16_16_16_16_FLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureFormat::k_32_FLOAT:
            return VK_FORMAT_R32_SFLOAT;
        case TextureFormat::k_32_32_FLOAT:
            return VK_FORMAT_R32G32_SFLOAT;
        case TextureFormat::k_32_32_32_32_FLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

u32 TextureCacheImpl::get_bytes_per_block(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_8:
        case TextureFormat::k_8_A:
        case TextureFormat::k_8_B:
            return 1;
        case TextureFormat::k_1_5_5_5:
        case TextureFormat::k_5_6_5:
        case TextureFormat::k_6_5_5:
        case TextureFormat::k_8_8:
        case TextureFormat::k_16:
        case TextureFormat::k_16_FLOAT:
            return 2;
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_2_10_10_10:
        case TextureFormat::k_16_16:
        case TextureFormat::k_16_16_FLOAT:
        case TextureFormat::k_32_FLOAT:
            return 4;
        case TextureFormat::k_DXT1:
        case TextureFormat::k_CTX1:
            return 8;  // 8 bytes per 4x4 block
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXN:
            return 16;  // 16 bytes per 4x4 block
        case TextureFormat::k_16_16_16_16:
        case TextureFormat::k_16_16_16_16_FLOAT:
        case TextureFormat::k_32_32_FLOAT:
            return 8;
        case TextureFormat::k_32_32_32_32_FLOAT:
            return 16;
        default:
            return 4;
    }
}

u32 TextureCacheImpl::get_block_size(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_CTX1:
        case TextureFormat::k_DXN:
            return 4;  // 4x4 blocks for compressed formats
        default:
            return 1;  // 1x1 for uncompressed
    }
}

void TextureCacheImpl::detile_texture(const void* src, void* dst, u32 width, u32 height,
                                      TextureFormat format, bool is_tiled) {
    u32 bytes_per_block = get_bytes_per_block(format);
    u32 block_size = get_block_size(format);
    
    u32 blocks_x = (width + block_size - 1) / block_size;
    u32 blocks_y = (height + block_size - 1) / block_size;
    
    if (!is_tiled) {
        // Just copy directly
        memcpy(dst, src, blocks_x * blocks_y * bytes_per_block);
        return;
    }
    
    // Xbox 360 uses morton order (Z-order) within tiles
    const u8* src_bytes = static_cast<const u8*>(src);
    u8* dst_bytes = static_cast<u8*>(dst);
    
    // For each block
    for (u32 by = 0; by < blocks_y; by++) {
        for (u32 bx = 0; bx < blocks_x; bx++) {
            // Calculate morton index for this block within its micro-tile
            u32 micro_x = bx % tiling::MICRO_TILE_WIDTH;
            u32 micro_y = by % tiling::MICRO_TILE_HEIGHT;
            u32 morton = tiling::encode_morton_2d(micro_x, micro_y);
            
            // Calculate macro tile indices
            u32 macro_x = bx / tiling::MICRO_TILE_WIDTH;
            u32 macro_y = by / tiling::MICRO_TILE_HEIGHT;
            u32 macro_tiles_x = (blocks_x + tiling::MICRO_TILE_WIDTH - 1) / tiling::MICRO_TILE_WIDTH;
            
            // Calculate source offset
            u32 micro_tile_offset = (macro_y * macro_tiles_x + macro_x) * 
                                    tiling::MICRO_TILE_WIDTH * tiling::MICRO_TILE_HEIGHT;
            u32 src_offset = (micro_tile_offset + morton) * bytes_per_block;
            
            // Calculate destination offset (linear)
            u32 dst_offset = (by * blocks_x + bx) * bytes_per_block;
            
            // Copy the block
            memcpy(dst_bytes + dst_offset, src_bytes + src_offset, bytes_per_block);
        }
    }
}

VkSampler TextureCacheImpl::get_sampler(const VkSamplerConfig& state) {
    u64 hash = state.compute_hash();
    
    {
        std::lock_guard<std::mutex> lock(sampler_mutex_);
        auto it = sampler_cache_.find(hash);
        if (it != sampler_cache_.end()) {
            return it->second;
        }
    }
    
    // Create new sampler
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = state.mag_filter;
    info.minFilter = state.min_filter;
    info.mipmapMode = state.mipmap_mode;
    info.addressModeU = state.address_u;
    info.addressModeV = state.address_v;
    info.addressModeW = state.address_w;
    info.mipLodBias = state.mip_lod_bias;
    info.anisotropyEnable = state.max_anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = state.max_anisotropy;
    info.compareEnable = VK_FALSE;
    info.compareOp = VK_COMPARE_OP_NEVER;
    info.minLod = 0.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    info.borderColor = state.border_color;
    info.unnormalizedCoordinates = VK_FALSE;
    
    VkSampler sampler;
    if (vkCreateSampler(vulkan_->device(), &info, nullptr, &sampler) != VK_SUCCESS) {
        LOGE("Failed to create sampler");
        return VK_NULL_HANDLE;
    }
    
    std::lock_guard<std::mutex> lock(sampler_mutex_);
    sampler_cache_[hash] = sampler;
    
    return sampler;
}

void TextureCacheImpl::invalidate(GuestAddr base, u64 size) {
    std::lock_guard<std::mutex> lock(texture_mutex_);
    
    std::vector<u64> to_remove;
    
    for (auto& [hash, texture] : texture_cache_) {
        GuestAddr tex_addr = texture.address;
        // Estimate texture size (simplified)
        u64 tex_size = texture.width * texture.height * 4;
        
        // Check for overlap
        if (tex_addr < base + size && tex_addr + tex_size > base) {
            to_remove.push_back(hash);
        }
    }
    
    for (u64 hash : to_remove) {
        auto it = texture_cache_.find(hash);
        if (it != texture_cache_.end()) {
            destroy_texture(it->second);
            texture_cache_.erase(it);
            stats_.textures_invalidated++;
        }
    }
    
    if (!to_remove.empty()) {
        LOGD("Invalidated %zu textures in range %08x-%08x",
             to_remove.size(), base, base + size);
    }
}

void TextureCacheImpl::clear() {
    std::lock_guard<std::mutex> lock(texture_mutex_);
    
    for (auto& [hash, texture] : texture_cache_) {
        destroy_texture(texture);
    }
    texture_cache_.clear();
    
    LOGI("Texture cache cleared");
}

void TextureCacheImpl::begin_frame(u64 frame_number) {
    current_frame_ = frame_number;
    
    // Evict old textures (not used in last 60 frames)
    constexpr u64 MAX_AGE = 60;
    
    std::lock_guard<std::mutex> lock(texture_mutex_);
    
    std::vector<u64> to_remove;
    for (auto& [hash, texture] : texture_cache_) {
        if (current_frame_ - texture.last_use_frame > MAX_AGE) {
            to_remove.push_back(hash);
        }
    }
    
    for (u64 hash : to_remove) {
        auto it = texture_cache_.find(hash);
        if (it != texture_cache_.end()) {
            destroy_texture(it->second);
            texture_cache_.erase(it);
        }
    }
}

} // namespace x360mu
