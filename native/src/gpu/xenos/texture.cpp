/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Texture Handling Implementation
 * 
 * Handles Xbox 360 texture formats including:
 * - DXT1/BC1, DXT2/3/BC2, DXT4/5/BC3 compressed textures
 * - CTX1 (Xbox 360 specific 2-channel normal map compression)
 * - Various uncompressed formats
 * - Texture tiling/untiling
 * - Mipmap handling
 * - Texture caching with LRU eviction
 */

#include "texture.h"
#include "edram.h"
#include "memory/memory.h"
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "360mu-texture"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) printf("[TEXTURE] " __VA_ARGS__); printf("\n")
#define LOGE(...) fprintf(stderr, "[TEXTURE ERROR] " __VA_ARGS__); fprintf(stderr, "\n")
#define LOGD(...) /* debug disabled */
#endif

namespace x360mu {

//=============================================================================
// Texture Format Utilities
//=============================================================================

bool TextureDecompressor::is_compressed(TextureFormat format) {
    switch (format) {
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT5A:
        case TextureFormat::k_CTX1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
            return true;
        default:
            return false;
    }
}

u32 TextureDecompressor::get_block_size(TextureFormat format) {
    // Block compressed formats use 4x4 pixel blocks
    switch (format) {
        case TextureFormat::k_DXT1:
        case TextureFormat::k_DXT1_AS_16_16_16_16:
        case TextureFormat::k_DXT3A:
        case TextureFormat::k_DXT5A:
        case TextureFormat::k_CTX1:
            return 8;  // 8 bytes per 4x4 block
            
        case TextureFormat::k_DXT2_3:
        case TextureFormat::k_DXT4_5:
        case TextureFormat::k_DXT2_3_AS_16_16_16_16:
        case TextureFormat::k_DXT4_5_AS_16_16_16_16:
            return 16;  // 16 bytes per 4x4 block
            
        default:
            return 0;  // Not block compressed
    }
}

u32 TextureDecompressor::get_bytes_per_block(TextureFormat format) {
    if (is_compressed(format)) {
        return get_block_size(format);
    }
    
    // Uncompressed formats - bytes per pixel
    switch (format) {
        case TextureFormat::k_1_REVERSE:
        case TextureFormat::k_1:
        case TextureFormat::k_8:
        case TextureFormat::k_8_A:
        case TextureFormat::k_8_B:
            return 1;
            
        case TextureFormat::k_1_5_5_5:
        case TextureFormat::k_5_6_5:
        case TextureFormat::k_6_5_5:
        case TextureFormat::k_8_8:
        case TextureFormat::k_4_4_4_4:
        case TextureFormat::k_16:
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
        case TextureFormat::k_16_16_EXPAND:
        case TextureFormat::k_16_16_FLOAT:
        case TextureFormat::k_32_FLOAT:
            return 4;
            
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

u32 TextureDecompressor::calculate_texture_size(u32 width, u32 height, u32 depth,
                                                 TextureFormat format, u32 mip_levels) {
    u32 total_size = 0;
    bool compressed = is_compressed(format);
    u32 bpp = get_bytes_per_block(format);
    
    for (u32 mip = 0; mip < mip_levels; mip++) {
        u32 mip_width = std::max(1u, width >> mip);
        u32 mip_height = std::max(1u, height >> mip);
        u32 mip_depth = std::max(1u, depth >> mip);
        
        if (compressed) {
            // Round up to 4x4 block size
            u32 blocks_x = (mip_width + 3) / 4;
            u32 blocks_y = (mip_height + 3) / 4;
            total_size += blocks_x * blocks_y * mip_depth * bpp;
        } else {
            total_size += mip_width * mip_height * mip_depth * bpp;
        }
    }
    
    return total_size;
}

//=============================================================================
// DXT/BC Decompression
//=============================================================================

void TextureDecompressor::color_565_to_rgba(u16 color, u8* rgba) {
    // Extract 5-6-5 components and expand to 8-bit
    u8 r = ((color >> 11) & 0x1F);
    u8 g = ((color >> 5) & 0x3F);
    u8 b = (color & 0x1F);
    
    // Expand to 8-bit by replicating high bits into low bits
    rgba[0] = (r << 3) | (r >> 2);
    rgba[1] = (g << 2) | (g >> 4);
    rgba[2] = (b << 3) | (b >> 2);
    rgba[3] = 255;
}

void TextureDecompressor::interpolate_colors(const u8* c0, const u8* c1, u8* c2, u8* c3, bool dxt1) {
    if (dxt1) {
        // DXT1: c2 = (2*c0 + c1) / 3, c3 = (c0 + 2*c1) / 3 OR c3 = transparent
        for (int i = 0; i < 3; i++) {
            c2[i] = (2 * c0[i] + c1[i] + 1) / 3;
            c3[i] = (c0[i] + 2 * c1[i] + 1) / 3;
        }
        c2[3] = 255;
        c3[3] = 255;
    } else {
        // DXT2-5: c2 = (2*c0 + c1) / 3, c3 = (c0 + 2*c1) / 3
        for (int i = 0; i < 4; i++) {
            c2[i] = (2 * c0[i] + c1[i] + 1) / 3;
            c3[i] = (c0[i] + 2 * c1[i] + 1) / 3;
        }
    }
}

void TextureDecompressor::decompress_dxt1_block(const u8* src, u8* dst, bool has_alpha) {
    // DXT1 block: 2 16-bit colors + 4x4 2-bit indices = 8 bytes
    u16 color0 = src[0] | (src[1] << 8);
    u16 color1 = src[2] | (src[3] << 8);
    u32 indices = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);
    
    // Decode colors
    u8 colors[4][4];
    color_565_to_rgba(color0, colors[0]);
    color_565_to_rgba(color1, colors[1]);
    
    if (color0 > color1 || !has_alpha) {
        // Opaque block: 4 color palette
        interpolate_colors(colors[0], colors[1], colors[2], colors[3], false);
    } else {
        // Transparent block: 3 colors + transparent
        for (int i = 0; i < 3; i++) {
            colors[2][i] = (colors[0][i] + colors[1][i]) / 2;
        }
        colors[2][3] = 255;
        colors[3][0] = colors[3][1] = colors[3][2] = 0;
        colors[3][3] = 0;  // Transparent
    }
    
    // Decode 4x4 block
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            u32 idx = (indices >> ((y * 4 + x) * 2)) & 0x3;
            u8* pixel = dst + (y * 4 + x) * 4;
            memcpy(pixel, colors[idx], 4);
        }
    }
}

void TextureDecompressor::decompress_dxt3_block(const u8* src, u8* dst) {
    // DXT3: 8 bytes explicit alpha + 8 bytes DXT1 color
    
    // Decode colors (same as DXT1, last 8 bytes)
    decompress_dxt1_block(src + 8, dst, false);
    
    // Override alpha with explicit 4-bit values
    for (int y = 0; y < 4; y++) {
        u16 alpha_row = src[y * 2] | (src[y * 2 + 1] << 8);
        for (int x = 0; x < 4; x++) {
            u8 alpha4 = (alpha_row >> (x * 4)) & 0xF;
            u8 alpha8 = (alpha4 << 4) | alpha4;  // Expand 4-bit to 8-bit
            dst[(y * 4 + x) * 4 + 3] = alpha8;
        }
    }
}

void TextureDecompressor::decompress_dxt5_block(const u8* src, u8* dst) {
    // DXT5: 8 bytes interpolated alpha + 8 bytes DXT1 color
    
    // Decode colors first
    decompress_dxt1_block(src + 8, dst, false);
    
    // Decode alpha
    u8 alpha0 = src[0];
    u8 alpha1 = src[1];
    
    // Build alpha palette
    u8 alphas[8];
    alphas[0] = alpha0;
    alphas[1] = alpha1;
    
    if (alpha0 > alpha1) {
        // 6 interpolated alphas
        for (int i = 0; i < 6; i++) {
            alphas[i + 2] = ((6 - i) * alpha0 + (i + 1) * alpha1 + 3) / 7;
        }
    } else {
        // 4 interpolated alphas + 0 and 255
        for (int i = 0; i < 4; i++) {
            alphas[i + 2] = ((4 - i) * alpha0 + (i + 1) * alpha1 + 2) / 5;
        }
        alphas[6] = 0;
        alphas[7] = 255;
    }
    
    // Decode 3-bit alpha indices (6 bytes for 16 pixels)
    u64 alpha_bits = 0;
    for (int i = 0; i < 6; i++) {
        alpha_bits |= static_cast<u64>(src[2 + i]) << (i * 8);
    }
    
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int pixel_idx = y * 4 + x;
            u8 alpha_idx = (alpha_bits >> (pixel_idx * 3)) & 0x7;
            dst[pixel_idx * 4 + 3] = alphas[alpha_idx];
        }
    }
}

void TextureDecompressor::decompress_dxt5a_block(const u8* src, u8* dst) {
    // DXT5A/BC4: Single channel using DXT5 alpha encoding
    u8 alpha0 = src[0];
    u8 alpha1 = src[1];
    
    // Build palette
    u8 alphas[8];
    alphas[0] = alpha0;
    alphas[1] = alpha1;
    
    if (alpha0 > alpha1) {
        for (int i = 0; i < 6; i++) {
            alphas[i + 2] = ((6 - i) * alpha0 + (i + 1) * alpha1 + 3) / 7;
        }
    } else {
        for (int i = 0; i < 4; i++) {
            alphas[i + 2] = ((4 - i) * alpha0 + (i + 1) * alpha1 + 2) / 5;
        }
        alphas[6] = 0;
        alphas[7] = 255;
    }
    
    // Decode indices
    u64 bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= static_cast<u64>(src[2 + i]) << (i * 8);
    }
    
    for (int i = 0; i < 16; i++) {
        u8 idx = (bits >> (i * 3)) & 0x7;
        dst[i] = alphas[idx];
    }
}

void TextureDecompressor::decompress_dxn_block(const u8* src, u8* dst) {
    // DXN/BC5: Two DXT5A channels (R and G) - 16 bytes total
    // First 8 bytes: Red channel (DXT5A encoding)
    // Last 8 bytes: Green channel (DXT5A encoding)
    u8 red_block[16];
    u8 green_block[16];
    decompress_dxt5a_block(src, red_block);
    decompress_dxt5a_block(src + 8, green_block);

    // Combine into RG output (2 bytes per pixel, 4x4 block)
    for (int i = 0; i < 16; i++) {
        dst[i * 2 + 0] = red_block[i];
        dst[i * 2 + 1] = green_block[i];
    }
}

void TextureDecompressor::decompress_ctx1_block(const u8* src, u8* dst) {
    // CTX1: Xbox 360 specific 2-channel normal map format
    // 8 bytes: 2 endpoint colors (2 bytes each) + 4 bytes indices
    
    u8 x0 = src[0];
    u8 y0 = src[1];
    u8 x1 = src[2];
    u8 y1 = src[3];
    u32 indices = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);
    
    // Build color palette for X and Y channels
    u8 x_palette[4], y_palette[4];
    x_palette[0] = x0;
    x_palette[1] = x1;
    y_palette[0] = y0;
    y_palette[1] = y1;
    
    // Interpolate
    x_palette[2] = (2 * x0 + x1 + 1) / 3;
    x_palette[3] = (x0 + 2 * x1 + 1) / 3;
    y_palette[2] = (2 * y0 + y1 + 1) / 3;
    y_palette[3] = (y0 + 2 * y1 + 1) / 3;
    
    // Decode 4x4 block to RG format (Z can be derived from XY for normals)
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            u32 idx = (indices >> ((y * 4 + x) * 2)) & 0x3;
            u8* pixel = dst + (y * 4 + x) * 2;
            pixel[0] = x_palette[idx];  // R = X
            pixel[1] = y_palette[idx];  // G = Y
        }
    }
}

void TextureDecompressor::decompress_texture(const u8* src, u8* dst,
                                              u32 width, u32 height,
                                              TextureFormat format) {
    u32 blocks_x = (width + 3) / 4;
    u32 blocks_y = (height + 3) / 4;
    u32 block_size = get_block_size(format);
    
    // Temporary buffer for one decompressed block
    u8 block[64];  // 4x4 RGBA = 64 bytes
    
    for (u32 by = 0; by < blocks_y; by++) {
        for (u32 bx = 0; bx < blocks_x; bx++) {
            u32 src_offset = (by * blocks_x + bx) * block_size;
            
            // Decompress block
            switch (format) {
                case TextureFormat::k_DXT1:
                case TextureFormat::k_DXT1_AS_16_16_16_16:
                    decompress_dxt1_block(src + src_offset, block, true);
                    break;
                    
                case TextureFormat::k_DXT2_3:
                case TextureFormat::k_DXT2_3_AS_16_16_16_16:
                    decompress_dxt3_block(src + src_offset, block);
                    break;
                    
                case TextureFormat::k_DXT4_5:
                case TextureFormat::k_DXT4_5_AS_16_16_16_16:
                    decompress_dxt5_block(src + src_offset, block);
                    break;
                    
                case TextureFormat::k_DXT5A:
                    decompress_dxt5a_block(src + src_offset, block);
                    break;

                case TextureFormat::k_DXT3A: {
                    // DXT3A: 4-bit alpha only, 8 bytes per block
                    for (int py = 0; py < 4; py++) {
                        u16 row = src[src_offset + py * 2] | (src[src_offset + py * 2 + 1] << 8);
                        for (int px = 0; px < 4; px++) {
                            u8 a4 = (row >> (px * 4)) & 0xF;
                            block[py * 4 + px] = (a4 << 4) | a4;
                        }
                    }
                    break;
                }

                case TextureFormat::k_CTX1:
                    decompress_ctx1_block(src + src_offset, block);
                    break;

                case TextureFormat::k_DXN: {
                    // DXN/BC5: two-channel, decode to RG then expand to RGBA
                    u8 rg_block[32];  // 16 pixels * 2 channels
                    decompress_dxn_block(src + src_offset, rg_block);
                    for (int i = 0; i < 16; i++) {
                        block[i * 4 + 0] = rg_block[i * 2 + 0];
                        block[i * 4 + 1] = rg_block[i * 2 + 1];
                        block[i * 4 + 2] = 0;
                        block[i * 4 + 3] = 255;
                    }
                    break;
                }

                default:
                    memset(block, 128, sizeof(block));
                    break;
            }

            // Copy decompressed block to output
            // DXT5A and DXT3A produce single-channel output; expand to RGBA
            bool single_channel = (format == TextureFormat::k_DXT5A ||
                                   format == TextureFormat::k_DXT3A);
            for (u32 py = 0; py < 4 && (by * 4 + py) < height; py++) {
                for (u32 px = 0; px < 4 && (bx * 4 + px) < width; px++) {
                    u32 dst_offset = ((by * 4 + py) * width + (bx * 4 + px)) * 4;
                    if (single_channel) {
                        u8 v = block[py * 4 + px];
                        dst[dst_offset + 0] = v;
                        dst[dst_offset + 1] = v;
                        dst[dst_offset + 2] = v;
                        dst[dst_offset + 3] = 255;
                    } else {
                        memcpy(dst + dst_offset, block + (py * 4 + px) * 4, 4);
                    }
                }
            }
        }
    }
}

//=============================================================================
// Format Conversion
//=============================================================================

f32 TextureFormatConverter::half_to_float(u16 h) {
    // IEEE 754 half-precision to single-precision
    u32 sign = (h >> 15) & 1;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    
    u32 f;
    if (exp == 0) {
        if (mant == 0) {
            // Zero
            f = sign << 31;
        } else {
            // Denormal
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            exp++;
            mant &= ~0x400;
            f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        // Inf or NaN
        f = (sign << 31) | 0x7F800000 | (mant << 13);
    } else {
        // Normal
        f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    
    return *reinterpret_cast<f32*>(&f);
}

void TextureFormatConverter::convert_565_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    for (u32 i = 0; i < pixel_count; i++) {
        u16 pixel = src[i * 2] | (src[i * 2 + 1] << 8);
        u8 r = ((pixel >> 11) & 0x1F);
        u8 g = ((pixel >> 5) & 0x3F);
        u8 b = (pixel & 0x1F);
        
        dst[i * 4 + 0] = (r << 3) | (r >> 2);
        dst[i * 4 + 1] = (g << 2) | (g >> 4);
        dst[i * 4 + 2] = (b << 3) | (b >> 2);
        dst[i * 4 + 3] = 255;
    }
}

void TextureFormatConverter::convert_1555_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    for (u32 i = 0; i < pixel_count; i++) {
        u16 pixel = src[i * 2] | (src[i * 2 + 1] << 8);
        u8 a = (pixel >> 15) ? 255 : 0;
        u8 r = ((pixel >> 10) & 0x1F);
        u8 g = ((pixel >> 5) & 0x1F);
        u8 b = (pixel & 0x1F);
        
        dst[i * 4 + 0] = (r << 3) | (r >> 2);
        dst[i * 4 + 1] = (g << 3) | (g >> 2);
        dst[i * 4 + 2] = (b << 3) | (b >> 2);
        dst[i * 4 + 3] = a;
    }
}

void TextureFormatConverter::convert_4444_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    for (u32 i = 0; i < pixel_count; i++) {
        u16 pixel = src[i * 2] | (src[i * 2 + 1] << 8);
        u8 a = (pixel >> 12) & 0xF;
        u8 r = (pixel >> 8) & 0xF;
        u8 g = (pixel >> 4) & 0xF;
        u8 b = pixel & 0xF;
        
        dst[i * 4 + 0] = (r << 4) | r;
        dst[i * 4 + 1] = (g << 4) | g;
        dst[i * 4 + 2] = (b << 4) | b;
        dst[i * 4 + 3] = (a << 4) | a;
    }
}

void TextureFormatConverter::convert_2101010_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    for (u32 i = 0; i < pixel_count; i++) {
        u32 pixel = *reinterpret_cast<const u32*>(src + i * 4);
        u32 a = (pixel >> 30) & 0x3;
        u32 b = (pixel >> 20) & 0x3FF;
        u32 g = (pixel >> 10) & 0x3FF;
        u32 r = pixel & 0x3FF;
        
        dst[i * 4 + 0] = static_cast<u8>(r >> 2);
        dst[i * 4 + 1] = static_cast<u8>(g >> 2);
        dst[i * 4 + 2] = static_cast<u8>(b >> 2);
        dst[i * 4 + 3] = static_cast<u8>(a * 85);  // 0,1,2,3 -> 0,85,170,255
    }
}

void TextureFormatConverter::convert_rg16f_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    for (u32 i = 0; i < pixel_count; i++) {
        const u16* fp16 = reinterpret_cast<const u16*>(src + i * 4);
        f32 r = half_to_float(fp16[0]);
        f32 g = half_to_float(fp16[1]);
        
        // Clamp and convert to 8-bit
        dst[i * 4 + 0] = static_cast<u8>(std::clamp(r * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 1] = static_cast<u8>(std::clamp(g * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 2] = 0;
        dst[i * 4 + 3] = 255;
    }
}

void TextureFormatConverter::convert_rgba16f_to_rgba8(const u8* src, u8* dst, u32 pixel_count) {
    for (u32 i = 0; i < pixel_count; i++) {
        const u16* fp16 = reinterpret_cast<const u16*>(src + i * 8);
        f32 r = half_to_float(fp16[0]);
        f32 g = half_to_float(fp16[1]);
        f32 b = half_to_float(fp16[2]);
        f32 a = half_to_float(fp16[3]);
        
        dst[i * 4 + 0] = static_cast<u8>(std::clamp(r * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 1] = static_cast<u8>(std::clamp(g * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 2] = static_cast<u8>(std::clamp(b * 255.0f, 0.0f, 255.0f));
        dst[i * 4 + 3] = static_cast<u8>(std::clamp(a * 255.0f, 0.0f, 255.0f));
    }
}

void TextureFormatConverter::convert_to_rgba8(const u8* src, u8* dst,
                                               u32 width, u32 height,
                                               TextureFormat format) {
    u32 pixel_count = width * height;
    
    switch (format) {
        case TextureFormat::k_8:
        case TextureFormat::k_8_A:
        case TextureFormat::k_8_B:
            // Single channel - replicate to all RGB, set alpha to 255
            for (u32 i = 0; i < pixel_count; i++) {
                dst[i * 4 + 0] = src[i];
                dst[i * 4 + 1] = src[i];
                dst[i * 4 + 2] = src[i];
                dst[i * 4 + 3] = 255;
            }
            break;
            
        case TextureFormat::k_8_8:
            // RG - set B to 0, A to 255
            for (u32 i = 0; i < pixel_count; i++) {
                dst[i * 4 + 0] = src[i * 2 + 0];
                dst[i * 4 + 1] = src[i * 2 + 1];
                dst[i * 4 + 2] = 0;
                dst[i * 4 + 3] = 255;
            }
            break;
            
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_8_8_8_8_A:
        case TextureFormat::k_8_8_8_8_GAMMA:
            // Direct copy
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
            
        case TextureFormat::k_32_FLOAT:
            // Single float channel
            for (u32 i = 0; i < pixel_count; i++) {
                f32 val = *reinterpret_cast<const f32*>(src + i * 4);
                u8 v8 = static_cast<u8>(std::clamp(val * 255.0f, 0.0f, 255.0f));
                dst[i * 4 + 0] = v8;
                dst[i * 4 + 1] = v8;
                dst[i * 4 + 2] = v8;
                dst[i * 4 + 3] = 255;
            }
            break;
            
        default:
            // Unknown format - fill with magenta for debugging
            for (u32 i = 0; i < pixel_count; i++) {
                dst[i * 4 + 0] = 255;
                dst[i * 4 + 1] = 0;
                dst[i * 4 + 2] = 255;
                dst[i * 4 + 3] = 255;
            }
            LOGE("Unknown texture format: %d", static_cast<int>(format));
            break;
    }
}

void TextureFormatConverter::byte_swap_16(u8* data, u32 size) {
    u16* data16 = reinterpret_cast<u16*>(data);
    u32 count = size / 2;
    for (u32 i = 0; i < count; i++) {
        data16[i] = __builtin_bswap16(data16[i]);
    }
}

void TextureFormatConverter::byte_swap_32(u8* data, u32 size) {
    u32* data32 = reinterpret_cast<u32*>(data);
    u32 count = size / 4;
    for (u32 i = 0; i < count; i++) {
        data32[i] = __builtin_bswap32(data32[i]);
    }
}

void TextureFormatConverter::apply_swizzle(u8* data, u32 pixel_count,
                                            u8 swizzle_r, u8 swizzle_g,
                                            u8 swizzle_b, u8 swizzle_a) {
    // Swizzle values: 0=R, 1=G, 2=B, 3=A, 4=0, 5=1
    auto get_component = [](const u8* pixel, u8 swizzle) -> u8 {
        switch (swizzle) {
            case 0: return pixel[0];  // R
            case 1: return pixel[1];  // G
            case 2: return pixel[2];  // B
            case 3: return pixel[3];  // A
            case 4: return 0;         // Zero
            case 5: return 255;       // One
            default: return pixel[swizzle & 3];
        }
    };
    
    for (u32 i = 0; i < pixel_count; i++) {
        u8* pixel = data + i * 4;
        u8 orig[4];
        memcpy(orig, pixel, 4);
        
        pixel[0] = get_component(orig, swizzle_r);
        pixel[1] = get_component(orig, swizzle_g);
        pixel[2] = get_component(orig, swizzle_b);
        pixel[3] = get_component(orig, swizzle_a);
    }
}

//=============================================================================
// TextureCache Implementation
//=============================================================================

TextureCache::TextureCache() 
    : access_counter_(0), max_size_(0), current_size_(0), stats_{} {
}

TextureCache::~TextureCache() {
    shutdown();
}

Status TextureCache::initialize(u32 max_size_mb) {
    max_size_ = max_size_mb * 1024 * 1024;
    current_size_ = 0;
    access_counter_ = 0;
    stats_ = {};
    
    LOGI("Texture cache initialized: %u MB max", max_size_mb);
    return Status::Ok;
}

void TextureCache::shutdown() {
    entries_.clear();
    current_size_ = 0;
}

u64 TextureCache::calculate_hash(const TextureInfo& info, const u8* data, u32 size) {
    // FNV-1a hash combining info fields and texture data
    u64 hash = 14695981039346656037ULL;
    
    // Hash texture info
    auto hash_bytes = [&hash](const void* ptr, size_t len) {
        const u8* bytes = static_cast<const u8*>(ptr);
        for (size_t i = 0; i < len; i++) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    };
    
    hash_bytes(&info.base_address, sizeof(info.base_address));
    hash_bytes(&info.width, sizeof(info.width));
    hash_bytes(&info.height, sizeof(info.height));
    hash_bytes(&info.format, sizeof(info.format));
    hash_bytes(&info.tiled, sizeof(info.tiled));
    
    // Hash texture data (sample for large textures)
    if (size > 0 && data) {
        if (size <= 4096) {
            hash_bytes(data, size);
        } else {
            // Sample every 64 bytes for large textures
            for (u32 i = 0; i < size; i += 64) {
                hash ^= data[i];
                hash *= 1099511628211ULL;
            }
        }
    }
    
    return hash;
}

const u8* TextureCache::get_texture(const TextureInfo& info, Memory* memory) {
    if (!memory) return nullptr;
    
    // Get raw texture data
    const u8* raw_data = static_cast<const u8*>(memory->get_host_ptr(info.base_address));
    if (!raw_data) return nullptr;
    
    // Calculate hash
    u32 raw_size = TextureDecompressor::calculate_texture_size(
        info.width, info.height, info.depth > 0 ? info.depth : 1, 
        info.format, info.mip_levels > 0 ? info.mip_levels : 1);
    u64 hash = calculate_hash(info, raw_data, raw_size);
    
    // Check cache
    auto it = entries_.find(hash);
    if (it != entries_.end()) {
        it->second->last_access = ++access_counter_;
        stats_.hits++;
        return it->second->data.data();
    }
    
    // Cache miss - decode texture
    stats_.misses++;
    
    // Evict if necessary
    while (current_size_ > max_size_ && !entries_.empty()) {
        evict_oldest();
    }
    
    // Create new entry
    auto entry = std::make_unique<CacheEntry>();
    entry->info = info;
    entry->hash = hash;
    entry->last_access = ++access_counter_;
    
    // Allocate decoded data (always RGBA8)
    u32 decoded_size = info.width * info.height * 4;
    entry->data.resize(decoded_size);
    
    // Decode texture
    decode_texture(info, raw_data, entry->data.data(), memory);
    
    const u8* result = entry->data.data();
    current_size_ += decoded_size;
    stats_.entry_count++;
    stats_.memory_used = current_size_;
    stats_.uploads++;
    
    entries_[hash] = std::move(entry);
    
    return result;
}

void TextureCache::decode_texture(const TextureInfo& info, const u8* src, u8* dst, Memory* memory) {
    (void)memory;  // May be used for mipmap fetching later
    
    u32 width = info.width;
    u32 height = info.height;
    
    // Handle tiled textures
    std::vector<u8> untiled;
    const u8* tex_data = src;
    
    if (info.tiled) {
        u32 bpp = TextureDecompressor::get_bytes_per_block(info.format);
        bool compressed = TextureDecompressor::is_compressed(info.format);
        
        if (compressed) {
            // Untile block-compressed texture
            u32 blocks_x = (width + 3) / 4;
            u32 blocks_y = (height + 3) / 4;
            u32 size = blocks_x * blocks_y * bpp;
            untiled.resize(size);
            TextureUntiler::untile_2d(src, untiled.data(), blocks_x, blocks_y, bpp, 1, 1);
        } else {
            // Untile uncompressed texture
            u32 size = width * height * bpp;
            untiled.resize(size);
            TextureUntiler::untile_2d(src, untiled.data(), width, height, bpp, 1, 1);
        }
        tex_data = untiled.data();
    }
    
    // Decompress or convert format
    if (TextureDecompressor::is_compressed(info.format)) {
        TextureDecompressor::decompress_texture(tex_data, dst, width, height, info.format);
    } else {
        TextureFormatConverter::convert_to_rgba8(tex_data, dst, width, height, info.format);
    }
    
    // Apply swizzle if non-default
    if (info.swizzle_x != 0 || info.swizzle_y != 1 || 
        info.swizzle_z != 2 || info.swizzle_w != 3) {
        TextureFormatConverter::apply_swizzle(dst, width * height,
            info.swizzle_x, info.swizzle_y, info.swizzle_z, info.swizzle_w);
    }
}

void TextureCache::invalidate_range(GuestAddr address, u32 size) {
    // Remove entries that overlap with the invalidated range
    auto it = entries_.begin();
    while (it != entries_.end()) {
        const auto& info = it->second->info;
        GuestAddr tex_end = info.base_address + TextureDecompressor::calculate_texture_size(
            info.width, info.height, info.depth > 0 ? info.depth : 1,
            info.format, info.mip_levels > 0 ? info.mip_levels : 1);
        
        if (info.base_address < address + size && tex_end > address) {
            current_size_ -= it->second->data.size();
            it = entries_.erase(it);
            stats_.entry_count--;
        } else {
            ++it;
        }
    }
    
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
    
    // Find oldest entry
    auto oldest_it = entries_.begin();
    u64 oldest_access = oldest_it->second->last_access;
    
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second->last_access < oldest_access) {
            oldest_access = it->second->last_access;
            oldest_it = it;
        }
    }
    
    // Remove it
    current_size_ -= oldest_it->second->data.size();
    entries_.erase(oldest_it);
    stats_.entry_count--;
    stats_.memory_used = current_size_;
}

TextureCache::Stats TextureCache::get_stats() const {
    return stats_;
}

} // namespace x360mu
