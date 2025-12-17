/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * eDRAM (Embedded DRAM) Emulation
 * 
 * The Xbox 360's Xenos GPU has 10MB of embedded DRAM for render targets.
 * This module handles:
 * - Tile-based storage (80x16 pixel tiles)
 * - Morton/Z-order addressing for efficient cache usage
 * - MSAA resolve (2x, 4x)
 * - Format conversion between Xbox and Vulkan formats
 * - Copy/resolve to main memory
 */

#include "edram.h"
#include "memory/memory.h"
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-edram"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[EDRAM] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[EDRAM ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// Morton Code Utilities
//=============================================================================

/**
 * Interleave bits of x and y to create Morton code (Z-order curve)
 * This is the addressing scheme used by Xbox 360 for tiled textures/render targets
 */
u32 TextureUntiler::morton_encode(u32 x, u32 y) {
    // Part1By1 - interleave bits
    auto part1by1 = [](u32 n) -> u32 {
        n = (n ^ (n << 8)) & 0x00FF00FF;
        n = (n ^ (n << 4)) & 0x0F0F0F0F;
        n = (n ^ (n << 2)) & 0x33333333;
        n = (n ^ (n << 1)) & 0x55555555;
        return n;
    };
    
    return part1by1(x) | (part1by1(y) << 1);
}

void TextureUntiler::morton_decode(u32 code, u32& x, u32& y) {
    // Compact1By1 - extract even/odd bits
    auto compact1by1 = [](u32 n) -> u32 {
        n &= 0x55555555;
        n = (n ^ (n >> 1)) & 0x33333333;
        n = (n ^ (n >> 2)) & 0x0F0F0F0F;
        n = (n ^ (n >> 4)) & 0x00FF00FF;
        n = (n ^ (n >> 8)) & 0x0000FFFF;
        return n;
    };
    
    x = compact1by1(code);
    y = compact1by1(code >> 1);
}

u32 TextureUntiler::get_tiled_offset_2d(u32 x, u32 y, u32 width, u32 bpp) {
    // Xbox 360 uses 32x32 pixel tiles with Morton ordering within tiles
    constexpr u32 TILE_SIZE = 32;
    
    u32 tile_x = x / TILE_SIZE;
    u32 tile_y = y / TILE_SIZE;
    u32 tile_width = (width + TILE_SIZE - 1) / TILE_SIZE;
    
    // Offset to start of tile
    u32 tile_offset = (tile_y * tile_width + tile_x) * TILE_SIZE * TILE_SIZE * bpp;
    
    // Morton offset within tile
    u32 local_x = x % TILE_SIZE;
    u32 local_y = y % TILE_SIZE;
    u32 morton = morton_encode(local_x, local_y);
    
    return tile_offset + morton * bpp;
}

u32 TextureUntiler::get_tiled_offset_3d(u32 x, u32 y, u32 z, u32 width, u32 height, u32 bpp) {
    // 3D tiling - slice, then 2D tile within slice
    u32 slice_size = width * height * bpp;
    return z * slice_size + get_tiled_offset_2d(x, y, width, bpp);
}

void TextureUntiler::untile_2d(const u8* src, u8* dst,
                               u32 width, u32 height, u32 bpp,
                               u32 block_width, u32 block_height) {
    // For block-compressed textures, operate on blocks
    u32 blocks_x = (width + block_width - 1) / block_width;
    u32 blocks_y = (height + block_height - 1) / block_height;
    
    for (u32 by = 0; by < blocks_y; by++) {
        for (u32 bx = 0; bx < blocks_x; bx++) {
            u32 tiled_offset = get_tiled_offset_2d(bx, by, blocks_x, bpp);
            u32 linear_offset = (by * blocks_x + bx) * bpp;
            
            memcpy(dst + linear_offset, src + tiled_offset, bpp);
        }
    }
}

void TextureUntiler::untile_3d(const u8* src, u8* dst,
                               u32 width, u32 height, u32 depth, u32 bpp) {
    for (u32 z = 0; z < depth; z++) {
        for (u32 y = 0; y < height; y++) {
            for (u32 x = 0; x < width; x++) {
                u32 tiled_offset = get_tiled_offset_3d(x, y, z, width, height, bpp);
                u32 linear_offset = ((z * height + y) * width + x) * bpp;
                
                memcpy(dst + linear_offset, src + tiled_offset, bpp);
            }
        }
    }
}

//=============================================================================
// EdramManager Implementation
//=============================================================================

EdramManager::EdramManager() {
    // Initialize render targets as disabled
    for (auto& rt : render_targets_) {
        rt.enabled = false;
    }
    depth_stencil_.enabled = false;
}

EdramManager::~EdramManager() {
    shutdown();
}

Status EdramManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Allocate eDRAM memory
    data_.resize(EDRAM_SIZE, 0);
    
    reset();
    
    LOGI("eDRAM initialized: %u bytes", EDRAM_SIZE);
    return Status::Ok;
}

void EdramManager::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
    data_.shrink_to_fit();
}

void EdramManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Clear all eDRAM
    std::fill(data_.begin(), data_.end(), 0);
    
    // Reset render target configs
    for (auto& rt : render_targets_) {
        rt = {};
        rt.enabled = false;
    }
    
    depth_stencil_ = {};
    depth_stencil_.enabled = false;
}

void EdramManager::set_render_target(u32 index, const RenderTargetConfig& config) {
    if (index >= render_targets_.size()) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    render_targets_[index] = config;
    
    LOGD("RT%u: base=%u, pitch=%u, format=%d, msaa=%d", 
         index, config.edram_base, config.edram_pitch, 
         static_cast<int>(config.format), static_cast<int>(config.msaa));
}

void EdramManager::set_depth_stencil(const DepthStencilConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    depth_stencil_ = config;
}

const RenderTargetConfig& EdramManager::get_render_target(u32 index) const {
    static RenderTargetConfig empty = {};
    if (index >= render_targets_.size()) return empty;
    return render_targets_[index];
}

u32 EdramManager::get_bytes_per_pixel(EdramSurfaceFormat format) {
    switch (format) {
        case EdramSurfaceFormat::k_8:
            return 1;
            
        case EdramSurfaceFormat::k_1_5_5_5:
        case EdramSurfaceFormat::k_5_6_5:
        case EdramSurfaceFormat::k_6_5_5:
        case EdramSurfaceFormat::k_8_8:
        case EdramSurfaceFormat::k_4_4_4_4:
        case EdramSurfaceFormat::k_16:
            return 2;
            
        case EdramSurfaceFormat::k_8_8_8_8:
        case EdramSurfaceFormat::k_8_8_8_8_A:
        case EdramSurfaceFormat::k_2_10_10_10:
        case EdramSurfaceFormat::k_10_11_11:
        case EdramSurfaceFormat::k_11_11_10:
        case EdramSurfaceFormat::k_16_16:
        case EdramSurfaceFormat::k_32_FLOAT:
            return 4;
            
        case EdramSurfaceFormat::k_16_16_16_16:
        case EdramSurfaceFormat::k_16_16_FLOAT:
        case EdramSurfaceFormat::k_32_32_FLOAT:
            return 8;
            
        case EdramSurfaceFormat::k_16_16_16_16_FLOAT:
        case EdramSurfaceFormat::k_32_32_32_32_FLOAT:
            return 16;
            
        default:
            return 4;  // Default to 32-bit
    }
}

u32 EdramManager::calculate_tile_offset(u32 x, u32 y, u32 pitch, EdramMsaaMode msaa) {
    // eDRAM tiles are 80x16 pixels
    u32 tile_x = x / EDRAM_TILE_WIDTH;
    u32 tile_y = y / EDRAM_TILE_HEIGHT;
    
    // MSAA multiplier
    u32 msaa_mult = 1;
    switch (msaa) {
        case EdramMsaaMode::k2X: msaa_mult = 2; break;
        case EdramMsaaMode::k4X: msaa_mult = 4; break;
        default: break;
    }
    
    u32 tiles_per_row = pitch / EDRAM_TILE_WIDTH;
    u32 tile_offset = (tile_y * tiles_per_row + tile_x) * EDRAM_TILE_SIZE * msaa_mult;
    
    // Morton offset within tile
    u32 local_x = x % EDRAM_TILE_WIDTH;
    u32 local_y = y % EDRAM_TILE_HEIGHT;
    u32 local_offset = TextureUntiler::morton_encode(local_x, local_y);
    
    return (tile_offset + local_offset) * msaa_mult;
}

void EdramManager::clear_render_target(u32 index, f32 r, f32 g, f32 b, f32 a) {
    if (index >= render_targets_.size() || !render_targets_[index].enabled) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    const auto& rt = render_targets_[index];
    u32 bpp = get_bytes_per_pixel(rt.format);
    
    // Convert float RGBA to format-specific value
    u32 clear_value = 0;
    switch (rt.format) {
        case EdramSurfaceFormat::k_8_8_8_8:
        case EdramSurfaceFormat::k_8_8_8_8_A:
            clear_value = (static_cast<u32>(a * 255) << 24) |
                          (static_cast<u32>(b * 255) << 16) |
                          (static_cast<u32>(g * 255) << 8) |
                          (static_cast<u32>(r * 255));
            break;
            
        case EdramSurfaceFormat::k_5_6_5:
            clear_value = (static_cast<u32>(b * 31) << 11) |
                          (static_cast<u32>(g * 63) << 5) |
                          (static_cast<u32>(r * 31));
            break;
            
        default:
            // Generic clear
            clear_value = 0xFFFFFFFF;
            break;
    }
    
    // Clear the eDRAM region
    u32 start = rt.edram_base * 4;  // eDRAM base is in tiles
    u32 size = rt.edram_pitch * EDRAM_TILE_HEIGHT * bpp;
    
    if (start + size <= data_.size()) {
        if (bpp == 4) {
            u32* data32 = reinterpret_cast<u32*>(data_.data() + start);
            std::fill_n(data32, size / 4, clear_value);
        } else {
            std::fill_n(data_.data() + start, size, static_cast<u8>(clear_value));
        }
    }
}

void EdramManager::clear_depth_stencil(f32 depth, u8 stencil) {
    if (!depth_stencil_.enabled) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // D24S8 format: 24-bit depth, 8-bit stencil
    u32 depth_bits = static_cast<u32>(depth * 0xFFFFFF) & 0xFFFFFF;
    u32 clear_value = (depth_bits << 8) | stencil;
    
    u32 start = depth_stencil_.edram_base * 4;
    u32 size = depth_stencil_.edram_pitch * EDRAM_TILE_HEIGHT * 4;
    
    if (start + size <= data_.size()) {
        u32* data32 = reinterpret_cast<u32*>(data_.data() + start);
        std::fill_n(data32, size / 4, clear_value);
    }
}

void EdramManager::resolve_render_target(u32 index, Memory* memory) {
    if (index >= render_targets_.size() || !render_targets_[index].enabled || !memory) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    const auto& rt = render_targets_[index];
    if (rt.resolve_address == 0) return;
    
    u32 bpp = get_bytes_per_pixel(rt.format);
    u32 width = rt.resolve_width;
    u32 height = rt.resolve_height;
    
    // Temporary buffer for untiled data
    std::vector<u8> temp(width * height * bpp);
    
    // Untile and resolve MSAA
    u32 edram_start = rt.edram_base * 4;
    
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 tile_offset = calculate_tile_offset(x, y, rt.edram_pitch, rt.msaa);
            u32 src_offset = edram_start + tile_offset * bpp;
            u32 dst_offset = (y * width + x) * bpp;
            
            if (src_offset + bpp <= data_.size()) {
                if (rt.msaa != EdramMsaaMode::k1X) {
                    // MSAA resolve - average samples
                    u32 sample_count = (rt.msaa == EdramMsaaMode::k2X) ? 2 : 4;
                    resolve_pixel(data_.data() + src_offset, temp.data() + dst_offset, 
                                 bpp, sample_count);
                } else {
                    memcpy(temp.data() + dst_offset, data_.data() + src_offset, bpp);
                }
            }
        }
    }
    
    // Write to main memory
    void* dst = memory->get_host_ptr(rt.resolve_address);
    if (dst) {
        // Convert from tiled to linear
        if (rt.resolve_pitch == width * bpp) {
            // Simple copy if pitches match
            memcpy(dst, temp.data(), temp.size());
        } else {
            // Row by row copy for different pitches
            for (u32 y = 0; y < height; y++) {
                memcpy(static_cast<u8*>(dst) + y * rt.resolve_pitch,
                       temp.data() + y * width * bpp,
                       width * bpp);
            }
        }
    }
    
    LOGD("Resolved RT%u: %ux%u to %08X", index, width, height, rt.resolve_address);
}

void EdramManager::resolve_depth_stencil(Memory* memory) {
    // Similar to render target resolve but for depth buffer
    // TODO: Implement depth resolve if needed
    (void)memory;
}

void EdramManager::copy_to_edram(u32 edram_offset, GuestAddr src_address,
                                  u32 width, u32 height, EdramSurfaceFormat format,
                                  Memory* memory) {
    if (!memory) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    u32 bpp = get_bytes_per_pixel(format);
    const void* src = memory->get_host_ptr(src_address);
    if (!src) return;
    
    // Tile the data as we copy
    u32 dst_start = edram_offset * 4;
    
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 src_offset = (y * width + x) * bpp;
            u32 tile_offset = calculate_tile_offset(x, y, width, EdramMsaaMode::k1X);
            u32 dst_offset = dst_start + tile_offset * bpp;
            
            if (dst_offset + bpp <= data_.size()) {
                memcpy(data_.data() + dst_offset,
                       static_cast<const u8*>(src) + src_offset, bpp);
            }
        }
    }
}

void EdramManager::untile_surface(const u8* src, u8* dst,
                                   u32 width, u32 height, u32 bpp,
                                   u32 src_pitch, u32 dst_pitch) {
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 tile_offset = calculate_tile_offset(x, y, src_pitch / bpp, EdramMsaaMode::k1X);
            memcpy(dst + y * dst_pitch + x * bpp,
                   src + tile_offset * bpp, bpp);
        }
    }
}

void EdramManager::tile_surface(const u8* src, u8* dst,
                                 u32 width, u32 height, u32 bpp,
                                 u32 src_pitch, u32 dst_pitch) {
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 tile_offset = calculate_tile_offset(x, y, dst_pitch / bpp, EdramMsaaMode::k1X);
            memcpy(dst + tile_offset * bpp,
                   src + y * src_pitch + x * bpp, bpp);
        }
    }
}

void EdramManager::resolve_msaa_2x(const u8* src, u8* dst,
                                    u32 width, u32 height, u32 bpp) {
    // 2x MSAA: average 2 samples
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 dst_offset = (y * width + x) * bpp;
            u32 src_offset = dst_offset * 2;  // 2 samples per pixel
            
            resolve_pixel(src + src_offset, dst + dst_offset, bpp, 2);
        }
    }
}

void EdramManager::resolve_msaa_4x(const u8* src, u8* dst,
                                    u32 width, u32 height, u32 bpp) {
    // 4x MSAA: average 4 samples
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 dst_offset = (y * width + x) * bpp;
            u32 src_offset = dst_offset * 4;  // 4 samples per pixel
            
            resolve_pixel(src + src_offset, dst + dst_offset, bpp, 4);
        }
    }
}

void EdramManager::resolve_pixel(const u8* src, u8* dst, u32 bpp, u32 sample_count) {
    // Average samples based on format
    if (bpp == 4) {
        // RGBA8 or similar
        u32 r = 0, g = 0, b = 0, a = 0;
        for (u32 i = 0; i < sample_count; i++) {
            const u8* sample = src + i * bpp;
            r += sample[0];
            g += sample[1];
            b += sample[2];
            a += sample[3];
        }
        dst[0] = static_cast<u8>(r / sample_count);
        dst[1] = static_cast<u8>(g / sample_count);
        dst[2] = static_cast<u8>(b / sample_count);
        dst[3] = static_cast<u8>(a / sample_count);
    } else if (bpp == 2) {
        // 16-bit format - simple average
        u32 sum = 0;
        for (u32 i = 0; i < sample_count; i++) {
            sum += *reinterpret_cast<const u16*>(src + i * bpp);
        }
        *reinterpret_cast<u16*>(dst) = static_cast<u16>(sum / sample_count);
    } else if (bpp == 8 || bpp == 16) {
        // Float formats - component-wise average
        for (u32 c = 0; c < bpp / 4; c++) {
            f32 sum = 0.0f;
            for (u32 i = 0; i < sample_count; i++) {
                sum += reinterpret_cast<const f32*>(src + i * bpp)[c];
            }
            reinterpret_cast<f32*>(dst)[c] = sum / sample_count;
        }
    } else {
        // Fallback: just take first sample
        memcpy(dst, src, bpp);
    }
}

void EdramManager::convert_format(const u8* src, u8* dst,
                                   u32 pixel_count,
                                   EdramSurfaceFormat src_format,
                                   EdramSurfaceFormat dst_format) {
    // Convert between formats
    // For now, handle common cases
    
    if (src_format == dst_format) {
        // No conversion needed
        u32 bpp = get_bytes_per_pixel(src_format);
        memcpy(dst, src, pixel_count * bpp);
        return;
    }
    
    // Convert to RGBA8 as intermediate
    for (u32 i = 0; i < pixel_count; i++) {
        u8 r, g, b, a;
        
        // Read source
        switch (src_format) {
            case EdramSurfaceFormat::k_8_8_8_8:
                r = src[i * 4 + 0];
                g = src[i * 4 + 1];
                b = src[i * 4 + 2];
                a = src[i * 4 + 3];
                break;
                
            case EdramSurfaceFormat::k_5_6_5: {
                u16 pixel = *reinterpret_cast<const u16*>(src + i * 2);
                r = static_cast<u8>(((pixel >> 11) & 0x1F) * 255 / 31);
                g = static_cast<u8>(((pixel >> 5) & 0x3F) * 255 / 63);
                b = static_cast<u8>((pixel & 0x1F) * 255 / 31);
                a = 255;
                break;
            }
                
            default:
                r = g = b = 128;
                a = 255;
                break;
        }
        
        // Write destination
        switch (dst_format) {
            case EdramSurfaceFormat::k_8_8_8_8:
                dst[i * 4 + 0] = r;
                dst[i * 4 + 1] = g;
                dst[i * 4 + 2] = b;
                dst[i * 4 + 3] = a;
                break;
                
            case EdramSurfaceFormat::k_5_6_5: {
                u16 pixel = (static_cast<u16>(b * 31 / 255) << 11) |
                            (static_cast<u16>(g * 63 / 255) << 5) |
                            (static_cast<u16>(r * 31 / 255));
                *reinterpret_cast<u16*>(dst + i * 2) = pixel;
                break;
            }
                
            default:
                break;
        }
    }
}

} // namespace x360mu
