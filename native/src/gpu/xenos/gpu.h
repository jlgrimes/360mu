/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * ATI Xenos GPU emulation
 */

#pragma once

#include "x360mu/types.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace x360mu {

class Memory;
class VulkanBackend;
class ShaderTranslator;
class TextureCache;
class CommandProcessor;

/**
 * GPU configuration
 */
struct GpuConfig {
    bool use_vulkan = true;
    u32 resolution_scale = 1;
    bool enable_vsync = true;
    bool enable_async_shaders = true;
    std::string cache_path;
};

/**
 * Xenos register addresses (subset)
 */
namespace xenos_reg {
    // Ring buffer
    constexpr u32 CP_RB_BASE = 0x0700;
    constexpr u32 CP_RB_CNTL = 0x0701;
    constexpr u32 CP_RB_RPTR_ADDR = 0x070C;
    constexpr u32 CP_RB_RPTR = 0x070D;
    constexpr u32 CP_RB_WPTR = 0x070E;
    
    // Render state
    constexpr u32 RB_MODECONTROL = 0x2210;
    constexpr u32 RB_SURFACE_INFO = 0x2211;
    constexpr u32 RB_COLORCONTROL = 0x2212;
    constexpr u32 RB_COLOR_INFO = 0x2213;
    constexpr u32 RB_DEPTH_INFO = 0x2214;
    
    // Shader control
    constexpr u32 SQ_PROGRAM_CNTL = 0x2280;
    constexpr u32 SQ_VS_PROGRAM = 0x2200;
    constexpr u32 SQ_PS_PROGRAM = 0x2201;
    
    // Texture state
    constexpr u32 FETCH_CONST_BASE = 0x4800;
    
    // Vertex format
    constexpr u32 VGT_VERTEX_REUSE_BLOCK_CNTL = 0x2316;
    constexpr u32 VGT_OUTPUT_PATH_CNTL = 0x2300;
    
    // Viewport
    constexpr u32 PA_CL_VPORT_XSCALE = 0x2100;
    constexpr u32 PA_CL_VPORT_XOFFSET = 0x2101;
    constexpr u32 PA_CL_VPORT_YSCALE = 0x2102;
    constexpr u32 PA_CL_VPORT_YOFFSET = 0x2103;
    constexpr u32 PA_CL_VPORT_ZSCALE = 0x2104;
    constexpr u32 PA_CL_VPORT_ZOFFSET = 0x2105;
    
    // Screen scissor
    constexpr u32 PA_SC_SCREEN_SCISSOR_TL = 0x2080;
    constexpr u32 PA_SC_SCREEN_SCISSOR_BR = 0x2081;
}

/**
 * Xenos shader type
 */
enum class ShaderType {
    Vertex,
    Pixel
};

/**
 * Xenos texture format
 */
enum class TextureFormat : u32 {
    k_8 = 2,                    // DXT1
    k_1_5_5_5 = 3,
    k_5_6_5 = 4,
    k_6_5_5 = 5,
    k_8_8_8_8 = 6,
    k_2_10_10_10 = 7,
    k_8_A = 10,
    k_8_B = 11,
    k_8_8 = 12,
    k_Cr_Y1_Cb_Y0 = 13,
    k_Y1_Cr_Y0_Cb = 14,
    k_DXT1 = 18,
    k_DXT2_3 = 19,
    k_DXT4_5 = 20,
    k_CTX1 = 27,
    k_DXN = 28,
    k_16 = 36,
    k_16_16 = 37,
    k_16_16_16_16 = 38,
    k_16_FLOAT = 48,
    k_16_16_FLOAT = 49,
    k_16_16_16_16_FLOAT = 50,
    k_32_FLOAT = 54,
    k_32_32_FLOAT = 55,
    k_32_32_32_32_FLOAT = 56,
};

/**
 * Surface format for render targets
 */
enum class SurfaceFormat : u32 {
    k_8_8_8_8 = 0,
    k_8_8_8_8_GAMMA = 1,
    k_2_10_10_10 = 2,
    k_2_10_10_10_FLOAT = 3,
    k_16_16 = 4,
    k_16_16_16_16 = 5,
    k_16_16_FLOAT = 6,
    k_16_16_16_16_FLOAT = 7,
    k_32_FLOAT = 10,
    k_32_32_FLOAT = 11,
};

/**
 * Primitive type
 */
enum class PrimitiveType : u32 {
    PointList = 1,
    LineList = 2,
    LineStrip = 3,
    TriangleList = 4,
    TriangleFan = 5,
    TriangleStrip = 6,
    RectList = 8,
    QuadList = 13,
};

/**
 * Fetch constant (for vertex buffers and textures)
 */
struct FetchConstant {
    u32 data[6];
    
    // Vertex buffer interpretation
    GuestAddr vertex_buffer_address() const {
        return (data[0] & 0xFFFFFFFC);
    }
    
    u32 vertex_buffer_size() const {
        return ((data[1] >> 2) & 0x3FFFFF) + 1;
    }
    
    // Texture interpretation
    GuestAddr texture_address() const {
        return (data[0] & 0xFFFFFFFC);
    }
    
    u32 texture_width() const {
        return ((data[2] >> 22) & 0x1FFF) + 1;
    }
    
    u32 texture_height() const {
        return ((data[3] >> 6) & 0x1FFF) + 1;
    }
    
    TextureFormat texture_format() const {
        return static_cast<TextureFormat>((data[1] >> 7) & 0x3F);
    }
};

/**
 * GPU render state snapshot
 */
struct RenderState {
    // Viewport
    f32 viewport_x, viewport_y;
    f32 viewport_width, viewport_height;
    f32 viewport_z_min, viewport_z_max;
    
    // Scissor
    u32 scissor_left, scissor_top;
    u32 scissor_right, scissor_bottom;
    
    // Render target
    GuestAddr color_target_address;
    SurfaceFormat color_format;
    u32 color_pitch;
    
    GuestAddr depth_target_address;
    u32 depth_pitch;
    
    // Shaders
    GuestAddr vertex_shader_address;
    GuestAddr pixel_shader_address;
    
    // Fetch constants
    FetchConstant vertex_fetch[96];
    FetchConstant texture_fetch[32];
    
    // Blend state
    bool blend_enable;
    u32 blend_src, blend_dst;
    u32 blend_op;
    
    // Depth state
    bool depth_test;
    bool depth_write;
    u32 depth_func;
    
    // Rasterizer state
    u32 cull_mode;
    bool front_ccw;
    f32 polygon_offset;
};

/**
 * Xenos GPU emulator
 */
class Gpu {
public:
    Gpu();
    ~Gpu();
    
    /**
     * Initialize GPU subsystem
     */
    Status initialize(Memory* memory, const GpuConfig& config);
    
    /**
     * Shutdown and release resources
     */
    void shutdown();
    
    /**
     * Reset GPU state
     */
    void reset();
    
    /**
     * Set display surface (from Android)
     */
    void set_surface(void* native_window);
    
    /**
     * Handle surface resize
     */
    void resize(u32 width, u32 height);
    
    /**
     * Process command buffer
     * Called from CPU emulation loop
     */
    void process_commands();
    
    /**
     * Check if a frame is ready
     */
    bool frame_complete() const { return frame_complete_; }
    
    /**
     * Present the frame
     */
    void present();
    
    /**
     * Register read/write (for MMIO)
     */
    u32 read_register(u32 offset);
    void write_register(u32 offset, u32 value);
    
    // Statistics
    struct Stats {
        u64 frames;
        u64 draw_calls;
        u64 triangles;
        u64 shader_compiles;
        u64 texture_uploads;
    };
    Stats get_stats() const { return stats_; }
    
private:
    Memory* memory_ = nullptr;
    GpuConfig config_;
    
    // GPU registers (subset)
    std::array<u32, 0x10000> registers_;
    
    // Ring buffer state
    GuestAddr ring_buffer_base_;
    u32 ring_buffer_size_;
    u32 read_ptr_;
    u32 write_ptr_;
    
    // Current render state
    RenderState render_state_;
    
    // Frame state
    bool frame_complete_ = false;
    bool in_frame_ = false;
    
    // Subsystems
    std::unique_ptr<VulkanBackend> vulkan_;
    std::unique_ptr<ShaderTranslator> shader_translator_;
    std::unique_ptr<TextureCache> texture_cache_;
    std::unique_ptr<CommandProcessor> command_processor_;
    
    // Statistics
    Stats stats_{};
    
    // Command processing
    void execute_packet(u32 packet);
    void execute_type0(u32 packet);  // Register write
    void execute_type3(u32 packet);  // Draw/state commands
    
    // Draw commands
    void cmd_draw_indices(PrimitiveType type, u32 index_count, GuestAddr index_addr);
    void cmd_draw_auto(PrimitiveType type, u32 vertex_count);
    void cmd_resolve();
    
    // State updates
    void update_render_state();
    void update_shaders();
    void update_textures();
};

/**
 * Shader translator - converts Xenos shaders to SPIR-V
 */
class ShaderTranslator {
public:
    ShaderTranslator();
    ~ShaderTranslator();
    
    /**
     * Translate a Xenos shader to SPIR-V
     */
    std::vector<u32> translate(
        const void* microcode,
        u32 size,
        ShaderType type
    );
    
    /**
     * Get cached SPIR-V for shader hash
     */
    const std::vector<u32>* get_cached(u64 hash) const;
    
    /**
     * Cache translated shader
     */
    void cache(u64 hash, std::vector<u32> spirv);
    
private:
    std::unordered_map<u64, std::vector<u32>> cache_;
    
    // Translation helpers
    void decode_alu_instruction(u32 instr);
    void decode_fetch_instruction(u32 instr);
    void emit_spirv_header(std::vector<u32>& out);
    void emit_spirv_body(std::vector<u32>& out);
};

/**
 * Texture cache - manages GPU textures
 */
class TextureCache {
public:
    TextureCache();
    ~TextureCache();
    
    /**
     * Get or create texture for fetch constant
     */
    void* get_texture(const FetchConstant& fetch, Memory* memory);
    
    /**
     * Invalidate textures in address range
     */
    void invalidate(GuestAddr base, u64 size);
    
    /**
     * Clear all cached textures
     */
    void clear();
    
private:
    struct CachedTexture {
        u64 hash;
        GuestAddr address;
        u32 width, height;
        TextureFormat format;
        void* vulkan_texture;
    };
    
    std::vector<CachedTexture> textures_;
    
    // Format conversion
    void detile_texture(const void* src, void* dst, u32 width, u32 height, TextureFormat format);
    void convert_format(const void* src, void* dst, u32 width, u32 height, TextureFormat src_format);
};

} // namespace x360mu

