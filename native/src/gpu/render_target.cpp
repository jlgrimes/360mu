/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Render Target Manager Implementation
 */

#include "render_target.h"
#include "vulkan/vulkan_backend.h"
#include "memory/memory.h"
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-rt"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[RT] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[RT ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// FramebufferKey Implementation
//=============================================================================

bool FramebufferKey::operator==(const FramebufferKey& other) const {
    return color_rt_hashes == other.color_rt_hashes &&
           depth_rt_hash == other.depth_rt_hash &&
           width == other.width &&
           height == other.height;
}

u64 FramebufferKey::compute_hash() const {
    // FNV-1a hash
    u64 hash = 0xcbf29ce484222325ULL;
    auto mix = [&hash](u64 value) {
        hash ^= value;
        hash *= 0x100000001b3ULL;
    };
    
    for (const auto& h : color_rt_hashes) {
        mix(h);
    }
    mix(depth_rt_hash);
    mix(width);
    mix(height);
    
    return hash;
}

//=============================================================================
// RenderTargetManager Implementation
//=============================================================================

RenderTargetManager::RenderTargetManager() {
    // Initialize configuration to disabled
    for (u32 i = 0; i < MAX_COLOR_TARGETS; i++) {
        config_.color_enabled[i] = false;
        config_.color_edram_base[i] = 0;
        config_.color_pitch[i] = 0;
        config_.color_format[i] = SurfaceFormat::k_8_8_8_8;
        config_.color_width[i] = 0;
        config_.color_height[i] = 0;
    }
    config_.depth_enabled = false;
    config_.depth_edram_base = 0;
    config_.depth_pitch = 0;
    config_.depth_format = SurfaceFormat::k_8_8_8_8;  // Will be overridden
    config_.depth_width = 0;
    config_.depth_height = 0;
}

RenderTargetManager::~RenderTargetManager() {
    shutdown();
}

Status RenderTargetManager::initialize(VulkanBackend* vulkan, Memory* memory, 
                                        EdramManager* edram) {
    vulkan_ = vulkan;
    memory_ = memory;
    edram_ = edram;
    
    if (create_staging_buffer() != Status::Ok) {
        LOGE("Failed to create staging buffer");
        return Status::ErrorInit;
    }
    
    LOGI("Render target manager initialized");
    return Status::Ok;
}

void RenderTargetManager::shutdown() {
    if (!vulkan_) return;
    
    VkDevice device = vulkan_->device();
    vkDeviceWaitIdle(device);
    
    // Destroy staging buffer
    destroy_staging_buffer();
    
    // Destroy render targets
    for (auto& rt : color_targets_) {
        destroy_render_target(rt);
    }
    destroy_render_target(depth_target_);
    
    // Destroy framebuffers
    for (auto& [key, fb] : framebuffer_cache_) {
        if (fb.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, fb.framebuffer, nullptr);
        }
    }
    framebuffer_cache_.clear();
    
    // Destroy render passes
    for (auto& [key, pass] : render_pass_cache_) {
        if (pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, pass, nullptr);
        }
    }
    render_pass_cache_.clear();
    
    vulkan_ = nullptr;
    memory_ = nullptr;
    edram_ = nullptr;
    
    LOGI("Render target manager shutdown");
}

Status RenderTargetManager::create_staging_buffer() {
    VkDevice device = vulkan_->device();
    
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = STAGING_SIZE;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(device, &buffer_info, nullptr, &staging_buffer_) != VK_SUCCESS) {
        return Status::ErrorInit;
    }
    
    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device, staging_buffer_, &mem_reqs);
    
    u32 mem_type = vulkan_->find_memory_type(
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = mem_type;
    
    if (vkAllocateMemory(device, &alloc_info, nullptr, &staging_memory_) != VK_SUCCESS) {
        vkDestroyBuffer(device, staging_buffer_, nullptr);
        staging_buffer_ = VK_NULL_HANDLE;
        return Status::ErrorInit;
    }
    
    vkBindBufferMemory(device, staging_buffer_, staging_memory_, 0);
    vkMapMemory(device, staging_memory_, 0, STAGING_SIZE, 0, &staging_mapped_);
    
    return Status::Ok;
}

void RenderTargetManager::destroy_staging_buffer() {
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

void RenderTargetManager::begin_frame() {
    current_frame_++;
    current_framebuffer_ = nullptr;
}

void RenderTargetManager::end_frame() {
    // Any pending work at frame end
}

void RenderTargetManager::set_color_target(u32 index, u32 edram_base, u32 pitch,
                                            SurfaceFormat format, u32 width, u32 height) {
    if (index >= MAX_COLOR_TARGETS) return;
    
    config_.color_edram_base[index] = edram_base;
    config_.color_pitch[index] = pitch;
    config_.color_format[index] = format;
    config_.color_width[index] = width;
    config_.color_height[index] = height;
    config_.color_enabled[index] = (width > 0 && height > 0);
    
    // Check if we need to recreate the render target
    auto& rt = color_targets_[index];
    VkFormat vk_format = translate_surface_format(format, false);
    
    if (!rt.is_valid() || rt.width != width || rt.height != height || 
        rt.format != vk_format || rt.edram_base != edram_base) {
        
        destroy_render_target(rt);
        if (config_.color_enabled[index]) {
            rt = create_render_target(width, height, vk_format, false);
            rt.edram_base = edram_base;
        }
    }
    
    // Invalidate current framebuffer
    current_framebuffer_ = nullptr;
}

void RenderTargetManager::set_depth_target(u32 edram_base, u32 pitch,
                                            SurfaceFormat format, u32 width, u32 height) {
    config_.depth_edram_base = edram_base;
    config_.depth_pitch = pitch;
    config_.depth_format = format;
    config_.depth_width = width;
    config_.depth_height = height;
    config_.depth_enabled = (width > 0 && height > 0);
    
    // Check if we need to recreate
    VkFormat vk_format = translate_surface_format(format, true);
    
    if (!depth_target_.is_valid() || depth_target_.width != width || 
        depth_target_.height != height || depth_target_.format != vk_format ||
        depth_target_.edram_base != edram_base) {
        
        destroy_render_target(depth_target_);
        if (config_.depth_enabled) {
            depth_target_ = create_render_target(width, height, vk_format, true);
            depth_target_.edram_base = edram_base;
        }
    }
    
    current_framebuffer_ = nullptr;
}

VulkanRenderTarget RenderTargetManager::create_render_target(u32 width, u32 height,
                                                              VkFormat format, bool is_depth) {
    VulkanRenderTarget rt{};
    rt.width = width;
    rt.height = height;
    rt.format = format;
    rt.is_depth = is_depth;
    
    VkDevice device = vulkan_->device();
    
    // Create image
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = is_depth ? 
        (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) :
        (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 
         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    if (vkCreateImage(device, &image_info, nullptr, &rt.image) != VK_SUCCESS) {
        LOGE("Failed to create render target image");
        return rt;
    }
    
    // Allocate memory
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, rt.image, &mem_reqs);
    
    u32 mem_type = vulkan_->find_memory_type(mem_reqs.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = mem_type;
    
    if (vkAllocateMemory(device, &alloc_info, nullptr, &rt.memory) != VK_SUCCESS) {
        vkDestroyImage(device, rt.image, nullptr);
        rt.image = VK_NULL_HANDLE;
        return rt;
    }
    
    vkBindImageMemory(device, rt.image, rt.memory, 0);
    
    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = rt.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = is_depth ? 
        VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    
    if (vkCreateImageView(device, &view_info, nullptr, &rt.view) != VK_SUCCESS) {
        vkDestroyImage(device, rt.image, nullptr);
        vkFreeMemory(device, rt.memory, nullptr);
        rt.image = VK_NULL_HANDLE;
        rt.memory = VK_NULL_HANDLE;
        return rt;
    }
    
    LOGD("Created %s target: %ux%u, format=%d", 
         is_depth ? "depth" : "color", width, height, format);
    
    return rt;
}

void RenderTargetManager::destroy_render_target(VulkanRenderTarget& rt) {
    if (!rt.is_valid()) return;
    
    VkDevice device = vulkan_->device();
    
    if (rt.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, rt.view, nullptr);
        rt.view = VK_NULL_HANDLE;
    }
    if (rt.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, rt.image, nullptr);
        rt.image = VK_NULL_HANDLE;
    }
    if (rt.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, rt.memory, nullptr);
        rt.memory = VK_NULL_HANDLE;
    }
}

VkFormat RenderTargetManager::translate_surface_format(SurfaceFormat format, bool is_depth) {
    if (is_depth) {
        // Depth formats
        switch (format) {
            case SurfaceFormat::k_8_8_8_8:  // D24S8 is common
            default:
                return VK_FORMAT_D24_UNORM_S8_UINT;
        }
    }
    
    // Color formats
    switch (format) {
        case SurfaceFormat::k_8_8_8_8:
        case SurfaceFormat::k_8_8_8_8_GAMMA:
            return VK_FORMAT_R8G8B8A8_UNORM;
            
        case SurfaceFormat::k_2_10_10_10:
        case SurfaceFormat::k_2_10_10_10_FLOAT:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            
        case SurfaceFormat::k_16_16:
            return VK_FORMAT_R16G16_UNORM;
            
        case SurfaceFormat::k_16_16_16_16:
            return VK_FORMAT_R16G16B16A16_UNORM;
            
        case SurfaceFormat::k_16_16_FLOAT:
            return VK_FORMAT_R16G16_SFLOAT;
            
        case SurfaceFormat::k_16_16_16_16_FLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
            
        case SurfaceFormat::k_32_FLOAT:
            return VK_FORMAT_R32_SFLOAT;
            
        case SurfaceFormat::k_32_32_FLOAT:
            return VK_FORMAT_R32G32_SFLOAT;
            
        case SurfaceFormat::k_32_32_32_32_FLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
            
        case SurfaceFormat::k_5_6_5:
            return VK_FORMAT_R5G6B5_UNORM_PACK16;
            
        case SurfaceFormat::k_6_5_5:
            return VK_FORMAT_R5G6B5_UNORM_PACK16;  // Closest
            
        case SurfaceFormat::k_1_5_5_5:
            return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
            
        case SurfaceFormat::k_4_4_4_4:
            return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
            
        default:
            return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

u64 RenderTargetManager::compute_render_target_hash(u32 edram_base, u32 pitch,
                                                     SurfaceFormat format, 
                                                     u32 width, u32 height) {
    u64 hash = 0xcbf29ce484222325ULL;
    auto mix = [&hash](u64 value) {
        hash ^= value;
        hash *= 0x100000001b3ULL;
    };
    
    mix(edram_base);
    mix(pitch);
    mix(static_cast<u64>(format));
    mix(width);
    mix(height);
    
    return hash;
}

CachedFramebuffer* RenderTargetManager::get_current_framebuffer() {
    // Build framebuffer key
    FramebufferKey key{};
    u32 width = 0, height = 0;
    
    for (u32 i = 0; i < MAX_COLOR_TARGETS; i++) {
        if (config_.color_enabled[i]) {
            key.color_rt_hashes[i] = compute_render_target_hash(
                config_.color_edram_base[i], config_.color_pitch[i],
                config_.color_format[i], config_.color_width[i], config_.color_height[i]
            );
            if (width == 0) {
                width = config_.color_width[i];
                height = config_.color_height[i];
            }
        }
    }
    
    if (config_.depth_enabled) {
        key.depth_rt_hash = compute_render_target_hash(
            config_.depth_edram_base, config_.depth_pitch,
            config_.depth_format, config_.depth_width, config_.depth_height
        );
        if (width == 0) {
            width = config_.depth_width;
            height = config_.depth_height;
        }
    }
    
    key.width = width;
    key.height = height;
    
    // Check cache
    auto it = framebuffer_cache_.find(key);
    if (it != framebuffer_cache_.end()) {
        current_framebuffer_ = &it->second;
        return current_framebuffer_;
    }
    
    // Create new framebuffer
    // First, get or create render pass
    VkRenderPass render_pass = get_current_render_pass();
    if (render_pass == VK_NULL_HANDLE) {
        return nullptr;
    }
    
    // Collect attachments
    std::vector<VkImageView> attachments;
    for (u32 i = 0; i < MAX_COLOR_TARGETS; i++) {
        if (config_.color_enabled[i] && color_targets_[i].is_valid()) {
            attachments.push_back(color_targets_[i].view);
        }
    }
    if (config_.depth_enabled && depth_target_.is_valid()) {
        attachments.push_back(depth_target_.view);
    }
    
    if (attachments.empty()) {
        return nullptr;
    }
    
    CachedFramebuffer fb = create_framebuffer(render_pass, attachments, width, height);
    if (fb.framebuffer == VK_NULL_HANDLE) {
        return nullptr;
    }
    
    auto [inserted, success] = framebuffer_cache_.emplace(key, fb);
    current_framebuffer_ = &inserted->second;
    return current_framebuffer_;
}

VkRenderPass RenderTargetManager::get_current_render_pass() {
    // Build render pass key from current state
    u64 rp_key = 0;
    std::array<VkFormat, MAX_COLOR_TARGETS> color_formats{};
    u32 color_count = 0;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    bool has_depth = false;
    
    for (u32 i = 0; i < MAX_COLOR_TARGETS; i++) {
        if (config_.color_enabled[i]) {
            color_formats[color_count++] = translate_surface_format(config_.color_format[i], false);
        }
    }
    
    if (config_.depth_enabled) {
        depth_format = translate_surface_format(config_.depth_format, true);
        has_depth = true;
    }
    
    // Simple hash for render pass key
    rp_key = color_count;
    for (u32 i = 0; i < color_count; i++) {
        rp_key = rp_key * 31 + color_formats[i];
    }
    rp_key = rp_key * 31 + depth_format;
    rp_key = rp_key * 31 + (has_depth ? 1 : 0);
    
    // Check cache
    auto it = render_pass_cache_.find(rp_key);
    if (it != render_pass_cache_.end()) {
        return it->second;
    }
    
    // Create new render pass
    VkRenderPass pass = create_render_pass(color_formats, color_count, depth_format, has_depth);
    if (pass != VK_NULL_HANDLE) {
        render_pass_cache_[rp_key] = pass;
    }
    
    return pass;
}

VkRenderPass RenderTargetManager::create_render_pass(
    const std::array<VkFormat, MAX_COLOR_TARGETS>& color_formats,
    u32 color_count, VkFormat depth_format, bool has_depth) {
    
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> color_refs;
    VkAttachmentReference depth_ref{};
    
    // Color attachments
    for (u32 i = 0; i < color_count; i++) {
        VkAttachmentDescription color_attachment{};
        color_attachment.format = color_formats[i];
        color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkAttachmentReference ref{};
        ref.attachment = static_cast<u32>(attachments.size());
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        attachments.push_back(color_attachment);
        color_refs.push_back(ref);
    }
    
    // Depth attachment
    if (has_depth) {
        VkAttachmentDescription depth_attachment{};
        depth_attachment.format = depth_format;
        depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        
        depth_ref.attachment = static_cast<u32>(attachments.size());
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        
        attachments.push_back(depth_attachment);
    }
    
    // Subpass
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<u32>(color_refs.size());
    subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;
    
    // Create render pass
    VkRenderPassCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = static_cast<u32>(attachments.size());
    create_info.pAttachments = attachments.empty() ? nullptr : attachments.data();
    create_info.subpassCount = 1;
    create_info.pSubpasses = &subpass;
    
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(vulkan_->device(), &create_info, nullptr, &pass) != VK_SUCCESS) {
        LOGE("Failed to create render pass");
        return VK_NULL_HANDLE;
    }
    
    return pass;
}

CachedFramebuffer RenderTargetManager::create_framebuffer(VkRenderPass render_pass,
                                                           const std::vector<VkImageView>& attachments,
                                                           u32 width, u32 height) {
    CachedFramebuffer fb{};
    fb.render_pass = render_pass;
    fb.width = width;
    fb.height = height;
    fb.attachments = attachments;
    
    VkFramebufferCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    create_info.renderPass = render_pass;
    create_info.attachmentCount = static_cast<u32>(attachments.size());
    create_info.pAttachments = attachments.data();
    create_info.width = width;
    create_info.height = height;
    create_info.layers = 1;
    
    if (vkCreateFramebuffer(vulkan_->device(), &create_info, nullptr, &fb.framebuffer) != VK_SUCCESS) {
        LOGE("Failed to create framebuffer");
        return fb;
    }
    
    LOGD("Created framebuffer: %ux%u, %zu attachments", width, height, attachments.size());
    return fb;
}

void RenderTargetManager::clear_color_target(u32 index, f32 r, f32 g, f32 b, f32 a) {
    if (index >= MAX_COLOR_TARGETS || !color_targets_[index].is_valid()) return;
    
    // Mark for clear on next render pass begin
    color_targets_[index].needs_clear = true;
    
    // Also update eDRAM if available
    if (edram_) {
        edram_->clear_render_target(index, r, g, b, a);
    }
}

void RenderTargetManager::clear_depth_stencil(f32 depth, u8 stencil) {
    if (!depth_target_.is_valid()) return;
    
    depth_target_.needs_clear = true;
    
    if (edram_) {
        edram_->clear_depth_stencil(depth, stencil);
    }
}

void RenderTargetManager::resolve_to_memory(u32 rt_index, GuestAddr dest_address,
                                             u32 dest_pitch, u32 width, u32 height) {
    if (rt_index >= MAX_COLOR_TARGETS || !color_targets_[rt_index].is_valid()) return;
    if (!memory_ || dest_address == 0) return;
    
    const auto& rt = color_targets_[rt_index];
    
    // Resolve Vulkan image to staging buffer
    resolve_render_target_to_buffer(rt, staging_buffer_, 0);
    
    // Copy from staging to guest memory
    void* dest = memory_->get_host_ptr(dest_address);
    if (dest && staging_mapped_) {
        // Handle pitch difference
        u32 bytes_per_pixel = 4;  // Assume RGBA8 for now
        u32 row_size = width * bytes_per_pixel;
        
        if (dest_pitch == row_size) {
            memcpy(dest, staging_mapped_, row_size * height);
        } else {
            for (u32 y = 0; y < height; y++) {
                memcpy(static_cast<u8*>(dest) + y * dest_pitch,
                       static_cast<u8*>(staging_mapped_) + y * row_size,
                       row_size);
            }
        }
    }
    
    LOGD("Resolved RT%u to %08X (%ux%u)", rt_index, dest_address, width, height);
}

void RenderTargetManager::copy_from_memory(u32 rt_index, GuestAddr src_address,
                                            u32 src_pitch, u32 width, u32 height) {
    if (rt_index >= MAX_COLOR_TARGETS || !color_targets_[rt_index].is_valid()) return;
    if (!memory_ || src_address == 0) return;
    
    // Read from guest memory to staging buffer
    const void* src = memory_->get_host_ptr(src_address);
    if (!src || !staging_mapped_) return;
    
    u32 bytes_per_pixel = 4;
    u32 row_size = width * bytes_per_pixel;
    
    if (src_pitch == row_size) {
        memcpy(staging_mapped_, src, row_size * height);
    } else {
        for (u32 y = 0; y < height; y++) {
            memcpy(static_cast<u8*>(staging_mapped_) + y * row_size,
                   static_cast<const u8*>(src) + y * src_pitch,
                   row_size);
        }
    }
    
    // Upload to Vulkan image
    // TODO: Implement buffer to image copy
    
    LOGD("Copied memory %08X to RT%u (%ux%u)", src_address, rt_index, width, height);
}

void RenderTargetManager::resolve_render_target_to_buffer(const VulkanRenderTarget& rt,
                                                           VkBuffer dest, u64 offset) {
    VkDevice device = vulkan_->device();
    VkCommandBuffer cmd = vulkan_->current_command_buffer();
    
    // Transition image to transfer source
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = rt.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Copy image to buffer
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.bufferRowLength = 0;  // Tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {rt.width, rt.height, 1};
    
    vkCmdCopyImageToBuffer(cmd, rt.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dest, 1, &region);
    
    // Transition back
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkImageView RenderTargetManager::get_color_view(u32 index) {
    if (index >= MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    return color_targets_[index].view;
}

VkImageView RenderTargetManager::get_depth_view() {
    return depth_target_.view;
}

} // namespace x360mu
