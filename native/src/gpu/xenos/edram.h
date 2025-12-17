/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * eDRAM (Embedded DRAM) Emulation
 * 
 * The Xbox 360's Xenos GPU has 10MB of embedded DRAM used exclusively for
 * render targets and depth buffers. This provides extremely high bandwidth
 * for tile-based rendering operations.
 * 
 * Key features:
 * - 10MB total capacity
 * - 256GB/s bandwidth (internal)
 * - Used for color and depth render targets
 * - Supports MSAA resolve
 * - Tile-based rendering with resolve to main memory
 */

#pragma once

#include "x360mu/types.h"
#include <vector>
#include <array>
#include <memory>
#include <mutex>

namespace x360mu {

// eDRAM constants
constexpr u32 EDRAM_SIZE = 10 * 1024 * 1024;  // 10MB
constexpr u32 EDRAM_TILE_WIDTH = 80;           // Pixels per tile horizontally
constexpr u32 EDRAM_TILE_HEIGHT = 16;          // Pixels per tile vertically
constexpr u32 EDRAM_TILE_SIZE = EDRAM_TILE_WIDTH * EDRAM_TILE_HEIGHT;

// Maximum render target dimensions supported
constexpr u32 MAX_RT_WIDTH = 2560;
constexpr u32 MAX_RT_HEIGHT = 2560;

/**
 * Surface format for eDRAM render targets
 */
enum class EdramSurfaceFormat : u8 {
    k_8 = 0,
    k_1_5_5_5 = 1,
    k_5_6_5 = 2,
    k_6_5_5 = 3,
    k_8_8_8_8 = 4,
    k_2_10_10_10 = 5,
    k_8_A = 6,
    k_8_B = 7,
    k_8_8 = 8,
    k_8_8_8_8_A = 10,
    k_4_4_4_4 = 15,
    k_10_11_11 = 16,
    k_11_11_10 = 17,
    k_16 = 24,
    k_16_16 = 25,
    k_16_16_16_16 = 26,
    k_16_EXPAND = 27,
    k_16_16_EXPAND = 28,
    k_16_16_16_16_EXPAND = 29,
    k_16_FLOAT = 30,
    k_16_16_FLOAT = 31,
    k_16_16_16_16_FLOAT = 32,
    k_32_FLOAT = 36,
    k_32_32_FLOAT = 37,
    k_32_32_32_32_FLOAT = 38,
    k_8_8_8_8_AS_16_16_16_16 = 50,
    k_2_10_10_10_AS_16_16_16_16 = 54,
    k_10_11_11_AS_16_16_16_16 = 55,
    k_11_11_10_AS_16_16_16_16 = 56,
};

/**
 * Depth format for eDRAM depth buffers
 */
enum class EdramDepthFormat : u8 {
    kD24S8 = 0,
    kD24FS8 = 1,  // 24-bit float depth + 8-bit stencil
};

/**
 * MSAA mode
 */
enum class EdramMsaaMode : u8 {
    k1X = 0,
    k2X = 1,
    k4X = 2,
};

/**
 * eDRAM tile info
 */
struct EdramTileInfo {
    u32 base_offset;      // Offset in eDRAM (in tiles)
    u32 pitch;            // Pitch in tiles
    u32 width;            // Width in pixels
    u32 height;           // Height in pixels
    EdramSurfaceFormat format;
    EdramMsaaMode msaa;
    bool is_depth;
};

/**
 * Render target configuration
 */
struct RenderTargetConfig {
    bool enabled;
    u32 edram_base;           // Base offset in eDRAM
    u32 edram_pitch;          // Pitch in eDRAM tiles
    EdramSurfaceFormat format;
    EdramMsaaMode msaa;
    
    // For resolve to main memory
    GuestAddr resolve_address;
    u32 resolve_pitch;
    u32 resolve_width;
    u32 resolve_height;
};

/**
 * Depth stencil configuration
 */
struct DepthStencilConfig {
    bool enabled;
    u32 edram_base;
    u32 edram_pitch;
    EdramDepthFormat format;
    EdramMsaaMode msaa;
};

/**
 * eDRAM Manager
 * 
 * Manages the Xbox 360's embedded DRAM and handles:
 * - Tile allocation and tracking
 * - Format conversion
 * - MSAA resolve
 * - Copy to/from main memory
 */
class EdramManager {
public:
    EdramManager();
    ~EdramManager();
    
    /**
     * Initialize eDRAM
     */
    Status initialize();
    
    /**
     * Shutdown
     */
    void shutdown();
    
    /**
     * Reset eDRAM state
     */
    void reset();
    
    /**
     * Set render target configuration
     */
    void set_render_target(u32 index, const RenderTargetConfig& config);
    
    /**
     * Set depth stencil configuration
     */
    void set_depth_stencil(const DepthStencilConfig& config);
    
    /**
     * Clear render target
     */
    void clear_render_target(u32 index, f32 r, f32 g, f32 b, f32 a);
    
    /**
     * Clear depth stencil
     */
    void clear_depth_stencil(f32 depth, u8 stencil);
    
    /**
     * Resolve render target to main memory
     * This converts from eDRAM tiled format to linear format in main memory
     */
    void resolve_render_target(u32 index, class Memory* memory);
    
    /**
     * Resolve depth buffer to main memory
     */
    void resolve_depth_stencil(class Memory* memory);
    
    /**
     * Copy from main memory to eDRAM (for render-to-texture)
     */
    void copy_to_edram(u32 edram_offset, GuestAddr src_address, 
                       u32 width, u32 height, EdramSurfaceFormat format,
                       class Memory* memory);
    
    /**
     * Get raw eDRAM data pointer (for Vulkan upload)
     */
    const u8* get_data() const { return data_.data(); }
    u8* get_data() { return data_.data(); }
    
    /**
     * Get render target info
     */
    const RenderTargetConfig& get_render_target(u32 index) const;
    
    /**
     * Get depth stencil info
     */
    const DepthStencilConfig& get_depth_stencil() const { return depth_stencil_; }
    
    /**
     * Calculate bytes per pixel for format
     */
    static u32 get_bytes_per_pixel(EdramSurfaceFormat format);
    
    /**
     * Calculate tile offset
     */
    static u32 calculate_tile_offset(u32 x, u32 y, u32 pitch, EdramMsaaMode msaa);
    
private:
    // eDRAM data
    std::vector<u8> data_;
    
    // Render target configs (up to 4 MRTs)
    std::array<RenderTargetConfig, 4> render_targets_;
    
    // Depth stencil config
    DepthStencilConfig depth_stencil_;
    
    // Mutex for thread safety
    std::mutex mutex_;
    
    // Helper methods
    void untile_surface(const u8* src, u8* dst, 
                        u32 width, u32 height, u32 bpp,
                        u32 src_pitch, u32 dst_pitch);
    
    void tile_surface(const u8* src, u8* dst,
                      u32 width, u32 height, u32 bpp,
                      u32 src_pitch, u32 dst_pitch);
    
    void resolve_msaa_2x(const u8* src, u8* dst,
                         u32 width, u32 height, u32 bpp);
    
    void resolve_msaa_4x(const u8* src, u8* dst,
                         u32 width, u32 height, u32 bpp);
    
    void convert_format(const u8* src, u8* dst,
                        u32 pixel_count,
                        EdramSurfaceFormat src_format,
                        EdramSurfaceFormat dst_format);
};

/**
 * Texture Tile Modes
 * Xbox 360 textures can be stored in various tiled formats
 */
enum class TextureTileMode : u8 {
    kLinear = 0,
    kTiled = 1,
};

/**
 * Texture untiling functions
 * Xbox 360 uses a complex tiling pattern for textures
 */
class TextureUntiler {
public:
    /**
     * Untile a 2D texture from Xbox 360 format to linear
     */
    static void untile_2d(const u8* src, u8* dst,
                          u32 width, u32 height, u32 bpp,
                          u32 block_width, u32 block_height);
    
    /**
     * Untile a 3D texture
     */
    static void untile_3d(const u8* src, u8* dst,
                          u32 width, u32 height, u32 depth, u32 bpp);
    
    /**
     * Calculate tiled offset for a pixel
     */
    static u32 get_tiled_offset_2d(u32 x, u32 y, u32 width, u32 bpp);
    
    /**
     * Calculate tiled offset for a 3D coordinate
     */
    static u32 get_tiled_offset_3d(u32 x, u32 y, u32 z, u32 width, u32 height, u32 bpp);
    
private:
    // Xbox 360 texture tiling uses a modified Morton curve (Z-order curve)
    static u32 morton_encode(u32 x, u32 y);
    static void morton_decode(u32 code, u32& x, u32& y);
};

} // namespace x360mu

