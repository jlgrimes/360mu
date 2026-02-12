/**
 * 360μ - Xbox 360 Emulator for Android
 *
 * Texture Cache Implementation
 *
 * Handles Xbox 360 → Vulkan texture format conversion, tiling/untiling,
 * mipmap chain upload, byte-swapping, sampler state translation,
 * and cache invalidation.
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

namespace tiling {

// Xbox 360 tiling geometry:
// - Macro tiles: 32x32 blocks (for block-compressed) or 32x32 texels
// - Micro tiles: 8x8 within each macro tile, addressed via Morton (Z-curve)
// - Macro tiles are laid out row-major across the surface
// - Within each macro tile, micro tiles use Morton order
// - Within each micro tile, elements are in Morton order

constexpr u32 TILE_WIDTH = 32;
constexpr u32 TILE_HEIGHT = 32;
constexpr u32 MICRO_TILE_WIDTH = 8;
constexpr u32 MICRO_TILE_HEIGHT = 8;
constexpr u32 MICRO_TILES_PER_MACRO = (TILE_WIDTH / MICRO_TILE_WIDTH) *
                                       (TILE_HEIGHT / MICRO_TILE_HEIGHT);  // 16

// Bit-interleave x and y to produce a Morton code (Z-order curve index)
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

// 3D Morton code: interleave x, y, z
inline u32 encode_morton_3d(u32 x, u32 y, u32 z) {
    auto part1by2 = [](u32 n) -> u32 {
        n &= 0x000003FF;  // 10 bits
        n = (n ^ (n << 16)) & 0xFF0000FF;
        n = (n ^ (n <<  8)) & 0x0300F00F;
        n = (n ^ (n <<  4)) & 0x030C30C3;
        n = (n ^ (n <<  2)) & 0x09249249;
        return n;
    };
    return part1by2(x) | (part1by2(y) << 1) | (part1by2(z) << 2);
}

/**
 * Compute the tiled byte offset for block (bx, by) in a surface that is
 * blocks_x blocks wide, with each block being bytes_per_block bytes.
 *
 * Xbox 360 tiling layout:
 *   1. Divide surface into 32x32-block macro tiles (row-major order)
 *   2. Within each macro tile, divide into 8x8-block micro tiles (Morton order)
 *   3. Within each micro tile, individual blocks are in Morton order
 */
inline u32 get_tiled_offset_2d(u32 bx, u32 by, u32 blocks_x, u32 bytes_per_block) {
    // Macro tile coordinates
    u32 macro_x = bx / TILE_WIDTH;
    u32 macro_y = by / TILE_HEIGHT;
    u32 macro_tiles_x = (blocks_x + TILE_WIDTH - 1) / TILE_WIDTH;

    // Byte offset to the start of this macro tile
    u32 macro_tile_size = TILE_WIDTH * TILE_HEIGHT * bytes_per_block;
    u32 macro_offset = (macro_y * macro_tiles_x + macro_x) * macro_tile_size;

    // Local position within macro tile
    u32 local_x = bx % TILE_WIDTH;
    u32 local_y = by % TILE_HEIGHT;

    // Micro tile coordinates within the macro tile
    u32 micro_tile_x = local_x / MICRO_TILE_WIDTH;
    u32 micro_tile_y = local_y / MICRO_TILE_HEIGHT;
    u32 micro_tiles_per_row = TILE_WIDTH / MICRO_TILE_WIDTH;  // 4

    // Morton index of the micro tile within the macro tile
    u32 micro_tile_idx = encode_morton_2d(micro_tile_x, micro_tile_y);
    u32 micro_tile_size = MICRO_TILE_WIDTH * MICRO_TILE_HEIGHT * bytes_per_block;
    u32 micro_offset = micro_tile_idx * micro_tile_size;

    // Morton index of the block within the micro tile
    u32 element_x = local_x % MICRO_TILE_WIDTH;
    u32 element_y = local_y % MICRO_TILE_HEIGHT;
    u32 element_idx = encode_morton_2d(element_x, element_y);

    return macro_offset + micro_offset + element_idx * bytes_per_block;
}

/**
 * Compute the tiled offset for a 3D texture element.
 * Xbox 360 3D textures tile each Z-slice independently as a 2D surface,
 * then stack slices contiguously.
 */
inline u32 get_tiled_offset_3d(u32 bx, u32 by, u32 bz,
                                u32 blocks_x, u32 blocks_y, u32 bytes_per_block) {
    u32 slice_size_blocks = ((blocks_x + TILE_WIDTH - 1) / TILE_WIDTH) *
                             ((blocks_y + TILE_HEIGHT - 1) / TILE_HEIGHT) *
                             TILE_WIDTH * TILE_HEIGHT;
    u32 slice_offset = bz * slice_size_blocks * bytes_per_block;
    return slice_offset + get_tiled_offset_2d(bx, by, blocks_x, bytes_per_block);
}

/**
 * Calculate the packed mip tail offset.
 * On Xbox 360, mip levels smaller than a macro tile (32x32 blocks) are packed
 * together into a single tile. This returns the byte offset within the packed
 * mip tail for a given mip level.
 */
inline u32 packed_mip_offset(u32 mip_width, u32 mip_height, u32 mip_index,
                              u32 bytes_per_block, u32 block_size) {
    // Mips are packed in Morton order within the tail tile.
    // Each sub-mip occupies a quadrant of progressively smaller size.
    // For simplicity: pack sequentially with alignment to block boundaries.
    u32 offset = 0;
    u32 w = mip_width;
    u32 h = mip_height;
    for (u32 i = 0; i < mip_index; i++) {
        u32 bx = (w + block_size - 1) / block_size;
        u32 by = (h + block_size - 1) / block_size;
        offset += bx * by * bytes_per_block;
        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
    }
    return offset;
}

/**
 * Check if a mip level is part of the packed mip tail.
 * Mips whose block dimensions are both < 32 are packed into the tail.
 */
inline bool is_packed_mip(u32 width, u32 height, u32 block_size) {
    u32 blocks_x = (width + block_size - 1) / block_size;
    u32 blocks_y = (height + block_size - 1) / block_size;
    return blocks_x < TILE_WIDTH && blocks_y < TILE_HEIGHT;
}

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

//=============================================================================
// Hash and Lookup
//=============================================================================

u64 TextureCacheImpl::compute_texture_hash(const FetchConstant& fetch) {
    // FNV-1a hash including all texture-relevant fields
    u64 hash = 14695981039346656037ULL;

    auto fold = [&hash](u64 v) {
        hash ^= v;
        hash *= 1099511628211ULL;
    };

    fold(fetch.texture_address());
    fold((static_cast<u64>(fetch.texture_width()) << 32) | fetch.texture_height());
    fold(static_cast<u32>(fetch.texture_format()));
    fold(fetch.texture_mip_levels());
    fold(fetch.texture_is_tiled() ? 1 : 0);
    fold(static_cast<u32>(fetch.texture_dimension()));

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

    LOGD("Created texture: %ux%u, fmt=%u, mips=%u, tiled=%d, addr=%08x",
         it->second.width, it->second.height,
         static_cast<u32>(it->second.format), it->second.mip_levels,
         it->second.is_tiled, addr);

    return &it->second;
}

//=============================================================================
// Texture Creation
//=============================================================================

CachedTexture TextureCacheImpl::create_texture(const FetchConstant& fetch) {
    CachedTexture texture{};

    texture.address = fetch.texture_address();
    texture.width = fetch.texture_width();
    texture.height = fetch.texture_height();
    texture.format = fetch.texture_format();
    texture.is_tiled = fetch.texture_is_tiled();
    texture.array_layers = 1;

    // Extract mip levels from fetch constant
    texture.mip_levels = fetch.texture_mip_levels();
    // Clamp to valid range (can't have more mips than log2(max_dim)+1)
    u32 max_dim = std::max(texture.width, texture.height);
    u32 max_mips = 1;
    while ((1u << max_mips) <= max_dim) max_mips++;
    texture.mip_levels = std::min(texture.mip_levels, max_mips);

    // Handle texture dimension
    TextureDimension dim = fetch.texture_dimension();
    switch (dim) {
        case TextureDimension::k3D:
            texture.depth = fetch.texture_depth();
            break;
        case TextureDimension::kCube:
            texture.depth = 1;
            texture.array_layers = 6;
            break;
        default:
            texture.depth = 1;
            break;
    }

    VkFormat vk_format = translate_format(texture.format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        LOGE("Unsupported texture format: %u", static_cast<u32>(texture.format));
        return texture;
    }

    VkDevice device = vulkan_->device();

    // Create image
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = (dim == TextureDimension::k3D) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    image_info.format = vk_format;
    image_info.extent.width = texture.width;
    image_info.extent.height = texture.height;
    image_info.extent.depth = texture.depth;
    image_info.mipLevels = texture.mip_levels;
    image_info.arrayLayers = texture.array_layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dim == TextureDimension::kCube) {
        image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

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
    view_info.format = vk_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = texture.mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = texture.array_layers;

    switch (dim) {
        case TextureDimension::k3D:
            view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
            break;
        case TextureDimension::kCube:
            view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            break;
        default:
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            break;
    }

    if (vkCreateImageView(device, &view_info, nullptr, &texture.view) != VK_SUCCESS) {
        LOGE("Failed to create image view");
        vkDestroyImage(device, texture.image, nullptr);
        vkFreeMemory(device, texture.memory, nullptr);
        texture.image = VK_NULL_HANDLE;
        texture.memory = VK_NULL_HANDLE;
        return texture;
    }

    // Upload texture data (all mip levels)
    u64 total_data_size = compute_texture_data_size(texture.width, texture.height,
                                                     texture.format, texture.mip_levels);

    if (total_data_size > STAGING_SIZE) {
        LOGE("Texture too large for staging buffer (%llu bytes)", (unsigned long long)total_data_size);
    } else if (memory_ && staging_mapped_) {
        const void* src_data = memory_->get_host_ptr(texture.address);
        if (src_data) {
            // Detile based on texture dimension type
            TextureDimension dim = fetch.texture_dimension();

            if (dim == TextureDimension::k3D && texture.depth > 1) {
                detile_texture_3d(src_data, staging_mapped_,
                                  texture.width, texture.height, texture.depth,
                                  texture.format, texture.is_tiled);
            } else if (dim == TextureDimension::kCube && texture.array_layers == 6) {
                detile_texture_cube(src_data, staging_mapped_,
                                    texture.width, texture.height,
                                    texture.format, texture.is_tiled);
            } else {
                // 2D or 1D texture
                detile_texture(src_data, staging_mapped_,
                              texture.width, texture.height,
                              texture.format, texture.is_tiled);
            }

            // Xbox 360 is big-endian; byte-swap the texture data
            byte_swap_texture_data(staging_mapped_, total_data_size, texture.format);

            // Upload all mip levels
            upload_texture_mips(texture, staging_mapped_, total_data_size);
            stats_.bytes_uploaded += total_data_size;
        }
    }

    // Create sampler from fetch constant
    VkSamplerConfig sampler_config = translate_sampler(fetch);
    texture.sampler = get_sampler(sampler_config);

    texture.hash = compute_texture_hash(fetch);
    return texture;
}

//=============================================================================
// Texture Upload (single mip, legacy)
//=============================================================================

void TextureCacheImpl::upload_texture(CachedTexture& texture, const void* data, u64 size) {
    // Delegate to mip-aware upload with single mip
    upload_texture_mips(texture, data, size);
}

//=============================================================================
// Mipmap-aware Texture Upload
//=============================================================================

void TextureCacheImpl::upload_texture_mips(CachedTexture& texture, const void* data, u64 total_size) {
    (void)total_size;
    VkDevice device = vulkan_->device();

    VkCommandPool pool;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = vulkan_->graphics_queue_family();
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(device, &pool_info, nullptr, &pool);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition all mip levels to TRANSFER_DST
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

    // Build copy regions for each mip level
    u32 bpb = get_bytes_per_block(texture.format);
    u32 bs = get_block_size(texture.format);
    u64 buffer_offset = 0;

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(texture.mip_levels);

    for (u32 mip = 0; mip < texture.mip_levels; mip++) {
        u32 mip_w = std::max(1u, texture.width >> mip);
        u32 mip_h = std::max(1u, texture.height >> mip);

        u32 blocks_x = (mip_w + bs - 1) / bs;
        u32 blocks_y = (mip_h + bs - 1) / bs;
        u64 mip_size = static_cast<u64>(blocks_x) * blocks_y * bpb;

        VkBufferImageCopy region{};
        region.bufferOffset = buffer_offset;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mip;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = texture.array_layers;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {mip_w, mip_h, texture.depth};

        regions.push_back(region);
        buffer_offset += mip_size;
    }

    vkCmdCopyBufferToImage(cmd, staging_buffer_, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<u32>(regions.size()), regions.data());

    // Transition to SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

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

//=============================================================================
// Byte Swapping (Xbox 360 big-endian → little-endian)
//=============================================================================

void TextureCacheImpl::byte_swap_texture_data(void* data, u64 size, TextureFormat format) {
    u8* bytes = static_cast<u8*>(data);

    switch (format) {
        // 16-bit formats: swap each 2 bytes
        case TextureFormat::k_1_5_5_5:
        case TextureFormat::k_5_6_5:
        case TextureFormat::k_6_5_5:
        case TextureFormat::k_4_4_4_4:
        case TextureFormat::k_8_8:
        case TextureFormat::k_16:
        case TextureFormat::k_16_FLOAT: {
            u16* d16 = reinterpret_cast<u16*>(bytes);
            u64 count = size / 2;
            for (u64 i = 0; i < count; i++) {
                d16[i] = __builtin_bswap16(d16[i]);
            }
            break;
        }

        // 32-bit formats: swap each 4 bytes
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_8_8_8_8_A:
        case TextureFormat::k_8_8_8_8_GAMMA:
        case TextureFormat::k_2_10_10_10:
        case TextureFormat::k_2_10_10_10_GAMMA:
        case TextureFormat::k_10_11_11:
        case TextureFormat::k_11_11_10:
        case TextureFormat::k_16_16:
        case TextureFormat::k_16_16_EXPAND:
        case TextureFormat::k_16_16_FLOAT:
        case TextureFormat::k_32_FLOAT: {
            u32* d32 = reinterpret_cast<u32*>(bytes);
            u64 count = size / 4;
            for (u64 i = 0; i < count; i++) {
                d32[i] = __builtin_bswap32(d32[i]);
            }
            break;
        }

        // 64-bit: swap as pairs of 32-bit words
        case TextureFormat::k_16_16_16_16:
        case TextureFormat::k_16_16_16_16_EXPAND:
        case TextureFormat::k_16_16_16_16_FLOAT:
        case TextureFormat::k_32_32_FLOAT: {
            u32* d32 = reinterpret_cast<u32*>(bytes);
            u64 count = size / 4;
            for (u64 i = 0; i < count; i++) {
                d32[i] = __builtin_bswap32(d32[i]);
            }
            break;
        }

        // 128-bit float
        case TextureFormat::k_32_32_32_32_FLOAT:
        case TextureFormat::k_32_32_32_FLOAT: {
            u32* d32 = reinterpret_cast<u32*>(bytes);
            u64 count = size / 4;
            for (u64 i = 0; i < count; i++) {
                d32[i] = __builtin_bswap32(d32[i]);
            }
            break;
        }

        // Block-compressed: DXT blocks are stored as 16-bit words in big-endian
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
        case TextureFormat::k_CTX1:
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT5A: {
            // DXT1/CTX1: 8 bytes per block, swap as 16-bit words
            u16* d16 = reinterpret_cast<u16*>(bytes);
            u64 count = size / 2;
            for (u64 i = 0; i < count; i++) {
                d16[i] = __builtin_bswap16(d16[i]);
            }
            break;
        }

        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
        case TextureFormat::k_DXN: {
            // 16 bytes per block, swap as 16-bit words
            u16* d16 = reinterpret_cast<u16*>(bytes);
            u64 count = size / 2;
            for (u64 i = 0; i < count; i++) {
                d16[i] = __builtin_bswap16(d16[i]);
            }
            break;
        }

        // 8-bit formats: no swap needed
        case TextureFormat::k_8:
        case TextureFormat::k_8_A:
        case TextureFormat::k_8_B:
        default:
            break;
    }
}

//=============================================================================
// Texture Data Size Calculation
//=============================================================================

u64 TextureCacheImpl::compute_texture_data_size(u32 width, u32 height,
                                                 TextureFormat format, u32 mip_levels) {
    u32 bpb = get_bytes_per_block(format);
    u32 bs = get_block_size(format);
    u64 total = 0;

    for (u32 mip = 0; mip < mip_levels; mip++) {
        u32 mip_w = std::max(1u, width >> mip);
        u32 mip_h = std::max(1u, height >> mip);
        u32 blocks_x = (mip_w + bs - 1) / bs;
        u32 blocks_y = (mip_h + bs - 1) / bs;
        total += static_cast<u64>(blocks_x) * blocks_y * bpb;
    }

    return total;
}

//=============================================================================
// Texture Destruction
//=============================================================================

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

//=============================================================================
// Format Translation
//=============================================================================

VkFormat TextureCacheImpl::translate_format(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_8:
        case TextureFormat::k_8_A:
        case TextureFormat::k_8_B:
            return VK_FORMAT_R8_UNORM;
        case TextureFormat::k_1_5_5_5:
            return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case TextureFormat::k_5_6_5:
            return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case TextureFormat::k_6_5_5:
            return VK_FORMAT_R5G6B5_UNORM_PACK16;  // Closest match
        case TextureFormat::k_4_4_4_4:
            return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
        case TextureFormat::k_8_8:
            return VK_FORMAT_R8G8_UNORM;
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_8_8_8_8_A:
        case TextureFormat::k_8_8_8_8_AS_16_16_16_16:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::k_8_8_8_8_GAMMA:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case TextureFormat::k_2_10_10_10:
        case TextureFormat::k_2_10_10_10_AS_16_16_16_16:
            return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case TextureFormat::k_2_10_10_10_GAMMA:
            return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case TextureFormat::k_10_11_11:
        case TextureFormat::k_10_11_11_AS_16_16_16_16:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case TextureFormat::k_11_11_10:
        case TextureFormat::k_11_11_10_AS_16_16_16_16:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case TextureFormat::k_DXT5A:
            return VK_FORMAT_BC4_UNORM_BLOCK;
        case TextureFormat::k_DXT3A:
            return VK_FORMAT_BC4_UNORM_BLOCK;  // Closest single-channel BC
        case TextureFormat::k_DXN:
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case TextureFormat::k_CTX1:
            return VK_FORMAT_R8G8_UNORM;  // Software decompressed
        case TextureFormat::k_16:
            return VK_FORMAT_R16_UNORM;
        case TextureFormat::k_16_16:
        case TextureFormat::k_16_16_EXPAND:
            return VK_FORMAT_R16G16_UNORM;
        case TextureFormat::k_16_16_16_16:
        case TextureFormat::k_16_16_16_16_EXPAND:
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
        case TextureFormat::k_32_32_32_FLOAT:
            return VK_FORMAT_R32G32B32_SFLOAT;
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
        case TextureFormat::k_1_REVERSE:
        case TextureFormat::k_1:
            return 1;
        case TextureFormat::k_1_5_5_5:
        case TextureFormat::k_5_6_5:
        case TextureFormat::k_6_5_5:
        case TextureFormat::k_4_4_4_4:
        case TextureFormat::k_8_8:
        case TextureFormat::k_16:
        case TextureFormat::k_16_FLOAT:
            return 2;
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_8_8_8_8_A:
        case TextureFormat::k_8_8_8_8_GAMMA:
        case TextureFormat::k_8_8_8_8_AS_16_16_16_16:
        case TextureFormat::k_2_10_10_10:
        case TextureFormat::k_2_10_10_10_GAMMA:
        case TextureFormat::k_2_10_10_10_AS_16_16_16_16:
        case TextureFormat::k_10_11_11:
        case TextureFormat::k_10_11_11_AS_16_16_16_16:
        case TextureFormat::k_11_11_10:
        case TextureFormat::k_11_11_10_AS_16_16_16_16:
        case TextureFormat::k_16_16:
        case TextureFormat::k_16_16_EXPAND:
        case TextureFormat::k_16_16_FLOAT:
        case TextureFormat::k_32_FLOAT:
            return 4;
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
        case TextureFormat::k_CTX1:
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT5A:
            return 8;  // 8 bytes per 4x4 block
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
        case TextureFormat::k_DXN:
            return 16;  // 16 bytes per 4x4 block
        case TextureFormat::k_16_16_16_16:
        case TextureFormat::k_16_16_16_16_EXPAND:
        case TextureFormat::k_16_16_16_16_FLOAT:
        case TextureFormat::k_32_32_FLOAT:
            return 8;
        case TextureFormat::k_32_32_32_FLOAT:
            return 12;
        case TextureFormat::k_32_32_32_32_FLOAT:
            return 16;
        default:
            return 4;
    }
}

u32 TextureCacheImpl::get_block_size(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
        case TextureFormat::k_CTX1:
        case TextureFormat::k_DXN:
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT5A:
            return 4;  // 4x4 blocks
        default:
            return 1;
    }
}

//=============================================================================
// Tiling / Untiling
//=============================================================================

void TextureCacheImpl::detile_texture(const void* src, void* dst, u32 width, u32 height,
                                      TextureFormat format, bool is_tiled) {
    u32 bytes_per_block = get_bytes_per_block(format);
    u32 block_size = get_block_size(format);

    u32 blocks_x = (width + block_size - 1) / block_size;
    u32 blocks_y = (height + block_size - 1) / block_size;
    u64 linear_size = static_cast<u64>(blocks_x) * blocks_y * bytes_per_block;

    if (!is_tiled) {
        memcpy(dst, src, linear_size);
        return;
    }

    const u8* src_bytes = static_cast<const u8*>(src);
    u8* dst_bytes = static_cast<u8*>(dst);

    // Use the proper two-level tiling: macro tiles (32x32) → micro tiles (8x8)
    // → Morton-ordered elements within each tile
    for (u32 by = 0; by < blocks_y; by++) {
        for (u32 bx = 0; bx < blocks_x; bx++) {
            u32 src_offset = tiling::get_tiled_offset_2d(bx, by, blocks_x, bytes_per_block);
            u32 dst_offset = (by * blocks_x + bx) * bytes_per_block;

            memcpy(dst_bytes + dst_offset, src_bytes + src_offset, bytes_per_block);
        }
    }
}

/**
 * Detile a 3D texture slice-by-slice.
 * Each Z-slice is tiled independently as a 2D surface.
 */
void TextureCacheImpl::detile_texture_3d(const void* src, void* dst,
                                          u32 width, u32 height, u32 depth,
                                          TextureFormat format, bool is_tiled) {
    u32 bytes_per_block = get_bytes_per_block(format);
    u32 block_size = get_block_size(format);
    u32 blocks_x = (width + block_size - 1) / block_size;
    u32 blocks_y = (height + block_size - 1) / block_size;
    u64 slice_size = static_cast<u64>(blocks_x) * blocks_y * bytes_per_block;

    if (!is_tiled) {
        memcpy(dst, src, slice_size * depth);
        return;
    }

    const u8* src_bytes = static_cast<const u8*>(src);
    u8* dst_bytes = static_cast<u8*>(dst);

    // Calculate tiled slice size (aligned to macro tile boundaries)
    u32 macro_tiles_x = (blocks_x + tiling::TILE_WIDTH - 1) / tiling::TILE_WIDTH;
    u32 macro_tiles_y = (blocks_y + tiling::TILE_HEIGHT - 1) / tiling::TILE_HEIGHT;
    u64 tiled_slice_size = static_cast<u64>(macro_tiles_x) * macro_tiles_y *
                           tiling::TILE_WIDTH * tiling::TILE_HEIGHT * bytes_per_block;

    for (u32 z = 0; z < depth; z++) {
        const u8* slice_src = src_bytes + z * tiled_slice_size;
        u8* slice_dst = dst_bytes + z * slice_size;

        for (u32 by = 0; by < blocks_y; by++) {
            for (u32 bx = 0; bx < blocks_x; bx++) {
                u32 src_offset = tiling::get_tiled_offset_2d(bx, by, blocks_x, bytes_per_block);
                u32 dst_offset = (by * blocks_x + bx) * bytes_per_block;
                memcpy(slice_dst + dst_offset, slice_src + src_offset, bytes_per_block);
            }
        }
    }
}

/**
 * Detile a cubemap texture. Each face is tiled as an independent 2D surface.
 */
void TextureCacheImpl::detile_texture_cube(const void* src, void* dst,
                                            u32 face_size, u32 height,
                                            TextureFormat format, bool is_tiled) {
    u32 bytes_per_block = get_bytes_per_block(format);
    u32 block_size = get_block_size(format);
    u32 blocks_x = (face_size + block_size - 1) / block_size;
    u32 blocks_y = (height + block_size - 1) / block_size;
    u64 linear_face_size = static_cast<u64>(blocks_x) * blocks_y * bytes_per_block;

    if (!is_tiled) {
        memcpy(dst, src, linear_face_size * 6);
        return;
    }

    const u8* src_bytes = static_cast<const u8*>(src);
    u8* dst_bytes = static_cast<u8*>(dst);

    // Tiled face size (aligned to macro tile boundaries)
    u32 macro_tiles_x = (blocks_x + tiling::TILE_WIDTH - 1) / tiling::TILE_WIDTH;
    u32 macro_tiles_y = (blocks_y + tiling::TILE_HEIGHT - 1) / tiling::TILE_HEIGHT;
    u64 tiled_face_size = static_cast<u64>(macro_tiles_x) * macro_tiles_y *
                          tiling::TILE_WIDTH * tiling::TILE_HEIGHT * bytes_per_block;

    for (u32 face = 0; face < 6; face++) {
        const u8* face_src = src_bytes + face * tiled_face_size;
        u8* face_dst = dst_bytes + face * linear_face_size;

        for (u32 by = 0; by < blocks_y; by++) {
            for (u32 bx = 0; bx < blocks_x; bx++) {
                u32 src_offset = tiling::get_tiled_offset_2d(bx, by, blocks_x, bytes_per_block);
                u32 dst_offset = (by * blocks_x + bx) * bytes_per_block;
                memcpy(face_dst + dst_offset, face_src + src_offset, bytes_per_block);
            }
        }
    }
}

/**
 * Detile a packed mip tail.
 * Small mip levels (block dimensions < 32x32) are packed together into a single
 * macro tile. Extract each sub-mip and detile it individually.
 */
void TextureCacheImpl::detile_packed_mip_tail(const void* src, void* dst,
                                               u32 base_width, u32 base_height,
                                               u32 start_mip, u32 mip_count,
                                               TextureFormat format) {
    u32 bytes_per_block = get_bytes_per_block(format);
    u32 block_size = get_block_size(format);

    const u8* src_bytes = static_cast<const u8*>(src);
    u8* dst_bytes = static_cast<u8*>(dst);

    // Track offset within the packed tile and output buffer
    u32 packed_offset = 0;
    u32 output_offset = 0;

    u32 mip_w = base_width;
    u32 mip_h = base_height;
    // Skip to start_mip dimensions
    for (u32 i = 0; i < start_mip; i++) {
        mip_w = (mip_w > 1) ? mip_w / 2 : 1;
        mip_h = (mip_h > 1) ? mip_h / 2 : 1;
    }

    for (u32 mip = start_mip; mip < start_mip + mip_count; mip++) {
        u32 bx = (mip_w + block_size - 1) / block_size;
        u32 by = (mip_h + block_size - 1) / block_size;
        u32 mip_data_size = bx * by * bytes_per_block;

        // Packed mip data is already linear within the packed tile (too small to tile)
        memcpy(dst_bytes + output_offset, src_bytes + packed_offset, mip_data_size);

        packed_offset += mip_data_size;
        output_offset += mip_data_size;

        mip_w = (mip_w > 1) ? mip_w / 2 : 1;
        mip_h = (mip_h > 1) ? mip_h / 2 : 1;
    }
}

//=============================================================================
// Sampler Translation (Xbox 360 → Vulkan)
//=============================================================================

VkSamplerAddressMode TextureCacheImpl::translate_address_mode(TextureAddressMode mode) {
    switch (mode) {
        case TextureAddressMode::kWrap:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case TextureAddressMode::kMirror:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case TextureAddressMode::kClampToEdge:
        case TextureAddressMode::kClampToHalf:
        case TextureAddressMode::kMirrorOnceToEdge:
        case TextureAddressMode::kMirrorOnceToHalf:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case TextureAddressMode::kClampToBorder:
        case TextureAddressMode::kMirrorOnceToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFilter TextureCacheImpl::translate_filter(TextureFilter filter) {
    switch (filter) {
        case TextureFilter::kPoint:
            return VK_FILTER_NEAREST;
        case TextureFilter::kLinear:
        case TextureFilter::kBaseMap:
        default:
            return VK_FILTER_LINEAR;
    }
}

VkSamplerConfig TextureCacheImpl::translate_sampler(const FetchConstant& fetch) {
    VkSamplerConfig config{};

    config.min_filter = translate_filter(fetch.min_filter());
    config.mag_filter = translate_filter(fetch.mag_filter());
    config.mipmap_mode = (fetch.mip_filter() == TextureFilter::kPoint)
                         ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                         : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    config.address_u = translate_address_mode(fetch.address_mode_u());
    config.address_v = translate_address_mode(fetch.address_mode_v());
    config.address_w = translate_address_mode(fetch.address_mode_w());
    config.mip_lod_bias = 0.0f;
    config.max_anisotropy = static_cast<f32>(std::min(fetch.max_anisotropy(), 16u));

    // Border color based on type
    switch (fetch.border_color_type()) {
        case 0: config.border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
        case 1: config.border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
        case 2: config.border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
        default: config.border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
    }

    return config;
}

//=============================================================================
// Sampler Cache
//=============================================================================

VkSampler TextureCacheImpl::get_sampler(const VkSamplerConfig& state) {
    u64 hash = state.compute_hash();

    {
        std::lock_guard<std::mutex> lock(sampler_mutex_);
        auto it = sampler_cache_.find(hash);
        if (it != sampler_cache_.end()) {
            return it->second;
        }
    }

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

//=============================================================================
// Invalidation and Lifecycle
//=============================================================================

void TextureCacheImpl::invalidate(GuestAddr base, u64 size) {
    std::lock_guard<std::mutex> lock(texture_mutex_);

    std::vector<u64> to_remove;

    for (auto& [hash, texture] : texture_cache_) {
        GuestAddr tex_addr = texture.address;
        // Use proper data size from format/dimensions/mips
        u64 tex_size = compute_texture_data_size(texture.width, texture.height,
                                                  texture.format, texture.mip_levels);

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
        LOGD("Invalidated %zu textures in range %08x-%08llx",
             to_remove.size(), base, (unsigned long long)(base + size));
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
