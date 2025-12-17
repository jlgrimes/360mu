/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * eDRAM Emulation Implementation
 * 
 * Handles the Xbox 360's 10MB embedded DRAM used for render targets.
 * Implements tiling/untiling, MSAA resolve, and format conversion.
 */

#include "edram.h"
#include "../../memory/memory.h"
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-edram"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[EDRAM] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#endif

namespace x360mu {

//=============================================================================
// EdramManager Implementation
//=============================================================================

EdramManager::EdramManager() {
    data_.resize(EDRAM_SIZE, 0);
}

EdramManager::~EdramManager() = default;

Status EdramManager::initialize() {
    reset();
    LOGI("eDRAM initialized (10MB)");
    return Status::Ok;
}

void EdramManager::shutdown() {
    data_.clear();
}

void EdramManager::reset() {
    std::fill(data_.begin(), data_.end(), 0);
    
    for (auto& rt : render_targets_) {
        rt = {};
    }
    depth_stencil_ = {};
}

void EdramManager::set_render_target(u32 index, const RenderTargetConfig& config) {
    if (index >= 4) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    render_targets_[index] = config;
    
    LOGD("RT%u: base=0x%X, pitch=%u, format=%u, msaa=%u",
         index, config.edram_base, config.edram_pitch,
         static_cast<u32>(config.format), static_cast<u32>(config.msaa));
}

void EdramManager::set_depth_stencil(const DepthStencilConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    depth_stencil_ = config;
}

const RenderTargetConfig& EdramManager::get_render_target(u32 index) const {
    static RenderTargetConfig empty = {};
    if (index >= 4) return empty;
    return render_targets_[index];
}

u32 EdramManager::get_bytes_per_pixel(EdramSurfaceFormat format) {
    switch (format) {
        case EdramSurfaceFormat::k_8:
        case EdramSurfaceFormat::k_8_A:
        case EdramSurfaceFormat::k_8_B:
            return 1;
            
        case EdramSurfaceFormat::k_1_5_5_5:
        case EdramSurfaceFormat::k_5_6_5:
        case EdramSurfaceFormat::k_6_5_5:
        case EdramSurfaceFormat::k_4_4_4_4:
        case EdramSurfaceFormat::k_8_8:
        case EdramSurfaceFormat::k_16:
        case EdramSurfaceFormat::k_16_EXPAND:
        case EdramSurfaceFormat::k_16_FLOAT:
            return 2;
            
        case EdramSurfaceFormat::k_8_8_8_8:
        case EdramSurfaceFormat::k_8_8_8_8_A:
        case EdramSurfaceFormat::k_2_10_10_10:
        case EdramSurfaceFormat::k_10_11_11:
        case EdramSurfaceFormat::k_11_11_10:
        case EdramSurfaceFormat::k_16_16:
        case EdramSurfaceFormat::k_16_16_EXPAND:
        case EdramSurfaceFormat::k_16_16_FLOAT:
        case EdramSurfaceFormat::k_32_FLOAT:
        case EdramSurfaceFormat::k_8_8_8_8_AS_16_16_16_16:
        case EdramSurfaceFormat::k_2_10_10_10_AS_16_16_16_16:
        case EdramSurfaceFormat::k_10_11_11_AS_16_16_16_16:
        case EdramSurfaceFormat::k_11_11_10_AS_16_16_16_16:
            return 4;
            
        case EdramSurfaceFormat::k_16_16_16_16:
        case EdramSurfaceFormat::k_16_16_16_16_EXPAND:
        case EdramSurfaceFormat::k_16_16_16_16_FLOAT:
        case EdramSurfaceFormat::k_32_32_FLOAT:
            return 8;
            
        case EdramSurfaceFormat::k_32_32_32_32_FLOAT:
            return 16;
            
        default:
            return 4;
    }
}

u32 EdramManager::calculate_tile_offset(u32 x, u32 y, u32 pitch, EdramMsaaMode msaa) {
    // Xbox 360 eDRAM uses 80x16 pixel tiles
    u32 tile_x = x / EDRAM_TILE_WIDTH;
    u32 tile_y = y / EDRAM_TILE_HEIGHT;
    
    u32 pixel_x = x % EDRAM_TILE_WIDTH;
    u32 pixel_y = y % EDRAM_TILE_HEIGHT;
    
    // MSAA multiplier
    u32 msaa_mult = 1;
    switch (msaa) {
        case EdramMsaaMode::k2X: msaa_mult = 2; break;
        case EdramMsaaMode::k4X: msaa_mult = 4; break;
        default: break;
    }
    
    // Calculate tile offset
    u32 tile_offset = (tile_y * pitch + tile_x) * EDRAM_TILE_SIZE * msaa_mult;
    
    // Calculate pixel offset within tile
    u32 pixel_offset = pixel_y * EDRAM_TILE_WIDTH + pixel_x;
    
    return tile_offset + pixel_offset * msaa_mult;
}

void EdramManager::clear_render_target(u32 index, f32 r, f32 g, f32 b, f32 a) {
    if (index >= 4 || !render_targets_[index].enabled) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    const auto& rt = render_targets_[index];
    u32 bpp = get_bytes_per_pixel(rt.format);
    
    // Convert clear color to format
    u32 clear_value = 0;
    switch (rt.format) {
        case EdramSurfaceFormat::k_8_8_8_8:
            clear_value = (static_cast<u32>(a * 255.0f) << 24) |
                          (static_cast<u32>(b * 255.0f) << 16) |
                          (static_cast<u32>(g * 255.0f) << 8) |
                          static_cast<u32>(r * 255.0f);
            break;
        case EdramSurfaceFormat::k_16_16_16_16_FLOAT:
            // Would need f16 conversion
            break;
        default:
            clear_value = 0xFF000000;
            break;
    }
    
    // Calculate size in tiles
    u32 tile_count = rt.edram_pitch * (rt.resolve_height / EDRAM_TILE_HEIGHT + 1);
    u32 size = tile_count * EDRAM_TILE_SIZE * bpp;
    
    u32 offset = rt.edram_base * EDRAM_TILE_SIZE * bpp;
    if (offset + size > EDRAM_SIZE) {
        size = EDRAM_SIZE - offset;
    }
    
    // Clear with pattern
    u8* ptr = data_.data() + offset;
    for (u32 i = 0; i < size / bpp; i++) {
        memcpy(ptr + i * bpp, &clear_value, bpp);
    }
}

void EdramManager::clear_depth_stencil(f32 depth, u8 stencil) {
    if (!depth_stencil_.enabled) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // D24S8 format: 24-bit depth, 8-bit stencil
    u32 depth_24 = static_cast<u32>(depth * 16777215.0f); // 2^24 - 1
    u32 clear_value = (depth_24 << 8) | stencil;
    
    u32 offset = depth_stencil_.edram_base * EDRAM_TILE_SIZE * 4;
    u32 pitch_tiles = depth_stencil_.edram_pitch;
    
    // Clear depth buffer
    u32* ptr = reinterpret_cast<u32*>(data_.data() + offset);
    u32 count = pitch_tiles * EDRAM_TILE_SIZE;
    
    for (u32 i = 0; i < count && (offset + i * 4) < EDRAM_SIZE; i++) {
        ptr[i] = clear_value;
    }
}

void EdramManager::resolve_render_target(u32 index, Memory* memory) {
    if (index >= 4 || !render_targets_[index].enabled) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    const auto& rt = render_targets_[index];
    
    if (rt.resolve_address == 0 || rt.resolve_width == 0 || rt.resolve_height == 0) {
        return;
    }
    
    u32 bpp = get_bytes_per_pixel(rt.format);
    u32 src_offset = rt.edram_base * EDRAM_TILE_SIZE * bpp;
    
    // Allocate temporary buffer for untiled data
    std::vector<u8> untiled(rt.resolve_width * rt.resolve_height * bpp);
    
    // Untile from eDRAM format to linear
    untile_surface(data_.data() + src_offset, untiled.data(),
                   rt.resolve_width, rt.resolve_height, bpp,
                   rt.edram_pitch * EDRAM_TILE_WIDTH * bpp,
                   rt.resolve_width * bpp);
    
    // Handle MSAA resolve if needed
    std::vector<u8> resolved;
    const u8* src_data = untiled.data();
    
    if (rt.msaa != EdramMsaaMode::k1X) {
        resolved.resize(rt.resolve_width * rt.resolve_height * bpp);
        
        if (rt.msaa == EdramMsaaMode::k2X) {
            resolve_msaa_2x(untiled.data(), resolved.data(),
                           rt.resolve_width, rt.resolve_height, bpp);
        } else {
            resolve_msaa_4x(untiled.data(), resolved.data(),
                           rt.resolve_width, rt.resolve_height, bpp);
        }
        
        src_data = resolved.data();
    }
    
    // Copy to main memory with big-endian conversion
    for (u32 y = 0; y < rt.resolve_height; y++) {
        for (u32 x = 0; x < rt.resolve_width; x++) {
            GuestAddr dst_addr = rt.resolve_address + y * rt.resolve_pitch + x * bpp;
            const u8* src_pixel = src_data + (y * rt.resolve_width + x) * bpp;
            
            // Write with byte swap for big-endian
            if (bpp == 4) {
                u32 value = *reinterpret_cast<const u32*>(src_pixel);
                // RGBA to ARGB for Xbox format
                memory->write_u32(dst_addr, value);
            } else if (bpp == 2) {
                u16 value = *reinterpret_cast<const u16*>(src_pixel);
                memory->write_u16(dst_addr, value);
            } else {
                memory->write_u8(dst_addr, *src_pixel);
            }
        }
    }
    
    LOGD("Resolved RT%u: %ux%u to 0x%08X", index, 
         rt.resolve_width, rt.resolve_height, rt.resolve_address);
}

void EdramManager::resolve_depth_stencil(Memory* memory) {
    if (!depth_stencil_.enabled) return;
    
    // Similar to render target resolve but for depth buffer
    // Implementation would follow same pattern
}

void EdramManager::copy_to_edram(u32 edram_offset, GuestAddr src_address,
                                  u32 width, u32 height, EdramSurfaceFormat format,
                                  Memory* memory) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    u32 bpp = get_bytes_per_pixel(format);
    
    // Read from main memory and tile into eDRAM
    std::vector<u8> linear(width * height * bpp);
    
    // Read linear data from memory
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            GuestAddr src_addr = src_address + y * width * bpp + x * bpp;
            u8* dst = linear.data() + (y * width + x) * bpp;
            
            if (bpp == 4) {
                *reinterpret_cast<u32*>(dst) = memory->read_u32(src_addr);
            } else if (bpp == 2) {
                *reinterpret_cast<u16*>(dst) = memory->read_u16(src_addr);
            } else {
                *dst = memory->read_u8(src_addr);
            }
        }
    }
    
    // Tile into eDRAM
    u32 dst_offset = edram_offset * EDRAM_TILE_SIZE * bpp;
    if (dst_offset + width * height * bpp <= EDRAM_SIZE) {
        tile_surface(linear.data(), data_.data() + dst_offset,
                     width, height, bpp,
                     width * bpp, EDRAM_TILE_WIDTH * bpp);
    }
}

void EdramManager::untile_surface(const u8* src, u8* dst,
                                   u32 width, u32 height, u32 bpp,
                                   u32 src_pitch, u32 dst_pitch) {
    // Xbox 360 uses 80x16 pixel tiles with Morton order within tiles
    
    u32 tiles_x = (width + EDRAM_TILE_WIDTH - 1) / EDRAM_TILE_WIDTH;
    u32 tiles_y = (height + EDRAM_TILE_HEIGHT - 1) / EDRAM_TILE_HEIGHT;
    
    for (u32 ty = 0; ty < tiles_y; ty++) {
        for (u32 tx = 0; tx < tiles_x; tx++) {
            // Calculate tile offset in source
            u32 tile_offset = (ty * tiles_x + tx) * EDRAM_TILE_SIZE * bpp;
            const u8* tile_src = src + tile_offset;
            
            // Copy pixels from tile to linear
            for (u32 py = 0; py < EDRAM_TILE_HEIGHT; py++) {
                u32 y = ty * EDRAM_TILE_HEIGHT + py;
                if (y >= height) continue;
                
                for (u32 px = 0; px < EDRAM_TILE_WIDTH; px++) {
                    u32 x = tx * EDRAM_TILE_WIDTH + px;
                    if (x >= width) continue;
                    
                    // Morton encoding within tile
                    u32 morton = TextureUntiler::morton_encode(px, py);
                    const u8* src_pixel = tile_src + morton * bpp;
                    u8* dst_pixel = dst + y * dst_pitch + x * bpp;
                    
                    memcpy(dst_pixel, src_pixel, bpp);
                }
            }
        }
    }
}

void EdramManager::tile_surface(const u8* src, u8* dst,
                                 u32 width, u32 height, u32 bpp,
                                 u32 src_pitch, u32 dst_pitch) {
    // Reverse of untile_surface
    
    u32 tiles_x = (width + EDRAM_TILE_WIDTH - 1) / EDRAM_TILE_WIDTH;
    u32 tiles_y = (height + EDRAM_TILE_HEIGHT - 1) / EDRAM_TILE_HEIGHT;
    
    for (u32 ty = 0; ty < tiles_y; ty++) {
        for (u32 tx = 0; tx < tiles_x; tx++) {
            u32 tile_offset = (ty * tiles_x + tx) * EDRAM_TILE_SIZE * bpp;
            u8* tile_dst = dst + tile_offset;
            
            for (u32 py = 0; py < EDRAM_TILE_HEIGHT; py++) {
                u32 y = ty * EDRAM_TILE_HEIGHT + py;
                if (y >= height) continue;
                
                for (u32 px = 0; px < EDRAM_TILE_WIDTH; px++) {
                    u32 x = tx * EDRAM_TILE_WIDTH + px;
                    if (x >= width) continue;
                    
                    u32 morton = TextureUntiler::morton_encode(px, py);
                    const u8* src_pixel = src + y * src_pitch + x * bpp;
                    u8* dst_pixel = tile_dst + morton * bpp;
                    
                    memcpy(dst_pixel, src_pixel, bpp);
                }
            }
        }
    }
}

void EdramManager::resolve_msaa_2x(const u8* src, u8* dst,
                                    u32 width, u32 height, u32 bpp) {
    // 2x MSAA: average 2 samples per pixel (horizontal pattern)
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            const u8* sample0 = src + (y * width + x) * 2 * bpp;
            const u8* sample1 = sample0 + bpp;
            u8* out = dst + (y * width + x) * bpp;
            
            // Average samples based on format
            if (bpp == 4) {
                // RGBA8
                for (int c = 0; c < 4; c++) {
                    out[c] = (sample0[c] + sample1[c]) / 2;
                }
            } else {
                memcpy(out, sample0, bpp); // Fallback
            }
        }
    }
}

void EdramManager::resolve_msaa_4x(const u8* src, u8* dst,
                                    u32 width, u32 height, u32 bpp) {
    // 4x MSAA: average 4 samples per pixel (2x2 pattern)
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            const u8* sample0 = src + (y * width + x) * 4 * bpp;
            const u8* sample1 = sample0 + bpp;
            const u8* sample2 = sample1 + bpp;
            const u8* sample3 = sample2 + bpp;
            u8* out = dst + (y * width + x) * bpp;
            
            if (bpp == 4) {
                for (int c = 0; c < 4; c++) {
                    out[c] = (sample0[c] + sample1[c] + sample2[c] + sample3[c]) / 4;
                }
            } else {
                memcpy(out, sample0, bpp);
            }
        }
    }
}

//=============================================================================
// TextureUntiler Implementation
//=============================================================================

void TextureUntiler::untile_2d(const u8* src, u8* dst,
                                u32 width, u32 height, u32 bpp,
                                u32 block_width, u32 block_height) {
    // Xbox 360 2D texture tiling uses a modified Morton curve
    // Block size is typically 32x32 for most formats
    
    if (block_width == 0) block_width = 32;
    if (block_height == 0) block_height = 32;
    
    u32 blocks_x = (width + block_width - 1) / block_width;
    u32 blocks_y = (height + block_height - 1) / block_height;
    
    for (u32 by = 0; by < blocks_y; by++) {
        for (u32 bx = 0; bx < blocks_x; bx++) {
            u32 block_offset = morton_encode(bx, by) * block_width * block_height * bpp;
            const u8* block_src = src + block_offset;
            
            for (u32 py = 0; py < block_height; py++) {
                u32 y = by * block_height + py;
                if (y >= height) continue;
                
                for (u32 px = 0; px < block_width; px++) {
                    u32 x = bx * block_width + px;
                    if (x >= width) continue;
                    
                    u32 tiled_offset = morton_encode(px, py) * bpp;
                    const u8* src_pixel = block_src + tiled_offset;
                    u8* dst_pixel = dst + (y * width + x) * bpp;
                    
                    memcpy(dst_pixel, src_pixel, bpp);
                }
            }
        }
    }
}

void TextureUntiler::untile_3d(const u8* src, u8* dst,
                                u32 width, u32 height, u32 depth, u32 bpp) {
    // 3D texture tiling - extend Morton curve to 3D
    for (u32 z = 0; z < depth; z++) {
        for (u32 y = 0; y < height; y++) {
            for (u32 x = 0; x < width; x++) {
                u32 tiled_offset = get_tiled_offset_3d(x, y, z, width, height, bpp);
                u32 linear_offset = (z * height * width + y * width + x) * bpp;
                
                memcpy(dst + linear_offset, src + tiled_offset, bpp);
            }
        }
    }
}

u32 TextureUntiler::get_tiled_offset_2d(u32 x, u32 y, u32 width, u32 bpp) {
    // Calculate tiled offset using Morton encoding
    u32 aligned_width = (width + 31) & ~31;
    u32 macro_x = x / 32;
    u32 macro_y = y / 32;
    u32 micro_x = x % 32;
    u32 micro_y = y % 32;
    
    u32 macro_offset = morton_encode(macro_x, macro_y) * 32 * 32 * bpp;
    u32 micro_offset = morton_encode(micro_x, micro_y) * bpp;
    
    return macro_offset + micro_offset;
}

u32 TextureUntiler::get_tiled_offset_3d(u32 x, u32 y, u32 z, 
                                         u32 width, u32 height, u32 bpp) {
    // 3D Morton encoding
    u32 offset = 0;
    u32 mask = 1;
    u32 tx = x, ty = y, tz = z;
    
    for (int i = 0; i < 10; i++) {
        offset |= ((tx & 1) << (i * 3));
        offset |= ((ty & 1) << (i * 3 + 1));
        offset |= ((tz & 1) << (i * 3 + 2));
        tx >>= 1;
        ty >>= 1;
        tz >>= 1;
    }
    
    return offset * bpp;
}

u32 TextureUntiler::morton_encode(u32 x, u32 y) {
    // Interleave bits of x and y
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

void TextureUntiler::morton_decode(u32 code, u32& x, u32& y) {
    // Extract interleaved bits
    x = code & 0x55555555;
    x = (x | (x >> 1)) & 0x33333333;
    x = (x | (x >> 2)) & 0x0F0F0F0F;
    x = (x | (x >> 4)) & 0x00FF00FF;
    x = (x | (x >> 8)) & 0x0000FFFF;
    
    y = (code >> 1) & 0x55555555;
    y = (y | (y >> 1)) & 0x33333333;
    y = (y | (y >> 2)) & 0x0F0F0F0F;
    y = (y | (y >> 4)) & 0x00FF00FF;
    y = (y | (y >> 8)) & 0x0000FFFF;
}

} // namespace x360mu

