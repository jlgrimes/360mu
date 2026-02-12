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
    // Xbox 360 two-level tiling:
    //   Level 1: 32x32 macro tiles, row-major across surface
    //   Level 2: 8x8 micro tiles within macro tile, Morton ordered
    //   Elements within micro tiles are also Morton ordered
    constexpr u32 MACRO_SIZE = 32;
    constexpr u32 MICRO_SIZE = 8;

    u32 macro_tiles_x = (width + MACRO_SIZE - 1) / MACRO_SIZE;

    // Macro tile position
    u32 macro_x = x / MACRO_SIZE;
    u32 macro_y = y / MACRO_SIZE;
    u32 macro_tile_size = MACRO_SIZE * MACRO_SIZE * bpp;
    u32 macro_offset = (macro_y * macro_tiles_x + macro_x) * macro_tile_size;

    // Position within macro tile
    u32 local_x = x % MACRO_SIZE;
    u32 local_y = y % MACRO_SIZE;

    // Micro tile position within macro tile (Morton ordered)
    u32 micro_tile_x = local_x / MICRO_SIZE;
    u32 micro_tile_y = local_y / MICRO_SIZE;
    u32 micro_tile_idx = morton_encode(micro_tile_x, micro_tile_y);
    u32 micro_tile_size = MICRO_SIZE * MICRO_SIZE * bpp;
    u32 micro_offset = micro_tile_idx * micro_tile_size;

    // Element within micro tile (Morton ordered)
    u32 element_x = local_x % MICRO_SIZE;
    u32 element_y = local_y % MICRO_SIZE;
    u32 element_idx = morton_encode(element_x, element_y);

    return macro_offset + micro_offset + element_idx * bpp;
}

u32 TextureUntiler::get_tiled_offset_3d(u32 x, u32 y, u32 z, u32 width, u32 height, u32 bpp) {
    // 3D tiling: each Z-slice is tiled independently as a 2D surface
    constexpr u32 MACRO_SIZE = 32;
    u32 macro_tiles_x = (width + MACRO_SIZE - 1) / MACRO_SIZE;
    u32 macro_tiles_y = (height + MACRO_SIZE - 1) / MACRO_SIZE;
    u32 tiled_slice_size = macro_tiles_x * macro_tiles_y * MACRO_SIZE * MACRO_SIZE * bpp;
    return z * tiled_slice_size + get_tiled_offset_2d(x, y, width, bpp);
}

void TextureUntiler::untile_2d(const u8* src, u8* dst,
                               u32 width, u32 height, u32 bpp,
                               u32 block_width, u32 block_height) {
    // For block-compressed textures, operate on blocks (bpp = bytes per block)
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
    // Untile each Z-slice as an independent 2D surface
    constexpr u32 MACRO_SIZE = 32;
    u32 macro_tiles_x = (width + MACRO_SIZE - 1) / MACRO_SIZE;
    u32 macro_tiles_y = (height + MACRO_SIZE - 1) / MACRO_SIZE;
    u32 tiled_slice_size = macro_tiles_x * macro_tiles_y * MACRO_SIZE * MACRO_SIZE * bpp;
    u32 linear_slice_size = width * height * bpp;

    for (u32 z = 0; z < depth; z++) {
        const u8* slice_src = src + z * tiled_slice_size;
        u8* slice_dst = dst + z * linear_slice_size;
        for (u32 y = 0; y < height; y++) {
            for (u32 x = 0; x < width; x++) {
                u32 tiled_offset = get_tiled_offset_2d(x, y, width, bpp);
                u32 linear_offset = (y * width + x) * bpp;
                memcpy(slice_dst + linear_offset, slice_src + tiled_offset, bpp);
            }
        }
    }
}

void TextureUntiler::untile_cube(const u8* src, u8* dst,
                                  u32 face_size, u32 bpp,
                                  u32 block_width, u32 block_height) {
    u32 blocks_per_face = (face_size + block_width - 1) / block_width;
    u32 blocks_y = blocks_per_face;  // Square faces
    u32 blocks_x = blocks_per_face;

    constexpr u32 MACRO_SIZE = 32;
    u32 macro_tiles_x = (blocks_x + MACRO_SIZE - 1) / MACRO_SIZE;
    u32 macro_tiles_y = (blocks_y + MACRO_SIZE - 1) / MACRO_SIZE;
    u32 tiled_face_size = macro_tiles_x * macro_tiles_y * MACRO_SIZE * MACRO_SIZE * bpp;
    u32 linear_face_size = blocks_x * blocks_y * bpp;

    for (u32 face = 0; face < 6; face++) {
        const u8* face_src = src + face * tiled_face_size;
        u8* face_dst = dst + face * linear_face_size;
        for (u32 by = 0; by < blocks_y; by++) {
            for (u32 bx = 0; bx < blocks_x; bx++) {
                u32 tiled_offset = get_tiled_offset_2d(bx, by, blocks_x, bpp);
                u32 linear_offset = (by * blocks_x + bx) * bpp;
                memcpy(face_dst + linear_offset, face_src + tiled_offset, bpp);
            }
        }
    }
}

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>

void TextureUntiler::untile_2d_neon(const u8* src, u8* dst,
                                     u32 width, u32 height, u32 bpp,
                                     u32 block_width, u32 block_height) {
    u32 blocks_x = (width + block_width - 1) / block_width;
    u32 blocks_y = (height + block_height - 1) / block_height;

    // NEON optimization: batch 4 offset calculations and memcpy for common bpp sizes
    if (bpp == 4) {
        // 4 bytes per block (e.g., RGBA8, R32F)
        // Process 4 blocks at a time using NEON vector gather
        for (u32 by = 0; by < blocks_y; by++) {
            u32 bx = 0;
            for (; bx + 4 <= blocks_x; bx += 4) {
                // Calculate 4 tiled offsets
                u32 off0 = get_tiled_offset_2d(bx + 0, by, blocks_x, bpp);
                u32 off1 = get_tiled_offset_2d(bx + 1, by, blocks_x, bpp);
                u32 off2 = get_tiled_offset_2d(bx + 2, by, blocks_x, bpp);
                u32 off3 = get_tiled_offset_2d(bx + 3, by, blocks_x, bpp);

                // Load 4 dwords from scattered tiled locations
                uint32x4_t vals = {
                    *reinterpret_cast<const u32*>(src + off0),
                    *reinterpret_cast<const u32*>(src + off1),
                    *reinterpret_cast<const u32*>(src + off2),
                    *reinterpret_cast<const u32*>(src + off3)
                };

                // Store 4 contiguous dwords to linear output
                u32 dst_offset = (by * blocks_x + bx) * bpp;
                vst1q_u32(reinterpret_cast<u32*>(dst + dst_offset), vals);
            }
            // Scalar tail
            for (; bx < blocks_x; bx++) {
                u32 tiled_offset = get_tiled_offset_2d(bx, by, blocks_x, bpp);
                u32 linear_offset = (by * blocks_x + bx) * bpp;
                memcpy(dst + linear_offset, src + tiled_offset, bpp);
            }
        }
    } else if (bpp == 8) {
        // 8 bytes per block (e.g., DXT1, DXT5A, RG16F)
        for (u32 by = 0; by < blocks_y; by++) {
            u32 bx = 0;
            for (; bx + 2 <= blocks_x; bx += 2) {
                u32 off0 = get_tiled_offset_2d(bx + 0, by, blocks_x, bpp);
                u32 off1 = get_tiled_offset_2d(bx + 1, by, blocks_x, bpp);

                // Load 2 x 8-byte blocks into a 128-bit register
                uint8x8_t v0 = vld1_u8(src + off0);
                uint8x8_t v1 = vld1_u8(src + off1);
                uint8x16_t combined = vcombine_u8(v0, v1);

                u32 dst_offset = (by * blocks_x + bx) * bpp;
                vst1q_u8(dst + dst_offset, combined);
            }
            for (; bx < blocks_x; bx++) {
                u32 tiled_offset = get_tiled_offset_2d(bx, by, blocks_x, bpp);
                u32 linear_offset = (by * blocks_x + bx) * bpp;
                memcpy(dst + linear_offset, src + tiled_offset, bpp);
            }
        }
    } else if (bpp == 16) {
        // 16 bytes per block (e.g., DXT3, DXT5, DXN, RGBA16F)
        for (u32 by = 0; by < blocks_y; by++) {
            for (u32 bx = 0; bx < blocks_x; bx++) {
                u32 tiled_offset = get_tiled_offset_2d(bx, by, blocks_x, bpp);
                u32 linear_offset = (by * blocks_x + bx) * bpp;
                // Single 128-bit NEON load/store
                uint8x16_t v = vld1q_u8(src + tiled_offset);
                vst1q_u8(dst + linear_offset, v);
            }
        }
    } else {
        // Fallback to scalar for unusual block sizes
        untile_2d(src, dst, width, height, bpp, block_width, block_height);
    }
}
#endif

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

void EdramManager::resolve_render_target(u32 index, Memory* memory,
                                          u32 src_x, u32 src_y, u32 w, u32 h) {
    if (index >= render_targets_.size() || !render_targets_[index].enabled || !memory) return;

    std::lock_guard<std::mutex> lock(mutex_);

    const auto& rt = render_targets_[index];
    if (rt.resolve_address == 0) return;

    u32 bpp = get_bytes_per_pixel(rt.format);
    u32 full_w = rt.resolve_width;

    // Clamp rect to surface bounds
    u32 x0 = std::min(src_x, full_w);
    u32 y0 = std::min(src_y, rt.resolve_height);
    u32 x1 = std::min(src_x + w, full_w);
    u32 y1 = std::min(src_y + h, rt.resolve_height);
    u32 rect_w = x1 - x0;
    u32 rect_h = y1 - y0;
    if (rect_w == 0 || rect_h == 0) return;

    std::vector<u8> temp(rect_w * rect_h * bpp);
    u32 edram_start = rt.edram_base * 4;

    for (u32 y = 0; y < rect_h; y++) {
        for (u32 x = 0; x < rect_w; x++) {
            u32 tile_offset = calculate_tile_offset(x0 + x, y0 + y, rt.edram_pitch, rt.msaa);
            u32 src_offset = edram_start + tile_offset * bpp;
            u32 dst_offset = (y * rect_w + x) * bpp;

            if (src_offset + bpp <= data_.size()) {
                if (rt.msaa != EdramMsaaMode::k1X) {
                    u32 sample_count = (rt.msaa == EdramMsaaMode::k2X) ? 2 : 4;
                    resolve_pixel(data_.data() + src_offset, temp.data() + dst_offset,
                                 bpp, sample_count);
                } else {
                    memcpy(temp.data() + dst_offset, data_.data() + src_offset, bpp);
                }
            }
        }
    }

    // Write to main memory at the correct offset within the destination surface
    void* base_dst = memory->get_host_ptr(rt.resolve_address);
    if (base_dst) {
        for (u32 y = 0; y < rect_h; y++) {
            u8* row_dst = static_cast<u8*>(base_dst) + (y0 + y) * rt.resolve_pitch + x0 * bpp;
            memcpy(row_dst, temp.data() + y * rect_w * bpp, rect_w * bpp);
        }
    }

    LOGD("Resolved RT%u subrect: (%u,%u) %ux%u to %08X", index, x0, y0, rect_w, rect_h, rt.resolve_address);
}

void EdramManager::resolve_depth_stencil(Memory* memory) {
    if (!depth_stencil_.enabled || !memory) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // D24S8: 4 bytes per pixel
    constexpr u32 bpp = 4;
    // Use a reasonable default resolution if no resolve config exists for depth
    // Depth resolve is less common - games usually just use depth in eDRAM
    u32 width = depth_stencil_.edram_pitch;
    u32 height = EDRAM_TILE_HEIGHT;

    if (width == 0 || height == 0) return;

    // Temporary buffer for untiled data
    std::vector<u8> temp(width * height * bpp);

    u32 edram_start = depth_stencil_.edram_base * 4;

    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            u32 tile_offset = calculate_tile_offset(x, y, depth_stencil_.edram_pitch,
                                                     depth_stencil_.msaa);
            u32 src_offset = edram_start + tile_offset * bpp;
            u32 dst_offset = (y * width + x) * bpp;

            if (src_offset + bpp <= data_.size()) {
                if (depth_stencil_.msaa != EdramMsaaMode::k1X) {
                    u32 sample_count = (depth_stencil_.msaa == EdramMsaaMode::k2X) ? 2 : 4;
                    resolve_pixel(data_.data() + src_offset, temp.data() + dst_offset,
                                 bpp, sample_count);
                } else {
                    memcpy(temp.data() + dst_offset, data_.data() + src_offset, bpp);
                }
            }
        }
    }

    LOGD("Depth/stencil resolve: %ux%u", width, height);
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
