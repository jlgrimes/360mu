/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * Texture Handling
 * 
 * Xbox 360 texture format handling including:
 * - DXT/BC compressed textures
 * - Various uncompressed formats
 * - Texture tiling/untiling
 * - Mipmap handling
 */

#pragma once

#include "x360mu/types.h"
#include <vector>
#include <memory>
#include <unordered_map>

namespace x360mu {

/**
 * Xbox 360 Texture Formats
 * Xenos supports a wide variety of texture formats
 */
enum class TextureFormat : u8 {
    // Uncompressed formats
    k_1_REVERSE = 0,
    k_1 = 1,
    k_8 = 2,
    k_1_5_5_5 = 3,
    k_5_6_5 = 4,
    k_6_5_5 = 5,
    k_8_8_8_8 = 6,
    k_2_10_10_10 = 7,
    k_8_A = 8,
    k_8_B = 9,
    k_8_8 = 10,
    k_Cr_Y1_Cb_Y0 = 11,  // YUV
    k_Y1_Cr_Y0_Cb = 12,  // YUV
    k_Shadow = 13,
    k_8_8_8_8_A = 14,
    k_4_4_4_4 = 15,
    k_10_11_11 = 16,
    k_11_11_10 = 17,
    k_DXT1 = 18,          // BC1
    k_DXT2_3 = 19,        // BC2
    k_DXT4_5 = 20,        // BC3
    k_16_16_EXPAND = 21,
    k_16_16 = 22,
    k_16_16_16_16_EXPAND = 23,
    k_16_16_16_16 = 24,
    k_16_FLOAT = 25,
    k_16_16_FLOAT = 26,
    k_16_16_16_16_FLOAT = 27,
    k_32_FLOAT = 28,
    k_32_32_FLOAT = 29,
    k_32_32_32_32_FLOAT = 30,
    k_8_8_8_8_AS_16_16_16_16 = 31,
    k_DXT1_AS_16_16_16_16 = 32,
    k_DXT2_3_AS_16_16_16_16 = 33,
    k_DXT4_5_AS_16_16_16_16 = 34,
    k_2_10_10_10_AS_16_16_16_16 = 35,
    k_10_11_11_AS_16_16_16_16 = 36,
    k_11_11_10_AS_16_16_16_16 = 37,
    k_32_32_32_FLOAT = 38,
    k_DXT3A = 39,
    k_DXT5A = 40,         // BC4
    k_CTX1 = 41,          // Xbox 360 specific
    k_DXN = 42,           // BC5 - two-channel normal map compression
    k_DXT3A_AS_1_1_1_1 = 43,
    k_8_8_8_8_GAMMA = 44,
    k_2_10_10_10_GAMMA = 45,
    k_16 = 46,  // 16-bit single channel
};

/**
 * Texture dimension type
 */
enum class TextureDimension : u8 {
    k1D = 0,
    k2D = 1,
    k3D = 2,
    kCube = 3,
};

/**
 * Texture addressing mode (for sampling)
 */
enum class TextureAddressMode : u8 {
    kWrap = 0,
    kMirror = 1,
    kClampToHalf = 2,
    kMirrorOnceToHalf = 3,
    kClampToBorder = 4,
    kMirrorOnceToBorder = 5,
    kClampToEdge = 6,
    kMirrorOnceToEdge = 7,
};

/**
 * Texture filter mode
 */
enum class TextureFilter : u8 {
    kPoint = 0,
    kLinear = 1,
    kBaseMap = 2,  // Use base texture for minification
};

/**
 * Texture descriptor from guest memory
 */
struct TextureInfo {
    GuestAddr base_address;
    GuestAddr mip_address;
    TextureFormat format;
    TextureDimension dimension;
    
    u32 width;
    u32 height;
    u32 depth;         // For 3D textures
    u32 pitch;
    
    u32 mip_levels;
    bool tiled;
    bool packed_mips;
    
    // Swizzle info
    u8 swizzle_x;      // Component for R
    u8 swizzle_y;      // Component for G
    u8 swizzle_z;      // Component for B
    u8 swizzle_w;      // Component for A
    
    // Border color for clamp modes
    f32 border_color[4];
};

/**
 * Sampler state
 */
struct SamplerState {
    TextureAddressMode address_u;
    TextureAddressMode address_v;
    TextureAddressMode address_w;
    
    TextureFilter min_filter;
    TextureFilter mag_filter;
    TextureFilter mip_filter;
    
    f32 mip_lod_bias;
    f32 max_anisotropy;
    f32 min_lod;
    f32 max_lod;
    
    f32 border_color[4];
};

/**
 * DXT/BC Texture Decompressor
 * 
 * Handles decompression of block-compressed textures:
 * - DXT1/BC1: RGB with 1-bit alpha
 * - DXT2/3/BC2: RGB with explicit alpha
 * - DXT4/5/BC3: RGB with interpolated alpha
 * - DXT5A/BC4: Single channel
 * - CTX1: Xbox 360 specific 2-channel format
 */
class TextureDecompressor {
public:
    /**
     * Decompress DXT1 (BC1) block
     * 4x4 pixels, 8 bytes -> 64 bytes RGBA
     */
    static void decompress_dxt1_block(const u8* src, u8* dst, bool has_alpha = false);
    
    /**
     * Decompress DXT3 (BC2) block
     * 4x4 pixels, 16 bytes -> 64 bytes RGBA
     */
    static void decompress_dxt3_block(const u8* src, u8* dst);
    
    /**
     * Decompress DXT5 (BC3) block
     * 4x4 pixels, 16 bytes -> 64 bytes RGBA
     */
    static void decompress_dxt5_block(const u8* src, u8* dst);
    
    /**
     * Decompress DXT5A (BC4) block
     * 4x4 pixels, 8 bytes -> 16 bytes single channel
     */
    static void decompress_dxt5a_block(const u8* src, u8* dst);
    
    /**
     * Decompress CTX1 block (Xbox 360 specific)
     * 2-channel normal map compression
     */
    static void decompress_ctx1_block(const u8* src, u8* dst);

    /**
     * Decompress DXN/BC5 block (two-channel normal map)
     * 4x4 pixels, 16 bytes -> 32 bytes RG
     */
    static void decompress_dxn_block(const u8* src, u8* dst);

    /**
     * Decompress an entire texture
     */
    static void decompress_texture(const u8* src, u8* dst,
                                   u32 width, u32 height,
                                   TextureFormat format);
    
    /**
     * Get block size for a compressed format
     */
    static u32 get_block_size(TextureFormat format);
    
    /**
     * Check if format is block compressed
     */
    static bool is_compressed(TextureFormat format);
    
    /**
     * Get bytes per pixel/block for a format
     */
    static u32 get_bytes_per_block(TextureFormat format);
    
    /**
     * Calculate texture size in bytes
     */
    static u32 calculate_texture_size(u32 width, u32 height, u32 depth,
                                      TextureFormat format, u32 mip_levels);
    
private:
    // Helper: expand 565 color to RGBA
    static void color_565_to_rgba(u16 color, u8* rgba);
    
    // Helper: interpolate colors
    static void interpolate_colors(const u8* c0, const u8* c1, u8* c2, u8* c3, bool dxt1);
};

/**
 * Texture Cache
 * 
 * Manages decoded/decompressed textures and tracks which
 * textures need to be re-uploaded to the GPU.
 */
class TextureCache {
public:
    TextureCache();
    ~TextureCache();
    
    /**
     * Initialize the texture cache
     */
    Status initialize(u32 max_size_mb = 256);
    
    /**
     * Shutdown and free all resources
     */
    void shutdown();
    
    /**
     * Get or create a decoded texture
     * Returns pointer to RGBA data
     */
    const u8* get_texture(const TextureInfo& info, class Memory* memory);
    
    /**
     * Invalidate textures in address range
     * Called when guest writes to texture memory
     */
    void invalidate_range(GuestAddr address, u32 size);
    
    /**
     * Invalidate all textures
     */
    void invalidate_all();
    
    /**
     * Get cache statistics
     */
    struct Stats {
        u64 hits;
        u64 misses;
        u64 uploads;
        u32 entry_count;
        u64 memory_used;
    };
    Stats get_stats() const;
    
private:
    struct CacheEntry {
        TextureInfo info;
        std::vector<u8> data;       // Decoded RGBA data
        u64 last_access;
        u64 hash;
    };
    
    std::unordered_map<u64, std::unique_ptr<CacheEntry>> entries_;
    u64 access_counter_;
    u64 max_size_;
    u64 current_size_;
    
    Stats stats_;
    
    u64 calculate_hash(const TextureInfo& info, const u8* data, u32 size);
    void evict_oldest();
    void decode_texture(const TextureInfo& info, const u8* src, u8* dst, class Memory* memory);
};

/**
 * Format conversion utilities
 */
class TextureFormatConverter {
public:
    /**
     * Convert from Xbox format to RGBA8
     */
    static void convert_to_rgba8(const u8* src, u8* dst,
                                 u32 width, u32 height,
                                 TextureFormat format);
    
    /**
     * Convert 5_6_5 to RGBA8
     */
    static void convert_565_to_rgba8(const u8* src, u8* dst, u32 pixel_count);
    
    /**
     * Convert 1_5_5_5 to RGBA8
     */
    static void convert_1555_to_rgba8(const u8* src, u8* dst, u32 pixel_count);
    
    /**
     * Convert 4_4_4_4 to RGBA8
     */
    static void convert_4444_to_rgba8(const u8* src, u8* dst, u32 pixel_count);
    
    /**
     * Convert 2_10_10_10 to RGBA8
     */
    static void convert_2101010_to_rgba8(const u8* src, u8* dst, u32 pixel_count);
    
    /**
     * Convert 16_16_FLOAT to RGBA8
     */
    static void convert_rg16f_to_rgba8(const u8* src, u8* dst, u32 pixel_count);
    
    /**
     * Convert 16_16_16_16_FLOAT to RGBA8
     */
    static void convert_rgba16f_to_rgba8(const u8* src, u8* dst, u32 pixel_count);
    
    /**
     * Byte swap for big-endian textures
     */
    static void byte_swap_16(u8* data, u32 size);
    static void byte_swap_32(u8* data, u32 size);
    
    /**
     * Apply swizzle to texture
     */
    static void apply_swizzle(u8* data, u32 pixel_count,
                              u8 swizzle_r, u8 swizzle_g,
                              u8 swizzle_b, u8 swizzle_a);
    
private:
    // Half-float to float conversion
    static f32 half_to_float(u16 h);
};

} // namespace x360mu

