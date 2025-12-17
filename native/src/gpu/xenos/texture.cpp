/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Texture Handling Implementation
 * 
 * DXT/BC texture decompression and format conversion.
 */

#include "texture.h"
#include "edram.h"
#include "../../memory/memory.h"
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-texture"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[TEXTURE] " __VA_ARGS__); printf("\n")
#define LOGD(...)
#endif

namespace x360mu {

//=============================================================================
// TextureDecompressor Implementation
//=============================================================================

void TextureDecompressor::color_565_to_rgba(u16 color, u8* rgba) {
    // Extract RGB components from 5:6:5 format
    u8 r = (color >> 11) & 0x1F;
    u8 g = (color >> 5) & 0x3F;
    u8 b = color & 0x1F;
    
    // Expand to 8-bit
    rgba[0] = (r << 3) | (r >> 2);
    rgba[1] = (g << 2) | (g >> 4);
    rgba[2] = (b << 3) | (b >> 2);
    rgba[3] = 255;
}

void TextureDecompressor::interpolate_colors(const u8* c0, const u8* c1, 
                                              u8* c2, u8* c3, bool dxt1) {
    if (dxt1) {
        // DXT1: 2/3, 1/3 interpolation OR black for punch-through alpha
        c2[0] = (2 * c0[0] + c1[0]) / 3;
        c2[1] = (2 * c0[1] + c1[1]) / 3;
        c2[2] = (2 * c0[2] + c1[2]) / 3;
        c2[3] = 255;
        
        c3[0] = (c0[0] + 2 * c1[0]) / 3;
        c3[1] = (c0[1] + 2 * c1[1]) / 3;
        c3[2] = (c0[2] + 2 * c1[2]) / 3;
        c3[3] = 255;
    } else {
        // DXT3/5: 2/3, 1/3 interpolation
        c2[0] = (2 * c0[0] + c1[0]) / 3;
        c2[1] = (2 * c0[1] + c1[1]) / 3;
        c2[2] = (2 * c0[2] + c1[2]) / 3;
        c2[3] = 255;
        
        c3[0] = (c0[0] + 2 * c1[0]) / 3;
        c3[1] = (c0[1] + 2 * c1[1]) / 3;
        c3[2] = (c0[2] + 2 * c1[2]) / 3;
        c3[3] = 255;
    }
}

void TextureDecompressor::decompress_dxt1_block(const u8* src, u8* dst, bool has_alpha) {
    // DXT1 block: 2 bytes color0, 2 bytes color1, 4 bytes indices
    
    // Read colors (little-endian in block, but Xbox textures are big-endian)
    u16 color0 = (src[1] << 8) | src[0];
    u16 color1 = (src[3] << 8) | src[2];
    
    // Decode color palette
    u8 palette[4][4];
    color_565_to_rgba(color0, palette[0]);
    color_565_to_rgba(color1, palette[1]);
    
    if (color0 > color1 || !has_alpha) {
        // Opaque block: interpolate colors
        interpolate_colors(palette[0], palette[1], palette[2], palette[3], false);
    } else {
        // Transparent block
        palette[2][0] = (palette[0][0] + palette[1][0]) / 2;
        palette[2][1] = (palette[0][1] + palette[1][1]) / 2;
        palette[2][2] = (palette[0][2] + palette[1][2]) / 2;
        palette[2][3] = 255;
        
        palette[3][0] = 0;
        palette[3][1] = 0;
        palette[3][2] = 0;
        palette[3][3] = 0;  // Transparent black
    }
    
    // Decode 4x4 block of pixels
    u32 indices = (src[7] << 24) | (src[6] << 16) | (src[5] << 8) | src[4];
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = (indices >> ((y * 4 + x) * 2)) & 3;
            u8* pixel = dst + (y * 4 + x) * 4;
            
            pixel[0] = palette[idx][0];
            pixel[1] = palette[idx][1];
            pixel[2] = palette[idx][2];
            pixel[3] = palette[idx][3];
        }
    }
}

void TextureDecompressor::decompress_dxt3_block(const u8* src, u8* dst) {
    // DXT3: 8 bytes explicit alpha, 8 bytes DXT1 color block
    
    // First, decompress the color part (last 8 bytes)
    decompress_dxt1_block(src + 8, dst, false);
    
    // Apply explicit alpha (first 8 bytes)
    // Each pixel gets 4 bits of alpha
    for (int y = 0; y < 4; y++) {
        u16 alpha_row = (src[y * 2 + 1] << 8) | src[y * 2];
        
        for (int x = 0; x < 4; x++) {
            u8 alpha4 = (alpha_row >> (x * 4)) & 0xF;
            u8 alpha8 = (alpha4 << 4) | alpha4;  // Expand to 8 bits
            
            dst[(y * 4 + x) * 4 + 3] = alpha8;
        }
    }
}

void TextureDecompressor::decompress_dxt5_block(const u8* src, u8* dst) {
    // DXT5: 8 bytes interpolated alpha, 8 bytes DXT1 color block
    
    // First, decompress the color part
    decompress_dxt1_block(src + 8, dst, false);
    
    // Decode alpha palette
    u8 alpha0 = src[0];
    u8 alpha1 = src[1];
    u8 alpha_palette[8];
    
    alpha_palette[0] = alpha0;
    alpha_palette[1] = alpha1;
    
    if (alpha0 > alpha1) {
        // 6 interpolated alpha values
        alpha_palette[2] = (6 * alpha0 + 1 * alpha1) / 7;
        alpha_palette[3] = (5 * alpha0 + 2 * alpha1) / 7;
        alpha_palette[4] = (4 * alpha0 + 3 * alpha1) / 7;
        alpha_palette[5] = (3 * alpha0 + 4 * alpha1) / 7;
        alpha_palette[6] = (2 * alpha0 + 5 * alpha1) / 7;
        alpha_palette[7] = (1 * alpha0 + 6 * alpha1) / 7;
    } else {
        // 4 interpolated + 0 and 255
        alpha_palette[2] = (4 * alpha0 + 1 * alpha1) / 5;
        alpha_palette[3] = (3 * alpha0 + 2 * alpha1) / 5;
        alpha_palette[4] = (2 * alpha0 + 3 * alpha1) / 5;
        alpha_palette[5] = (1 * alpha0 + 4 * alpha1) / 5;
        alpha_palette[6] = 0;
        alpha_palette[7] = 255;
    }
    
    // Decode alpha indices (48 bits = 16 x 3-bit indices)
    u64 alpha_bits = 0;
    for (int i = 0; i < 6; i++) {
        alpha_bits |= static_cast<u64>(src[2 + i]) << (i * 8);
    }
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = (alpha_bits >> ((y * 4 + x) * 3)) & 7;
            dst[(y * 4 + x) * 4 + 3] = alpha_palette[idx];
        }
    }
}

void TextureDecompressor::decompress_dxt5a_block(const u8* src, u8* dst) {
    // DXT5A (BC4): Single channel with DXT5-style interpolation
    
    u8 alpha0 = src[0];
    u8 alpha1 = src[1];
    u8 palette[8];
    
    palette[0] = alpha0;
    palette[1] = alpha1;
    
    if (alpha0 > alpha1) {
        palette[2] = (6 * alpha0 + 1 * alpha1) / 7;
        palette[3] = (5 * alpha0 + 2 * alpha1) / 7;
        palette[4] = (4 * alpha0 + 3 * alpha1) / 7;
        palette[5] = (3 * alpha0 + 4 * alpha1) / 7;
        palette[6] = (2 * alpha0 + 5 * alpha1) / 7;
        palette[7] = (1 * alpha0 + 6 * alpha1) / 7;
    } else {
        palette[2] = (4 * alpha0 + 1 * alpha1) / 5;
        palette[3] = (3 * alpha0 + 2 * alpha1) / 5;
        palette[4] = (2 * alpha0 + 3 * alpha1) / 5;
        palette[5] = (1 * alpha0 + 4 * alpha1) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }
    
    u64 bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= static_cast<u64>(src[2 + i]) << (i * 8);
    }
    
    for (int i = 0; i < 16; i++) {
        int idx = (bits >> (i * 3)) & 7;
        dst[i] = palette[idx];
    }
}

void TextureDecompressor::decompress_ctx1_block(const u8* src, u8* dst) {
    // CTX1: Xbox 360 specific 2-channel compression for normal maps
    // 8 bytes: 2 endpoint pairs + indices
    
    u8 x0 = src[0];
    u8 x1 = src[1];
    u8 y0 = src[2];
    u8 y1 = src[3];
    
    // Create palettes for X and Y channels
    u8 x_palette[4] = {
        x0,
        x1,
        static_cast<u8>((2 * x0 + x1) / 3),
        static_cast<u8>((x0 + 2 * x1) / 3)
    };
    
    u8 y_palette[4] = {
        y0,
        y1,
        static_cast<u8>((2 * y0 + y1) / 3),
        static_cast<u8>((y0 + 2 * y1) / 3)
    };
    
    // Decode indices
    u32 indices = (src[7] << 24) | (src[6] << 16) | (src[5] << 8) | src[4];
    
    for (int i = 0; i < 16; i++) {
        int idx = (indices >> (i * 2)) & 3;
        
        // Output as RG (normal map XY)
        dst[i * 4 + 0] = x_palette[idx];
        dst[i * 4 + 1] = y_palette[idx];
        
        // Reconstruct Z from X and Y (assuming normalized)
        // Z = sqrt(1 - X^2 - Y^2)
        f32 x = (x_palette[idx] / 255.0f) * 2.0f - 1.0f;
        f32 y = (y_palette[idx] / 255.0f) * 2.0f - 1.0f;
        f32 z_sq = 1.0f - x * x - y * y;
        f32 z = z_sq > 0.0f ? std::sqrt(z_sq) : 0.0f;
        dst[i * 4 + 2] = static_cast<u8>((z * 0.5f + 0.5f) * 255.0f);
        dst[i * 4 + 3] = 255;
    }
}

void TextureDecompressor::decompress_texture(const u8* src, u8* dst,
                                              u32 width, u32 height,
                                              TextureFormat format) {
    if (!is_compressed(format)) {
        return;  // Not a compressed format
    }
    
    u32 block_width = (width + 3) / 4;
    u32 block_height = (height + 3) / 4;
    u32 block_size = get_bytes_per_block(format);
    
    for (u32 by = 0; by < block_height; by++) {
        for (u32 bx = 0; bx < block_width; bx++) {
            const u8* block_src = src + (by * block_width + bx) * block_size;
            
            // Decompress to temp 4x4 block
            u8 block[64];  // 4x4 RGBA
            
            switch (format) {
                case TextureFormat::k_DXT1:
                case TextureFormat::k_DXT1_AS_16_16_16_16:
                    decompress_dxt1_block(block_src, block, true);
                    break;
                    
                case TextureFormat::k_DXT2_3:
                case TextureFormat::k_DXT2_3_AS_16_16_16_16:
                    decompress_dxt3_block(block_src, block);
                    break;
                    
                case TextureFormat::k_DXT4_5:
                case TextureFormat::k_DXT4_5_AS_16_16_16_16:
                    decompress_dxt5_block(block_src, block);
                    break;
                    
                case TextureFormat::k_DXT5A:
                    decompress_dxt5a_block(block_src, block);
                    break;
                    
                case TextureFormat::k_CTX1:
                    decompress_ctx1_block(block_src, block);
                    break;
                    
                default:
                    memset(block, 0, 64);
                    break;
            }
            
            // Copy block to destination
            for (u32 py = 0; py < 4; py++) {
                u32 y = by * 4 + py;
                if (y >= height) continue;
                
                for (u32 px = 0; px < 4; px++) {
                    u32 x = bx * 4 + px;
                    if (x >= width) continue;
                    
                    u8* dst_pixel = dst + (y * width + x) * 4;
                    const u8* src_pixel = block + (py * 4 + px) * 4;
                    memcpy(dst_pixel, src_pixel, 4);
                }
            }
        }
    }
}

u32 TextureDecompressor::get_block_size(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT5A:
        case TextureFormat::k_CTX1:
            return 4;  // 4x4 block
            
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
            return 4;  // 4x4 block
            
        default:
            return 1;  // Not block compressed
    }
}

bool TextureDecompressor::is_compressed(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT5A:
        case TextureFormat::k_CTX1:
            return true;
        default:
            return false;
    }
}

u32 TextureDecompressor::get_bytes_per_block(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT5A:
        case TextureFormat::k_CTX1:
            return 8;
            
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
            return 16;
            
        // Uncompressed formats
        case TextureFormat::k_8:
        case TextureFormat::k_8_A:
        case TextureFormat::k_8_B:
            return 1;
            
        case TextureFormat::k_5_6_5:
        case TextureFormat::k_1_5_5_5:
        case TextureFormat::k_4_4_4_4:
        case TextureFormat::k_8_8:
        case TextureFormat::k_16_FLOAT:
            return 2;
            
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_8_8_8_8_A:
        case TextureFormat::k_8_8_8_8_GAMMA:
        case TextureFormat::k_2_10_10_10:
        case TextureFormat::k_2_10_10_10_GAMMA:
        case TextureFormat::k_10_11_11:
        case TextureFormat::k_11_11_10:
        case TextureFormat::k_16_16:
        case TextureFormat::k_16_16_FLOAT:
        case TextureFormat::k_32_FLOAT:
            return 4;
            
        case TextureFormat::k_16_16_16_16:
        case TextureFormat::k_16_16_16_16_FLOAT:
        case TextureFormat::k_32_32_FLOAT:
            return 8;
            
        case TextureFormat::k_32_32_32_32_FLOAT:
        case TextureFormat::k_32_32_32_FLOAT:
            return 16;
            
        default:
            return 4;
    }
}

u32 TextureDecompressor::calculate_texture_size(u32 width, u32 height, u32 depth,
                                                 TextureFormat format, u32 mip_levels) {
    u32 total_size = 0;
    
    for (u32 mip = 0; mip < mip_levels; mip++) {
        u32 mip_width = std::max(1u, width >> mip);
        u32 mip_height = std::max(1u, height >> mip);
        u32 mip_depth = std::max(1u, depth >> mip);
        
        if (is_compressed(format)) {
            u32 block_width = (mip_width + 3) / 4;
            u32 block_height = (mip_height + 3) / 4;
            total_size += block_width * block_height * mip_depth * get_bytes_per_block(format);
        } else {
            total_size += mip_width * mip_height * mip_depth * get_bytes_per_block(format);
        }
    }
    
    return total_size;
}

//=============================================================================
// TextureCache Implementation
//=============================================================================

TextureCache::TextureCache()
    : access_counter_(0)
    , max_size_(256 * 1024 * 1024)
    , current_size_(0)
    , stats_{} {
}

TextureCache::~TextureCache() = default;

Status TextureCache::initialize(u32 max_size_mb) {
    max_size_ = static_cast<u64>(max_size_mb) * 1024 * 1024;
    entries_.clear();
    current_size_ = 0;
    stats_ = {};
    
    LOGI("Texture cache initialized (max %u MB)", max_size_mb);
    return Status::Ok;
}

void TextureCache::shutdown() {
    entries_.clear();
    current_size_ = 0;
}

u64 TextureCache::calculate_hash(const TextureInfo& info, const u8* data, u32 size) {
    // Simple FNV-1a hash
    u64 hash = 14695981039346656037ULL;
    
    // Hash texture info
    hash ^= info.base_address;
    hash *= 1099511628211ULL;
    hash ^= info.width;
    hash *= 1099511628211ULL;
    hash ^= info.height;
    hash *= 1099511628211ULL;
    hash ^= static_cast<u64>(info.format);
    hash *= 1099511628211ULL;
    
    // Hash some of the data (sample for speed)
    u32 sample_stride = std::max(1u, size / 256);
    for (u32 i = 0; i < size; i += sample_stride) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    
    return hash;
}

const u8* TextureCache::get_texture(const TextureInfo& info, Memory* memory) {
    // Calculate cache key from address and parameters
    u64 key = info.base_address ^ (static_cast<u64>(info.width) << 32) ^
              (static_cast<u64>(info.height) << 48) ^
              (static_cast<u64>(info.format) << 56);
    
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second->last_access = ++access_counter_;
        stats_.hits++;
        return it->second->data.data();
    }
    
    stats_.misses++;
    
    // Calculate source size
    u32 src_size = TextureDecompressor::calculate_texture_size(
        info.width, info.height, std::max(1u, info.depth),
        info.format, info.mip_levels ? info.mip_levels : 1);
    
    // Read source data from memory
    std::vector<u8> src_data(src_size);
    for (u32 i = 0; i < src_size; i++) {
        src_data[i] = memory->read_u8(info.base_address + i);
    }
    
    // Untile if needed
    if (info.tiled) {
        std::vector<u8> untiled(src_size);
        TextureUntiler::untile_2d(src_data.data(), untiled.data(),
                                   info.width, info.height,
                                   TextureDecompressor::get_bytes_per_block(info.format),
                                   32, 32);
        src_data = std::move(untiled);
    }
    
    // Create entry
    auto entry = std::make_unique<CacheEntry>();
    entry->info = info;
    entry->last_access = ++access_counter_;
    
    // Decode/decompress texture
    u32 dst_size = info.width * info.height * 4;  // RGBA8
    entry->data.resize(dst_size);
    
    decode_texture(info, src_data.data(), entry->data.data(), memory);
    
    entry->hash = calculate_hash(info, src_data.data(), src_size);
    
    // Evict if necessary
    while (current_size_ + dst_size > max_size_ && !entries_.empty()) {
        evict_oldest();
    }
    
    current_size_ += dst_size;
    stats_.entry_count = entries_.size() + 1;
    stats_.memory_used = current_size_;
    stats_.uploads++;
    
    const u8* result = entry->data.data();
    entries_[key] = std::move(entry);
    
    return result;
}

void TextureCache::decode_texture(const TextureInfo& info, const u8* src, u8* dst, Memory*) {
    if (TextureDecompressor::is_compressed(info.format)) {
        TextureDecompressor::decompress_texture(src, dst, info.width, info.height, info.format);
    } else {
        TextureFormatConverter::convert_to_rgba8(src, dst, info.width, info.height, info.format);
    }
    
    // Apply swizzle if not identity
    if (info.swizzle_x != 0 || info.swizzle_y != 1 || 
        info.swizzle_z != 2 || info.swizzle_w != 3) {
        TextureFormatConverter::apply_swizzle(dst, info.width * info.height,
                                               info.swizzle_x, info.swizzle_y,
                                               info.swizzle_z, info.swizzle_w);
    }
}

void TextureCache::invalidate_range(GuestAddr address, u32 size) {
    GuestAddr end_address = address + size;
    
    for (auto it = entries_.begin(); it != entries_.end();) {
        const auto& info = it->second->info;
        u32 tex_size = TextureDecompressor::calculate_texture_size(
            info.width, info.height, std::max(1u, info.depth),
            info.format, info.mip_levels ? info.mip_levels : 1);
        
        // Check for overlap
        if (info.base_address < end_address && 
            info.base_address + tex_size > address) {
            current_size_ -= it->second->data.size();
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    
    stats_.entry_count = entries_.size();
    stats_.memory_used = current_size_;
}

void TextureCache::invalidate_all() {
    entries_.clear();
    current_size_ = 0;
    stats_.entry_count = 0;
    stats_.memory_used = 0;
}

void TextureCache::evict_oldest() {
    if (entries_.empty()) return;
    
    auto oldest = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second->last_access < oldest->second->last_access) {
            oldest = it;
        }
    }
    
    current_size_ -= oldest->second->data.size();
    entries_.erase(oldest);
}

TextureCache::Stats TextureCache::get_stats() const {
    return stats_;
}

//=============================================================================
// TextureFormatConverter Implementation
//=============================================================================

void TextureFormatConverter::convert_to_rgba8(const u8* src, u8* dst,
                                               u32 width, u32 height,
                                               TextureFormat format) {
    u32 pixel_count = width * height;
    
    switch (format) {
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_8_8_8_8_GAMMA:
            // Already RGBA8, just copy (with possible byte swap)
            memcpy(dst, src, pixel_count * 4);
            break;
            
        case TextureFormat::k_5_6_5:
            convert_565_to_rgba8(src, dst, pixel_count);
            break;
            
        case TextureFormat::k_1_5_5_5:
            convert_1555_to_rgba8(src, dst, pixel_count);
            break;
            
        case TextureFormat::k_4_4_4_4:
            convert_4444_to_rgba8(src, dst, pixel_count);
            break;
            
        case TextureFormat::k_2_10_10_10:
        case TextureFormat::k_2_10_10_10_GAMMA:
            convert_2101010_to_rgba8(src, dst, pixel_count);
            break;
            
        case TextureFormat::k_16_16_FLOAT:
            convert_rg16f_to_rgba8(src, dst, pixel_count);
            break;
            
        case TextureFormat::k_16_16_16_16_FLOAT:
            convert_rgba16f_to_rgba8(src, dst, pixel_count);
            break;
            
        case TextureFormat::k_8:
        case TextureFormat::k_8_A:
        case TextureFormat::k_8_B:
            // Single channel to RGBA
            for (u32 i = 0; i < pixel_count; i++) {
                dst[i * 4 + 0] = src[i];
                dst[i * 4 + 1] = src[i];
                dst[i * 4 + 2] = src[i];
                dst[i * 4 + 3] = 255;
            }
            break;
            
        case TextureFormat::k_8_8:
            // Two channels to RGBA
            for (u32 i = 0; i < pixel_count; i++) {
                dst[i * 4 + 0] = src[i * 2 + 0];
                dst[i * 4 + 1] = src[i * 2 + 1];
                dst[i * 4 + 2] = 0;
                dst[i * 4 + 3] = 255;
            }
            break;
            
        default:
            // Unknown format, fill with magenta for debugging
            for (u32 i = 0; i < pixel_count; i++) {
                dst[i * 4 + 0] = 255;
                dst[i * 4 + 1] = 0;
                dst[i * 4 + 2] = 255;
                dst[i * 4 + 3] = 255;
            }
            break;
    }
}

void TextureFormatConverter::convert_565_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    const u16* src16 = reinterpret_cast<const u16*>(src);
    
    for (u32 i = 0; i < pixel_count; i++) {
        u16 color = src16[i];
        
        u8 r = (color >> 11) & 0x1F;
        u8 g = (color >> 5) & 0x3F;
        u8 b = color & 0x1F;
        
        dst[i * 4 + 0] = (r << 3) | (r >> 2);
        dst[i * 4 + 1] = (g << 2) | (g >> 4);
        dst[i * 4 + 2] = (b << 3) | (b >> 2);
        dst[i * 4 + 3] = 255;
    }
}

void TextureFormatConverter::convert_1555_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    const u16* src16 = reinterpret_cast<const u16*>(src);
    
    for (u32 i = 0; i < pixel_count; i++) {
        u16 color = src16[i];
        
        u8 a = (color >> 15) ? 255 : 0;
        u8 r = (color >> 10) & 0x1F;
        u8 g = (color >> 5) & 0x1F;
        u8 b = color & 0x1F;
        
        dst[i * 4 + 0] = (r << 3) | (r >> 2);
        dst[i * 4 + 1] = (g << 3) | (g >> 2);
        dst[i * 4 + 2] = (b << 3) | (b >> 2);
        dst[i * 4 + 3] = a;
    }
}

void TextureFormatConverter::convert_4444_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    const u16* src16 = reinterpret_cast<const u16*>(src);
    
    for (u32 i = 0; i < pixel_count; i++) {
        u16 color = src16[i];
        
        u8 a = (color >> 12) & 0xF;
        u8 r = (color >> 8) & 0xF;
        u8 g = (color >> 4) & 0xF;
        u8 b = color & 0xF;
        
        dst[i * 4 + 0] = (r << 4) | r;
        dst[i * 4 + 1] = (g << 4) | g;
        dst[i * 4 + 2] = (b << 4) | b;
        dst[i * 4 + 3] = (a << 4) | a;
    }
}

void TextureFormatConverter::convert_2101010_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    const u32* src32 = reinterpret_cast<const u32*>(src);
    
    for (u32 i = 0; i < pixel_count; i++) {
        u32 color = src32[i];
        
        u32 r = color & 0x3FF;
        u32 g = (color >> 10) & 0x3FF;
        u32 b = (color >> 20) & 0x3FF;
        u32 a = (color >> 30) & 0x3;
        
        dst[i * 4 + 0] = r >> 2;
        dst[i * 4 + 1] = g >> 2;
        dst[i * 4 + 2] = b >> 2;
        dst[i * 4 + 3] = (a << 6) | (a << 4) | (a << 2) | a;
    }
}

f32 TextureFormatConverter::half_to_float(u16 h) {
    u32 sign = (h >> 15) & 1;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    
    u32 f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            // Denormal
            exp = 1;
            while ((mant & 0x400) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3FF;
            f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7F800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    
    return *reinterpret_cast<f32*>(&f);
}

void TextureFormatConverter::convert_rg16f_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    const u16* src16 = reinterpret_cast<const u16*>(src);
    
    for (u32 i = 0; i < pixel_count; i++) {
        f32 r = half_to_float(src16[i * 2 + 0]);
        f32 g = half_to_float(src16[i * 2 + 1]);
        
        dst[i * 4 + 0] = static_cast<u8>(std::clamp(r * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 1] = static_cast<u8>(std::clamp(g * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 2] = 0;
        dst[i * 4 + 3] = 255;
    }
}

void TextureFormatConverter::convert_rgba16f_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    const u16* src16 = reinterpret_cast<const u16*>(src);
    
    for (u32 i = 0; i < pixel_count; i++) {
        f32 r = half_to_float(src16[i * 4 + 0]);
        f32 g = half_to_float(src16[i * 4 + 1]);
        f32 b = half_to_float(src16[i * 4 + 2]);
        f32 a = half_to_float(src16[i * 4 + 3]);
        
        dst[i * 4 + 0] = static_cast<u8>(std::clamp(r * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 1] = static_cast<u8>(std::clamp(g * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 2] = static_cast<u8>(std::clamp(b * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 3] = static_cast<u8>(std::clamp(a * 255.0f, 0.0f, 255.0f));
    }
}

void TextureFormatConverter::byte_swap_16(u8* data, u32 size) {
    u16* data16 = reinterpret_cast<u16*>(data);
    u32 count = size / 2;
    
    for (u32 i = 0; i < count; i++) {
        data16[i] = (data16[i] >> 8) | (data16[i] << 8);
    }
}

void TextureFormatConverter::byte_swap_32(u8* data, u32 size) {
    u32* data32 = reinterpret_cast<u32*>(data);
    u32 count = size / 4;
    
    for (u32 i = 0; i < count; i++) {
        u32 v = data32[i];
        data32[i] = ((v >> 24) & 0xFF) |
                    ((v >> 8) & 0xFF00) |
                    ((v << 8) & 0xFF0000) |
                    ((v << 24) & 0xFF000000);
    }
}

void TextureFormatConverter::apply_swizzle(u8* data, u32 pixel_count,
                                            u8 swizzle_r, u8 swizzle_g,
                                            u8 swizzle_b, u8 swizzle_a) {
    // Swizzle values: 0=R, 1=G, 2=B, 3=A, 4=0, 5=1
    for (u32 i = 0; i < pixel_count; i++) {
        u8* pixel = data + i * 4;
        u8 orig[4] = { pixel[0], pixel[1], pixel[2], pixel[3] };
        
        auto get_component = [&orig](u8 swizzle) -> u8 {
            if (swizzle < 4) return orig[swizzle];
            if (swizzle == 4) return 0;
            return 255;
        };
        
        pixel[0] = get_component(swizzle_r);
        pixel[1] = get_component(swizzle_g);
        pixel[2] = get_component(swizzle_b);
        pixel[3] = get_component(swizzle_a);
    }
}

} // namespace x360mu

