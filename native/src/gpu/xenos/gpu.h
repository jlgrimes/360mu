/**
 * 360μ - Xbox 360 Emulator for Android
 * 
 * ATI Xenos GPU emulation
 */

#pragma once

#include "x360mu/types.h"
#include "texture.h"  // For TextureFormat, TextureDimension
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
 * Xenos register addresses (complete list)
 */
namespace xenos_reg {
    // Ring buffer control
    constexpr u32 CP_RB_BASE = 0x0700;
    constexpr u32 CP_RB_CNTL = 0x0701;
    constexpr u32 CP_RB_RPTR_ADDR = 0x070C;
    constexpr u32 CP_RB_RPTR = 0x070D;
    constexpr u32 CP_RB_WPTR = 0x070E;
    constexpr u32 CP_RB_WPTR_DELAY = 0x070F;
    
    // Command processor
    constexpr u32 CP_ME_CNTL = 0x0000;
    constexpr u32 CP_ME_STATUS = 0x0001;
    constexpr u32 CP_RB_RPTR_WR = 0x0702;
    constexpr u32 CP_IB1_BASE = 0x0705;
    constexpr u32 CP_IB1_BUFSZ = 0x0706;
    constexpr u32 CP_IB2_BASE = 0x0707;
    constexpr u32 CP_IB2_BUFSZ = 0x0708;
    constexpr u32 CP_ST_BASE = 0x044D;
    constexpr u32 CP_ST_BUFSZ = 0x044E;
    
    // Render backend
    constexpr u32 RB_MODECONTROL = 0x2210;
    constexpr u32 RB_SURFACE_INFO = 0x2211;
    constexpr u32 RB_COLORCONTROL = 0x2212;
    constexpr u32 RB_COLOR_INFO = 0x2213;
    constexpr u32 RB_DEPTH_INFO = 0x2214;
    constexpr u32 RB_STENCILREFMASK = 0x2215;
    constexpr u32 RB_COLOR_MASK = 0x2216;
    constexpr u32 RB_BLENDCONTROL = 0x2217;
    constexpr u32 RB_COLOR1_INFO = 0x221A;
    constexpr u32 RB_COLOR2_INFO = 0x221B;
    constexpr u32 RB_COLOR3_INFO = 0x221C;
    constexpr u32 RB_ALPHA_REF = 0x221E;
    constexpr u32 RB_DEPTHCONTROL = 0x2230;
    constexpr u32 RB_BLEND_RED = 0x2231;
    constexpr u32 RB_BLEND_GREEN = 0x2232;
    constexpr u32 RB_BLEND_BLUE = 0x2233;
    constexpr u32 RB_BLEND_ALPHA = 0x2234;
    constexpr u32 RB_COPY_CONTROL = 0x2238;
    constexpr u32 RB_COPY_DEST_BASE = 0x2239;
    constexpr u32 RB_COPY_DEST_PITCH = 0x223A;
    constexpr u32 RB_COPY_DEST_INFO = 0x223B;
    constexpr u32 RB_SAMPLE_COUNT_CTL = 0x2243;
    constexpr u32 RB_EDRAM_INFO = 0x2244;
    
    // Shader sequencer
    constexpr u32 SQ_PROGRAM_CNTL = 0x2180;
    constexpr u32 SQ_CONTEXT_MISC = 0x2181;
    constexpr u32 SQ_INTERPOLATOR_CNTL = 0x2182;
    constexpr u32 SQ_VS_PROGRAM = 0x2200;
    constexpr u32 SQ_PS_PROGRAM = 0x2201;
    constexpr u32 SQ_VS_CONST = 0x2308;
    constexpr u32 SQ_PS_CONST = 0x2309;
    constexpr u32 SQ_CF_BOOLEANS = 0x2310;
    constexpr u32 SQ_CF_LOOP = 0x2311;
    constexpr u32 SQ_WRAPPING_0 = 0x2312;
    constexpr u32 SQ_WRAPPING_1 = 0x2313;
    
    // Texture state
    constexpr u32 FETCH_CONST_BASE = 0x4800;
    constexpr u32 SQ_TEX_SAMPLER_0 = 0x5000;
    
    // Vertex generation
    constexpr u32 VGT_MAX_VTX_INDX = 0x2300;
    constexpr u32 VGT_MIN_VTX_INDX = 0x2301;
    constexpr u32 VGT_INDX_OFFSET = 0x2302;
    constexpr u32 VGT_OUTPUT_PATH_CNTL = 0x2303;
    constexpr u32 VGT_HOS_CNTL = 0x2304;
    constexpr u32 VGT_HOS_MAX_TESS_LEVEL = 0x2305;
    constexpr u32 VGT_HOS_MIN_TESS_LEVEL = 0x2306;
    constexpr u32 VGT_HOS_REUSE_DEPTH = 0x2307;
    constexpr u32 VGT_GROUP_PRIM_TYPE = 0x2308;
    constexpr u32 VGT_GROUP_FIRST_DECR = 0x2309;
    constexpr u32 VGT_GROUP_DECR = 0x230A;
    constexpr u32 VGT_GROUP_VECT_0_CNTL = 0x230B;
    constexpr u32 VGT_GROUP_VECT_1_CNTL = 0x230C;
    constexpr u32 VGT_GROUP_VECT_0_FMT_CNTL = 0x230D;
    constexpr u32 VGT_GROUP_VECT_1_FMT_CNTL = 0x230E;
    constexpr u32 VGT_DRAW_INITIATOR = 0x2314;
    constexpr u32 VGT_IMMED_DATA = 0x2315;
    constexpr u32 VGT_VERTEX_REUSE_BLOCK_CNTL = 0x2316;
    constexpr u32 VGT_OUT_DEALLOC_CNTL = 0x2317;
    constexpr u32 VGT_MULTI_PRIM_IB_RESET_INDX = 0x2318;
    
    // Viewport/clip
    constexpr u32 PA_CL_VTE_CNTL = 0x2006;
    constexpr u32 PA_CL_VPORT_XSCALE = 0x2100;
    constexpr u32 PA_CL_VPORT_XOFFSET = 0x2101;
    constexpr u32 PA_CL_VPORT_YSCALE = 0x2102;
    constexpr u32 PA_CL_VPORT_YOFFSET = 0x2103;
    constexpr u32 PA_CL_VPORT_ZSCALE = 0x2104;
    constexpr u32 PA_CL_VPORT_ZOFFSET = 0x2105;
    constexpr u32 PA_CL_CLIP_CNTL = 0x2110;
    constexpr u32 PA_CL_GB_VERT_CLIP_ADJ = 0x2120;
    constexpr u32 PA_CL_GB_VERT_DISC_ADJ = 0x2121;
    constexpr u32 PA_CL_GB_HORZ_CLIP_ADJ = 0x2122;
    constexpr u32 PA_CL_GB_HORZ_DISC_ADJ = 0x2123;
    
    // Scissor
    constexpr u32 PA_SC_SCREEN_SCISSOR_TL = 0x2080;
    constexpr u32 PA_SC_SCREEN_SCISSOR_BR = 0x2081;
    constexpr u32 PA_SC_WINDOW_OFFSET = 0x2082;
    constexpr u32 PA_SC_WINDOW_SCISSOR_TL = 0x2083;
    constexpr u32 PA_SC_WINDOW_SCISSOR_BR = 0x2084;
    constexpr u32 PA_SC_CLIPRECT_RULE = 0x2085;
    constexpr u32 PA_SC_CLIPRECT_0_TL = 0x2086;
    constexpr u32 PA_SC_CLIPRECT_0_BR = 0x2087;
    constexpr u32 PA_SC_VIZ_QUERY = 0x20C0;
    
    // Setup unit  
    constexpr u32 PA_SU_SC_MODE_CNTL = 0x2280;
    constexpr u32 PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2281;
    constexpr u32 PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2282;
    constexpr u32 PA_SU_POLY_OFFSET_BACK_SCALE = 0x2283;
    constexpr u32 PA_SU_POLY_OFFSET_BACK_OFFSET = 0x2284;
    constexpr u32 PA_SU_POINT_SIZE = 0x2285;
    constexpr u32 PA_SU_POINT_MINMAX = 0x2286;
    constexpr u32 PA_SU_LINE_CNTL = 0x2287;
    constexpr u32 PA_SU_VTX_CNTL = 0x2288;
    constexpr u32 PA_SU_PERFCOUNTER0_SELECT = 0x2290;
    
    // eDRAM
    constexpr u32 RB_EDRAM_BASE = 0x0040;
    constexpr u32 RB_BC_CONTROL = 0x0041;
}

/**
 * Xenos shader type
 */
enum class ShaderType {
    Vertex,
    Pixel
};

// TextureFormat is defined in texture.h

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
    k_5_6_5 = 8,
    k_6_5_5 = 9,
    k_32_FLOAT = 10,
    k_32_32_FLOAT = 11,
    k_32_32_32_32_FLOAT = 12,
    k_1_5_5_5 = 14,
    k_4_4_4_4 = 15,
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

// Forward declarations - real implementations in shader_translator.h and texture.h
// class ShaderTranslator - see shader_translator.h
// class TextureCache - see texture.h

} // namespace x360mu

